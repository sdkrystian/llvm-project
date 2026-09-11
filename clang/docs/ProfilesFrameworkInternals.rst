================================
C++ Profiles Framework Internals
================================

.. contents::
   :depth: 2
   :local:


This document describes the implementation of the C++ Profiles framework
(`P3589R2 <https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3589r2.pdf>`_)
for Clang contributors -- in particular, how to add a new profile.  For
user-facing documentation of the feature, see :doc:`ProfilesFramework`.


Architecture
============

The framework is profile-agnostic.  Profile names are opaque strings and
there is no central registry: a profile is "enforced" simply because the user
wrote ``[[profiles::enforce(name)]]``, and each rule within a profile is just
a string identifier that ``[[profiles::suppress(name, rule: "...")]]`` can
target.  ``SemaProfiles`` owns all the bookkeeping -- attribute parsing and
placement checking, enforcement tracking, suppression scoping, template
instantiation, module propagation, and PCH/BMI serialization -- so a profile
implementation consists only of its diagnostics plus calls to the framework
at its semantic check sites.

The enforced-profile list itself is stored on the ``ASTContext`` (recorded
through ``addEnforcedProfile``; queried through ``isProfileEnforced`` /
``getProfileEnforcement`` / ``enforced_profiles``) rather than on ``Sema``:
the ASTReader restores a PCH's ``ENFORCED_PROFILES`` records directly into
it, so a consumer that never sees a Sema -- e.g. code generation directly
from an AST file -- observes the same list.  The list answers the
whole-unit questions: what the unit advertises and ``[[profiles::require]]``
validates, designator mismatches, redeclaration compatibility, and the "is
any profile of this table enforced" gates of the post-parse dispatchers.
*Where* a rule is enforced is the diagnostics engine's question:
``SemaProfiles::addProfileEnforcement`` records the enforcement into the
list and maps the profile's rule diagnostics to errors from the attribute
to the end of the translation unit, and ``ASTContext::isProfileRuleActiveAt``
-- the enforcement half of every violation gate -- asks whether the rule's
diagnostic is mapped at the check site and the site is not
system-header-exempt (``isProfileExemptSystemHeaderLoc``).  See
`Enforcement and Suppression State`_.

``-fprofiles-enforce=`` is the second Sema-free seed: the ``ASTContext``
constructor records each name in ``LangOptions::ProfilesEnforce`` with an
invalid location and maps its rule diagnostics in the initial diagnostic
state, which covers the whole translation unit.  The constructor is the one
point every consumer passes through -- a source compile, a PCH or BMI
build, code generation from an AST file.  ``CompilerInvocation`` validates
the names against the profile-name production only
(``profiles::isValidProfileName``), sorts and dedups the list, and sets
``LangOptions::Profiles`` when it is non-empty.

Profile-rule diagnostics are defined with the ``ProfileRule`` diagnostic
class rather than plain ``Error``.  A rule diagnostic is a *latent*
``Warning``-class diagnostic (``Latent`` in ``Diagnostic.td``): it belongs to
a diagnostic group (an ``Error`` cannot), is ignored until an enforcement or
its group maps it, is left alone by ``-Weverything``, and once mapped to an
error is a genuine error that ``-w`` does not silence.  The ``Warning``
class also marks it SFINAE-suppressed:
violations do not count as substitution failures and cannot change overload
resolution, but selected specializations replay them when actually used.
Every rule has its own group, ``profile-<profile>-<rule>``, nested in the
profile's group ``profile-<profile>`` under ``-Wprofiles``; a rule-less
diagnostic sits directly in the profile's group.  The names are derived by
``profiles::getProfileDiagGroupName``.  The framework passes the profile
name as ``%0``:

.. code-block:: text

   def err_profile_type_cast_reinterpret : ProfileRule<
     "'reinterpret_cast' is unsafe under profile '%0'",
     ProfileTestTypeCastReinterpretCast>;

There are five implementation patterns, keyed on when -- and for pattern 5,
how -- the rule is checked.


Adding a Profile
================

Every profile follows the same recipe:

1. Pick the pattern that matches when the rule can be decided: a single
   semantic entry point (pattern 1), whole-function analysis (pattern 2),
   class or constructor finalization (patterns 3 and 4), or a runtime check
   (pattern 5).
2. Define the diagnostic: a ``ProfileRule`` in ``DiagnosticSemaKinds.td``
   for compile-time rules, or a ``Trap`` in ``DiagnosticTrapKinds.td`` for
   runtime-checked rules, in the rule's diagnostic group
   (``DiagnosticGroups.td``; a new profile adds its group tree there, under
   ``Profiles``).
3. Add the check: a ``shouldEmitProfileViolation`` gate before the diagnostic,
   or an ``EmitProfileRuntimeCheck`` call, at the check site (patterns 1 and
   5), or a row in the analysis's opt-in table (patterns 2-4).
4. Add tests; a test-only profile must be named under ``test::`` (see `Test
   Profiles`_).

Enforcement, suppression, module propagation, and serialization then work
without further profile-specific code.


Pattern 1: Parse-Time Check Sites
=================================

For a rule checkable at a single semantic entry point, the entire profile
implementation is a gate and a diagnostic at that site:

.. code-block:: c++

   if (Profiles().shouldEmitProfileViolation(diag::err_my_profile_rule, Loc))
     Diag(Loc, diag::err_my_profile_rule) << "my::profile";

The gate is ``SemaProfiles::shouldEmitProfileViolation``: the
enforce/exempt/suppress rung ``ASTContext::isProfileRuleActiveAt`` (which
CodeGen's runtime checks run as is) plus the parse-time rungs -- a templated
declaration, an unevaluated context, and a discarded statement never fire.
Every pattern's check site goes through that one gate.  ``test::type_cast``
is the in-tree example.

Inside a template, a profile rule is checked on phase-7 entities only where
it depends on a declaration, a completed class or constructor, or a function
body: those checks pass their ``Decl`` (or run from the post-parse dispatch)
and fire once per instantiation, never on a template pattern -- the gate's
templated-declaration rung and the CFG dispatch's dependent-context skip
enforce it.  An expression-level check has no such anchor: it fires on the
pattern for an operand that is not instantiation-dependent (TreeTransform
reuses such a node, so an instantiation could not re-check it) and once per
instantiation for a dependent operand, which is rebuilt there; each such
check site defers in a dependent context from its own wrapper.  A
non-dependent operand that is rebuilt anyway -- a call, or an operand that
names a parameter or a sibling that changed -- is reported again at that
instantiation, with the ``in instantiation of ...`` note; an operand the
instantiation reuses is not.  The pattern-time half is the one place the
framework departs from P3589R2 §1.1's "as-if after phase 7" model, in
exchange for reuse-proof diagnostics.  Under ``-fdelayed-template-parsing``
the body of a never-instantiated template is never parsed, so pattern-time
diagnosis does not occur in that mode.  ``test::type_cast`` follows the
expression policy exactly: a ``reinterpret_cast`` of a non-dependent operand
fires at the definition, of a dependent operand once per instantiation.
An expression check whose source names flow-tracked storage is the CFG
pass's and follows the per-instantiation rule ("Flow-Tracked Storage
(std::init)" below).


Pattern 2: Post-Parse / CFG-Based
=================================

For a rule that needs whole-function analysis.  Each post-parse analysis owns
a small opt-in table of the profiles that ride it, one row per profile
(profile name, diagnostic); the framework never learns the profile's name.
``test::uninit_read`` is the in-tree example:

.. code-block:: c++

   constexpr CFGProfileEntry CFGProfiles[] = {
       {"my::profile", diag::err_my_profile_rule},
   };

The analysis's diagnostic reporter walks the table calling the single gate
as ``shouldEmitProfileViolation(DiagID, Loc, /*D=*/nullptr,
/*PostParse=*/true)`` per use site, emitting the entry's diagnostic (and
skipping the default warning) when it returns true; the use site's location
finds the suppression dominion it lies in, so the post-parse site needs no
suppression sources of its own.

A row may additionally install up to three optional hooks; a null column
costs nothing:

- ``VarExempt`` -- a per-variable exemption consulted once per variable,
  before either reporter arm; an exempting row takes no part in that
  variable's diagnosis.  The hook must not emit.
- ``ConfigureCFG`` -- adds the row's extra always-add statement classes to
  the CFG build options; both analysis paths apply it, so they build the
  same CFG shape.
- ``ExtraPass`` -- a whole-function pass run on both analysis paths after
  the uninitialized-variables reporter has flushed.  It owns its rules and
  diagnostics, gating each check site through
  ``shouldEmitProfileViolation``, and receives its row so the profile's
  identity keeps flowing from the table.

A row whose diagnostic is ``0`` takes no part in the uninitialized-read
reporter and rides the analysis for its hooks alone -- an ``ExtraPass`` that
owns every diagnostic of its profile.

An ``ExtraPass`` must tolerate both CFG shapes the dispatch points can
build: the non-linearized shape (the base always-add classes plus whatever
the enforced rows' ``ConfigureCFG`` hooks add) and the fully linearized
shape (another analysis in the same run forced ``setAllAlwaysAdd``, which
adds arbitrary extra elements).  A pass's extraction arms may therefore only
match always-add classes or unconditional CFG elements: an element class
that is neither is present in one shape and absent in the other, so matching
it would produce diagnostics on one path only.  ``test::cfg_hooks`` is the
in-tree pilot for the hook columns.

Profile rules are errors, so the pass must also run where the warning
pipeline is skipped.  ``AnalysisBasedWarnings::hasEnforcedCFGProfile()``
gates those paths: after an uncompilable TU error, and when warnings are
disabled for the declaration (``-w``, or a system-header declaration under
``-fno-profiles-exempt-system-headers``), the per-function dispatch runs
``runProfileOnlyCFGAnalysis`` instead of skipping -- the same analysis with
the reporter in ``ProfileOnly`` mode, so an ordinary warning the error or
flag is meant to silence cannot resurface.  ``Sema::ActOnFinishFunctionBody``
likewise keeps dispatching per-function analysis after a TU error when such
a profile is enforced; without this, the first error would disable the
profile for every later function.

``std::init`` installs two hook columns: ``VarExempt`` exempts ``std::byte``
variables (P4222R2 §4) and ``ExtraPass`` runs the member read-before-init
engine (``runStdInitMemberReadChecks``: one ``TrackedStorage`` entity table,
``extractStdInitEvents``, ``runDefiniteAssignment``, one report, over a CFG
of its own built with exception edges and every expression as an element,
so the shape contract above does not bind it).  The row threads the profile's
*identity* -- its name and its uninitialized-read diagnostic -- through the
engine and its reporter, not its semantics: the tracked-member vocabulary
the passes implement
(``[[uninit]]`` scalar members of constructor-less aggregates) and the
member diagnostic are ``std::init``'s own.


Patterns 3 and 4: Class and Constructor Finalization
====================================================

For rules that run once per completed class definition (pattern 3,
``test::class_final``) or once per user-defined constructor with its complete
member-initializer list (pattern 4, ``test::ctor_final``).  Both share one
dispatcher and one per-pass table shape:

.. code-block:: c++

   constexpr FinalizationProfile<CXXRecordDecl> ClassFinalizationProfiles[] = {
       {"my::profile", &runMyProfileCallback},
   };

A profile may register several rows in one table -- one per independent
rule -- and the dispatcher runs every row whose profile is enforced.

The class hook runs from the single function every class-completion path
funnels through (parsing, template instantiation, lambda completion); the
constructor hook runs from the three functions every user-provided
constructor definition funnels through -- ``ActOnMemInitializers``,
``ActOnDefaultCtorInitializers``, and ``SetDeclDefaulted`` (an out-of-line
``= default``) -- including instantiation.  The per-pattern entry points
filter out dependent entities (the hooks re-fire on each instantiation),
invalid ones, lambdas (pattern 3), and delegating constructors (pattern 4)
before the shared dispatcher runs the enforced callbacks; a filter that is
one profile's policy rather than the pattern's contract (``std::init``
exempting defaulted copy/move constructors, which initialize member-wise
while writing no initializer) lives in that profile's callback.  Each
callback gates its diagnostics on ``shouldEmitProfileViolation`` with the
finalized declaration and its location.

The split between the two patterns matters: class finalization runs *before
any constructor body or member-initializer list has been parsed*, so a
pattern-3 callback must not inspect a constructor's ``inits()``.  Rules that
depend on what a constructor initializes belong on pattern 4; rules that need
flow analysis belong on pattern 2.


Pattern 5: Runtime-Checked Rules
================================

For a rule whose enforcement means emitting a *runtime check* during code
generation rather than a compile-time diagnostic -- P3589R2 sanctions
dynamic semantics for profiles (a profile "may have an effect on the runtime
behavior of a program", e.g. bound checking, §1.1/§2.2.2).  The entire
profile implementation is one call at the check site, naming the profile
and a trap diagnostic (``Trap`` class, ``DiagnosticTrapKinds.td``, category
"C++ Profiles") whose group identifies the rule, and passing the check's
"no violation" predicate as a lazily-invoked builder.  The site
guards on its own applicability conditions; the integer div/rem site is the
in-tree pilot:

.. code-block:: c++

   if (Ops.Ty->isIntegerType() && Ops.mayHaveIntegerDivisionByZero())
     CGF.EmitProfileRuntimeCheck(
         "test::arith", diag::trap_profile_zero_divide, Ops.E->getExprLoc(),
         [&] {
           return Builder.CreateICmpNE(
               Ops.RHS, llvm::Constant::getNullValue(Ops.RHS->getType()));
         });

``EmitProfileRuntimeCheck`` checks that the rule is enforced and not
suppressed at the check site and that the site is not in an exempt system
header (``ASTContext::isProfileRuleActiveAt``: the trap diagnostic's mapping
at that location, so a body deserialized from an AST file is decided by the
state its own unit recorded, and an NSDMI or default argument by its
member's or parameter's dominion wherever it is emitted).  Only when the
check is active does it invoke the builder for the predicate, so an inactive
site emits no IR, and then emits a conditional branch to a trap block
(``SanitizerHandler::ProfileViolation``).  Unevaluated operands and
discarded statements are never emitted at all, so the Sema-side gates for
those contexts need no CodeGen counterpart.

Failure semantics are trap-only: ``llvm.ubsantrap`` with the handler's own
immediate, no handler call, no runtime library.  That is forced rather than
merely chosen -- enforcement is declared *in source*, so the driver cannot
see it and no runtime support library can be auto-linked.  Under the default
``-fsanitize-debug-trap-reasons=detailed`` with debug info enabled, the
trap's debug location carries the rule's trap diagnostic naming the
violated profile.  External decoders of ``ubsantrap`` immediates (e.g.
LLDB) do not know the new immediate and label the trap "Undefined Behavior
Sanitizer" when trap reasons are off or basic; the detailed trap-reason
string compensates.  At ``-O0`` every check site gets its own trap instruction,
keeping locations and reasons exact; optimized builds coalesce the traps of
one handler kind and merge their locations, like UBSan's trap mode.

The pilot's zero-divisor check deliberately mirrors UBSan's blind spots: GCC
vector-extension integer division and ``_Complex int`` division are not
checked.


Enforcement and Suppression State
=================================

An enforcement's dominion (P3589R2 [decl.attr.enforce]p4: the tokens from
the attribute to the end of the translation unit) is represented as
diagnostic state: ``SemaProfiles::addProfileEnforcement`` maps every
diagnostic of the profile's group (`Diagnostic Groups`_ in
:doc:`ProfilesFramework`; ``profiles::getProfileDiagGroupName``) to an
error from the attribute's expansion location on, and
``ASTContext::isProfileRuleActiveAt`` reads the mapping back at each check
site with ``DiagnosticsEngine::isIgnored``.  The mapping is installed with
``DiagnosticsEngine::setDiagnosticMappingsFrom``, which takes effect no
earlier than the current diagnostic state -- a ``#pragma clang diagnostic``
lexed as the parser's lookahead behind the enforce declaration is already
recorded when the attribute is processed -- and also reaches every state a
``#pragma clang diagnostic push`` saved, so a later ``pop`` cannot restore
an unenforced state.  ``-fprofiles-enforce=`` maps the group in the initial
state instead (the ``ASTContext`` constructor, a command-line mapping).  A
profile the implementation does not know has no group, and mapping it does
nothing, which is the specified behavior of an unknown profile.  A check
site with an invalid location sees the initial state, so it is checked
under command-line enforcement only.

The engine's own rungs then apply: the rule diagnostics are latent, so
``-w`` keeps an enforced rule, ``-Weverything`` in either form leaves the
rules alone, and ``-Wprofile-...`` and the diagnostic pragmas move a rule's
severity like any other diagnostic's.  Nothing is mapped until an
enforcement or option asks for it, so a translation unit that enforces no
profile carries no profile state, in its diagnostic pragma record included.
Enforcement therefore
travels with the diagnostic state: through a PCH or preamble as the state
the main file continues from, through a BMI compiled to object code as the
unit's own file transitions, and through an imported module as transitions
installed for the module's files -- so module code is checked under the
module's enforcement wherever it is instantiated or emitted, never under
the importer's, and importer code never under the module's.  A module-map
module (``-fmodules``) has no initial state of its own and takes the
importer's, so the importer's ``-fprofiles-enforce=`` reaches it; the
diagnostic-option validation an implicit module's import performs skips
latent diagnostics, since an enforcement written in source is not a
``-Werror`` option the module build had to share.

A suppression's dominion ([decl.attr.suppress]p3: the attribute's tokens
through the last token of the declaration or statement it appertains to) is
recorded the same way, by ``Parser::ProfileSuppressionDominion`` guards.
``SemaProfiles::beginSuppression`` maps every diagnostic of the attribute's
rule group (the profile's group for a rule-less attribute) to ignored from
the attribute's expansion location on, remembering the mappings it
replaced, and ``endSuppression`` restores them from the end of the last
consumed token on -- in the states a ``#pragma clang diagnostic push``
inside the dominion saved as well, so a later ``pop`` does not revive the
suppression.  A guard whose attributes suppress nothing new (an enclosing
construct already suppresses the rule) records nothing, so the doubled
guards of a block-scope declaration cost nothing.  Because the state is
keyed on positions, a token-cached body, default argument, or member
initializer parsed after its class, and a template instantiated at any
later point, find the dominion their tokens lie in without re-establishing
anything: ``setDiagnosticMappingsAt`` inserts a transition when the lexer
has already passed the location.

A construct whose attributes reach its Decl only after its leading tokens
(a declarator-id attribute) is covered by two guards that stitch together:
the declarator's guard, from the declarator-id to the declarator's end, and
a Decl-keyed guard around the initializer, default argument, mem-initializers
and body, or lambda body, beginning at the current token -- beginning at the
declarator-id again would find the rule already ignored there and record
nothing, while the first guard has already restored the mapping at the
declarator's end.  Guards are installed at every parser site that parses a
construct carrying ``[[profiles::suppress]]``: the prefix attributes of a
statement, a declaration (at namespace, block, class, and template scope),
a parameter, a condition, and an enumerator; the class, enum, and namespace
heads (from the attributes themselves); a declarator's declarator-id
attributes; and the Decl-keyed continuations, at each point a definition's
body is parsed or token-cached (``ParseFunctionDefinition`` and its
``-fdelayed-template-parsing`` branch, ``ParseCXXInlineMethodDef``).
clang/test/SemaCXX/safety-profile-suppress-coverage.cpp is the per-context
regression net for this contract.  Template instantiation, late parsing, and
code generation consult no suppression state of their own: an instantiated
body, default argument, or member initializer keeps the pattern's locations,
and a check site emitted or analyzed anywhere is judged by the dominion its
tokens lie in -- an NSDMI or default argument emitted at a use site by its
member's or parameter's, never the use site's.
``ProfilesSuppressAttr`` is not inherited by redeclarations
(``mergeDeclAttribute`` skips it), so a suppression written on a previous
declaration does not appear on the definition in the AST either.

Where the recorded dominion departs from the construct's token range:

- Positions are expansion sites, so a macro that expands to a suppressed
  declaration or statement followed by further tokens suppresses the whole
  expansion (the one under-check corner).
- A ``#pragma clang diagnostic`` inside a token-cached construct (an inline
  member function body, a class-scope default argument or member
  initializer, a ``-fdelayed-template-parsing`` body) is recorded before the
  construct's guard runs, and the state it establishes does not carry the
  suppression: the tokens between the pragma and the construct's end are
  checked.
- A ``#pragma clang diagnostic pop`` inside a suppressed construct, of a
  push before it, restores the unsuppressed state for the rest of the
  construct.
- A check site with an invalid location sees the initial state: no
  suppression, and command-line enforcement only.

Each of the last three is an over-check (a check that suppression fails to
remove), never a missing check.


Modules and Serialization
=========================

``[[profiles::enforce]]`` on a module interface declaration *advertises* the
enforced designators: they are recorded on
``Module::AdvertisedProfiles``, which is what
``[[profiles::require]]`` on an import validates against.  A header unit
advertises the same way from the empty-declaration form P3589R2 prescribes
for headers.  A non-partition implementation unit inherits the
interface's enforcements through its implicit import of the primary
interface.  A partition implementation unit does not implicitly import the
interface, whose BMI is normally built later, so inheritance there is
best-effort: enforcements are inherited only when the interface's BMI is
already resident, and it is never force-loaded nor its absence diagnosed --
a missed diagnostic, never a wrong one.
``[[profiles::enforce]]`` on a *non-interface* module-declaration is recorded
only translation-unit-locally and is invisible to importers.  A designator
written on an implementation unit's module-declaration is checked against the
inherited enforcements, so a conflicting one is reported at that attribute.

Serialization is automatic for every profile, through four records.
The TU's enforcement list is written to every AST file as
``ENFORCED_PROFILES`` records, and the reader restores it only when the file
is the compilation's own textual prefix or main input -- a PCH, a preamble,
or the AST file being code-generated (a module unit compiled from its BMI)
-- never from an import, whatever its module kind, so an importer's list --
what it advertises, requires, and checks redeclarations against -- is its
own.  The rule mappings travel separately, in the engine's own
``DIAG_PRAGMA_MAPPINGS`` record (`Enforcement and Suppression State`_).  Command-line
enforcements are written with the rest and dedup on restore against the
consumer's own constructor seed.  ``LangOptions::ProfilesEnforce``
is a Compatible language option, carried in ``LANGUAGE_OPTIONS`` and hashed
into the implicit-module signature: a PCH, a preamble, or a BMI compiled to
object code must have been built under the same list as the compilation
consuming it (a mismatch is the usual language-option error), while an
imported explicit module may differ.
``Module::AdvertisedProfiles`` is written to a BMI as
``SUBMODULE_ENFORCED_PROFILES`` records within each submodule block.
``Module::DominionProfiles`` -- the names of every profile the unit enforced
anywhere -- is written for the module being built as
``SUBMODULE_DOMINION_PROFILES`` records, one name each, from the TU's
enforcement list at the end of the unit; the redeclaration-compatibility
check below reads it, ``[[profiles::require]]`` never does.
``PROFILES_TU_HAS_NONEMPTY_DECL`` records whether a PCH contributed a
non-empty top-level declaration, so the empty-declaration placement check
works across a PCH boundary without deserializing the PCH's declarations; it
is written for PCHs only and restored from PCHs and preambles only, since an
importer's placement state is its own.


Redeclaration Compatibility
===========================

P3589R2 [decl.attr.enforce]p5 requires a declaration and its redeclarations
to appear in the dominions of mutually compatible profiles -- an
*enforcement's* dominion, the region of the program an
``[[profiles::enforce]]`` covers, as distinct from a suppression's dominion
above.
``checkRedeclarationProfileCompatibility`` runs from the module-level
redeclaration funnel and checks the rule symmetrically in both directions.
It is a framework rule: a plain error, not suppressible with
``[[profiles::suppress]]``, and diagnose-only (the redeclaration still
merges).  Two profiles are compatible if they have the same name (designator
arguments configure a profile without changing its identity) or if both are
standard ``std::``-prefixed profiles.

The previous declaration's dominion is its translation unit's recorded
enforcement set -- every profile enforced there by attribute, by
``-fprofiles-enforce=``, or by inheritance -- carried on the module as
``Module::DominionProfiles`` (``SUBMODULE_DOMINION_PROFILES``).  It is
distinct from the advertised set that ``[[profiles::require]]`` reads.
Two cases are skipped rather than guessed at (a missed diagnostic, never a
wrong one): a declaration in an explicit global module fragment (an
enforcement written on the module-declaration does not cover it while one
written before the fragment's declarations does, and the recorded set cannot
tell the two apart), and a previous declaration from the same module family
(this unit inherits the interface's enforcements, and an implementation
unit's additional TU-local enforcements do not obligate the interface).  A
previous declaration owned by a module-map module (a Clang header module,
``-fmodules``) is not checked at all: that is textual inclusion wearing an
AST file, not a separate TU in the standard's model, so it has no dominion of
its own -- header units, which are real TUs with recorded designators, stay
checked.  A textual or PCH previous declaration shares the current TU's
dominion and is not checked; implicit template instantiations are exempt.
A previous declaration covered by the system-header exemption stopgap is
skipped as well -- a header unit built from a system header has no recorded
designators, and redeclaring or specializing its entities must not draw the
error while every other check exempts that code
(``-fno-profiles-exempt-system-headers`` restores spec-exact checking).
Profiles gated off in this compilation (``test::`` names without
``-fprofiles-test-profiles``) are inert on either side of the comparison.


Test Profiles
=============

The six built-in ``test::`` profiles exist only to exercise the framework in
the test suite.  They are gated on the ``-cc1``-only
``-fprofiles-test-profiles`` flag: under ``-fprofiles`` alone their
designators are still parsed, recorded, and exported across modules, but
``isProfileEnforced`` reports any ``test::``-prefixed profile as not
enforced, so no ``test::`` rule ever fires.  Because that gate keys on the
``test::`` prefix, a new test-only profile must also live under ``test::``.

- ``test::type_cast`` -- pattern 1; diagnoses ``reinterpret_cast<>`` (the
  keyword form only).
- ``test::uninit_read`` -- pattern 2; rides the existing CFG
  uninitialized-variables analysis.
- ``test::cfg_hooks`` -- pattern 2; exercises the optional hook columns: its
  ``VarExempt`` hook exempts variables named with an ``exempt`` prefix from
  its uninitialized-read rule, its ``ConfigureCFG`` hook always-adds lambda
  expressions, and its ``ExtraPass`` diagnoses every lambda-expression CFG
  element under its ``lambda`` rule.
- ``test::class_final`` -- pattern 3; fires on completion of every non-lambda
  class, on instantiations rather than dependent patterns.
- ``test::ctor_final`` -- pattern 4; fires once per user-defined,
  non-delegating constructor.
- ``test::arith`` -- pattern 5; its ``zero_divide`` rule emits a runtime
  zero-divisor trap check on integer division and remainder.

The names ``test::other``, ``test::bounds``, ``test::new_profile``, and
``test::not_enforced`` are deliberately *not* implemented and appear in
negative tests as "some other profile" stand-ins; adding a real profile under
any of them would invalidate those tests.


The std::init Implementation Map
================================

``std::init`` (documented in :doc:`ProfilesFramework`) uses all four
patterns.  Its rules map to mechanisms as follows:

.. list-table::
   :header-rows: 1
   :widths: 24 12 64

   * - Rule
     - Pattern
     - Primary entry points
   * - ``uninit_read``
     - 2 and 1
     - ``CFGProfiles`` row for local variables; ``runStdInitMemberReadChecks``
       for the same row through its ``ExtraPass`` hook: one
       ``TrackedStorage`` entity table (the current object's and tracked
       locals' ``[[uninit]]`` scalar members), one ``extractStdInitEvents``
       loop whose arms resolve every lvalue through
       ``TrackedStorage::resolve`` and distribute comma, conditional, and GNU
       ``?:`` shapes to their leaves, and one definite-assignment run over a
       CFG of its own, built with exception edges over a fully linearized
       shape (the shared CFG the other analyses see is untouched); the
       ``CallExpr`` arm turns a ``[[now_init]]`` call into a ``Gen`` bit for
       the storage bound to the callee's marked parameters (P4222R2 §6.2)
       and a ``[[now_uninit]]`` call into a ``Kill`` that clears the
       assigned bit again (a local's escape credit already subsumes
       ``[[now_init]]``).  A "destroyed here"
       note on the read is deferred -- a kill witness would have to be
       carried per bit through the meet and both engine replays, roughly
       doubling the engine state and touching the enqueue-skip invariant);
       ``checkInitProfileReadThrough`` at the lvalue-to-rvalue chokepoint,
       plus compound-assignment and increment/decrement hooks, for a
       glvalue with no flow-tracked leaf; the ReadThrough sites of
       ``extractStdInitEvents`` (``judgeAccessSite``, suppressed by
       ``May``) otherwise
   * - ``uninit_decl``
     - 1
     - ``checkInitProfileUninitDecl``
   * - ``uninit_with_initializer``
     - 1
     - ``checkInitProfileUninitWithInitializer``
   * - ``static_runtime_init``
     - 1
     - ``checkInitProfileStaticRuntimeInit``
   * - ``static_marker``
     - 1
     - ``checkInitProfileStaticMarker``, hosted at every point a
       static-duration declaration's initializer state becomes final: both
       arms of ``ActOnUninitializedDecl`` (definitions and non-defining
       declarations) and the instantiated in-class static data member arm of
       ``InstantiateVariableInitializer``
   * - ``union_marker``, ``pointer_marker``
     - attribute handler (enforcement-gated)
     - ``checkInitProfileMarkerPlacement``
   * - ``ctor_uninit_member``
     - 4; 3 for inherited constructors
     - ``ConstructorFinalizationProfiles`` row for user-provided
       constructors; ``runStdInitInheritedCtorUninitMemberCallback`` (a
       second ``std::init`` ``ClassFinalizationProfiles`` row) checks the
       members and non-nominated bases an inherited constructor leaves
       uninitialized, once per class at the ``using``-declaration
   * - ``ref_to_uninit``
     - 2 for flow-tracked sources, 1 otherwise
     - ``checkInitProfileBinding``, one funnel keyed on ``InitBindingKind``
       for every binding site (variable and member initialization, call
       arguments, returns, aggregate elements, pointer assignments, throws,
       new-initializers, variadic arguments, captures, object arguments);
       every entity-driven binding enters it from
       ``InitializationSequence::Perform``, beside
       ``checkInitializerLifetime``, keyed on the ``InitializedEntity``;
       a source with a flow-tracked leaf (``isFlowTrackedLeaf``) is left to
       the binding arms of ``extractStdInitEvents``, which derive the same
       kinds from the CFG's elements and judge each site against the flow
       state (``judgeBindingSite``); ``classifyPointerGlvalue`` judges a
       reference-to-pointer binding by the pointer's value (a read-only
       alias) or not at all (a mutable alias); a defaulted argument is a
       Parameter binding at its declaration
       (``Sema::ConvertParamDefaultArgument``), judged once there and once
       per instantiation of a template, never at a call; a rebuild of a
       default argument or initializer and a SFINAE context are skipped
   * - ``double_destroy``, ``destroy_uninit``
     - 2 for flow-tracked sources, 1 otherwise
     - the Destroy sites of ``extractStdInitEvents`` for a source with a
       flow-tracked leaf (``judgeDestroySite``: ``double_destroy`` on
       ``Destroyed``, ``destroy_uninit`` unless ``May``, a reinitializer, or
       a marked parameter); otherwise the destroy arm of
       ``checkInitProfileBinding`` (its Parameter kind), where
       ``classifyUninitSource`` -- run exactly as for an
       unmarked binding target -- answers ``destroy_uninit`` by form and no
       destroyed state exists
   * - ``uninit_write``
     - 2 for flow-tracked targets, 1 otherwise
     - the SubobjectWrite sites of ``extractStdInitEvents`` for a target
       with a flow-tracked leaf (``judgeAccessSite``, suppressed by
       ``May``); otherwise ``checkInitProfileSubobjectWrite`` (its store
       preset trusts ``[[ref_to_uninit]]`` at the top level only: the
       member arm of the glvalue recognizer clears the trust, so subobject
       writes below the marker classify uninitialized)

Two helpers are shared across the rules.  ``classifyUninitSource`` -- the
pointer and glvalue recognizers -- classifies an expression as referring to
initialized, uninitialized, or unknown storage purely from its syntactic
form (the flow state of tracked storage is the CFG pass's, "Flow-Tracked
Storage (std::init)" below); its ``UninitAccessOpts``
presets distinguish a *binding* source (markers count everywhere), a value
*read*, and a scalar *store* (which differ in whether the top-level
``[[uninit]]`` marker counts and whether ``[[ref_to_uninit]]`` storage is
trusted).  ``defaultInitLeavesScalarIndeterminate`` answers whether a type's
default-initialization leaves an unacknowledged scalar subobject
indeterminate, trusting user-provided default constructors -- the paper's
trust-the-constructor principle (P4222R2 §5.1), which is also why members
of objects initialized by a user-provided constructor are not flow-tracked.

Clang itself supplies the standard library's lifecycle annotations:
``SemaProfiles::addKnownInitLifecycleAttributes``, called from
``Sema::AddKnownFunctionAttributes`` for every function declaration, attaches
an implicit ``RefToUninitAttr`` (first parameter) plus ``NowInitAttr`` to a
``std::construct_at``, and ``NowUninitAttr`` to a ``std::destroy_at``, whose
first parameter is of pointer type.  The form key keeps the marker subject
rules holding by construction (a ``T*`` parameter is a pointer by form even
when ``T`` is dependent), and the seam runs after declaration merging and
after ``checkNowInitVacuity``, so the vacuity check never sees a
half-injected pair.  Implicit attributes are indistinguishable from
hand-written ones, so the funnels, the CFG pass's lifecycle arm,
serialization, and suppression apply unchanged, and specializations
inherit them from the pattern via attribute instantiation.  Iterator-shaped
relatives (``destroy_n``, the ``uninitialized_*`` family) and ``ranges::``
CPOs sit outside the form key; the user-facing scope note lives in
:doc:`ProfilesFramework`.


Flow-Tracked Storage (std::init)
================================

The ``std::init`` recognizers classify storage from an expression's
syntactic form alone.  Inside a function body that answer is refined by the
definite-assignment engine in AnalysisBasedWarnings.cpp: the ``ExtraPass``
of the ``std::init`` ``CFGProfiles`` row harvests the body's flow-tracked
entities (``TrackedEntity``: ``[[uninit]]`` locals, the referents of marked
local pointers and references, and marked scalar members of locals and of
the current object), extracts one event stream from a CFG of its own
(exception edges, fully linearized), runs a forward dataflow over four bit
vectors per entity -- ``Must`` (assigned on every path), ``May`` (assigned
on some path), ``Esc`` (escaped; read leniency for local aggregates), and
``Destroyed`` (destroyed on every path and not stored since) -- plus, per
marked pointer or reference, ``Target``, the entity it refers to at that
point: an ``[[uninit]]`` local, a tracked member, another pointer's
anonymous referent, itself (an anonymous referent whose state is the
entity's own bits), or unidentified.  The transfer function resolves every
event and site leaf on such an entity through ``Target`` before applying
it, and a ``Reseat`` event carries the new referent (``ReseatTarget``): the
entity a single-leaf source names, what another marked pointer refers to,
a fresh anonymous referent for an untracked source, or unidentified for a
conditional source with a tracked arm.  An identified referent named by a
marked binding is asserted unassigned (``Must`` and ``May`` cleared,
``Destroyed`` and ``Esc`` kept); the ``Reseat`` follows the binding's
``Binding`` event in block order, so the judgment sees the pre-assertion
state.  A binding to a marked parameter is a ``Binding`` event only and
asserts nothing.  The pass reports the
``ref_to_uninit`` judgments of the
body's bindings, the ``double_destroy`` / ``destroy_uninit`` judgments of
its ``[[now_uninit]]`` calls, and the ``uninit_read`` / ``uninit_write``
judgments of its reads through and stores below tracked storage at their
program points through the shared violation gate.  The parse-time checks and
the pass split the work by one predicate, ``SemaProfiles::isFlowTrackedLeaf``:
a binding, destroy, read, or store with a tracked leaf is the pass's,
everything else -- and everything outside a function body -- is judged at
parse time without flow state.  The pass's resolver
(``TrackedStorage::resolve``) and the predicate share one lvalue-shape walk,
``SemaProfiles::flowLeafShape``, so no binding falls between the two.  A
templated body is never analyzed; each instantiation is analyzed on its own
CFG, in which statements TreeTransform reused from the pattern are ordinary
elements.  Consumers read the lattices as follows: a marked-target binding
fires on ``Must`` and ``double_destroy`` on ``Destroyed``; an
unmarked-target binding, ``destroy_uninit``, a read through a marker, and a
subobject write are suppressed by ``May``.  A
destroy or release on one of several arms leaves the entity possibly
assigned, not definitely, and not destroyed; a mutable alias of a marked
pointer handed out gives the pointer an anonymous referent in that state.  A
variable of an enclosing function reached by capture,
whose state the enclosing body decides, enters with ``May`` set and ``Must``
clear, so neither direction fires on it until the body itself stores.  The
remaining blind spots are listed in :doc:`ProfilesFramework`,
"Limitations".
