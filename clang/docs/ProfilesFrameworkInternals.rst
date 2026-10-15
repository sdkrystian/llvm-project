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
validates, designator mismatches, redeclaration compatibility, and, joined
with the imported module units' recorded dominions, the enforcement half of
a profile's liveness.  *Where* a rule is enforced is the
diagnostics engine's question: ``SemaProfiles::addProfileEnforcement``
records the enforcement into the list and maps the profile's rule
diagnostics to errors from the attribute to the end of the translation unit,
and ``ASTContext::isProfileRuleActiveAt`` -- the positional rung of every
violation gate -- asks whether the rule's diagnostic is mapped at the check
site and the site is not system-header-exempt
(``isProfileExemptSystemHeaderLoc``).  See `Enforcement and Suppression
State`_.

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
Every named rule has its own group, a ``ProfileRuleGroup`` record nested
in its profile's ``ProfileGroup`` record under ``-Wprofiles``; a rule-less
diagnostic sits directly in the profile's group.  The records carry the
profile and rule names and are the profiles registry:
``profiles::getProfileDiagGroupName`` resolves a name to its group by exact
match, and the ``-W`` name, ``profile-<profile>-<rule>``, is spelled from
the names by TableGen and identifies nothing.  The framework passes the
profile name as ``%0``:

.. code-block:: text

   def err_profile_type_cast_reinterpret : ProfileRule<
     "'reinterpret_cast' is unsafe under profile '%0'",
     ProfileTestTypeCastReinterpretCast>;

Every rule is checked at a *check site* behind one gate, in one of two
hosts.  A Sema check site -- an expression or declaration site, or the
class-completion or constructor-finalization *funnel* -- calls
``SemaProfiles::shouldEmitProfileViolation`` and emits a ``ProfileRule``
diagnostic; a CodeGen check site calls
``CodeGenFunction::EmitProfileRuntimeCheck`` and emits a runtime check.  A
rule that needs whole-function analysis rides the uninitialized-variables
analysis as a row of the ``CFGProfiles`` table, the framework's one
table-driven extension point, whose reporter and hooks call the same Sema
gate.  The gate's first rung is the profile's *liveness*: a profile is live
when it is enforced by this unit or an imported module unit, or a rule
diagnostic of it is enabled by a command-line option (`Enforcement and
Suppression State`_); the rungs after it decide per site.


Adding a Profile
================

Every profile follows the same recipe:

1. Define the diagnostics: a ``ProfileRule`` in ``DiagnosticSemaKinds.td``
   for a compile-time rule, or a ``Trap`` in ``DiagnosticTrapKinds.td`` for
   a runtime-checked rule, in the rule's diagnostic group
   (``DiagnosticGroups.td``: a new profile adds a ``ProfileGroup``, and a
   ``ProfileRuleGroup`` per named rule, under ``Profiles``; the records
   register its names).
2. Pick each rule's host.  A rule decided where Sema knows the fact is a
   Sema check site -- an expression site, a declaration site, the
   class-completion funnel, or the constructor-finalization funnel (`Check
   Sites`_) -- gated by ``shouldEmitProfileViolation("my::profile",
   diag::..., Loc, D)`` before its diagnostic; a rule that needs
   whole-function analysis is a row in the ``CFGProfiles`` table (`CFG
   Riders`_); a rule enforced by instrumenting the generated code is a
   CodeGen check site calling ``EmitProfileRuntimeCheck`` (`Runtime Check
   Sites`_).
3. Add tests; a test-only profile must be named under ``test::`` (see `Test
   Profiles`_).

Enforcement, suppression, liveness, module propagation, and serialization
then work without further profile-specific code.


Check Sites
===========

A Sema check site is a gate and a diagnostic:

.. code-block:: c++

   if (Profiles().shouldEmitProfileViolation("my::profile",
                                             diag::err_my_profile_rule, Loc))
     Diag(Loc, diag::err_my_profile_rule) << "my::profile";

The gate is ``SemaProfiles::shouldEmitProfileViolation``, whose rungs run
in order: the profile is live (`Enforcement and Suppression State`_); the
rule's diagnostic is mapped at ``Loc`` and ``Loc`` is not
system-header-exempt (``ASTContext::isProfileRuleActiveAt``, the positional
rung, which CodeGen's runtime checks run as is); the declaration passed as
``D`` is not templated; and the site is not in an unevaluated or discarded
context (a post-parse site, which has no evaluation context of its own,
skips these two).  Every check site in either host goes through that one
gate.  An entry point that does work before its first gate -- collecting
facts, walking an initializer -- hoists the liveness rung as its first
statement, ``if (!Profiles().isProfileLive("my::profile")) return;``, so a
profile that is not live costs nothing.  ``test::type_cast`` (an expression
site), ``test::class_final`` (the class-completion funnel), and
``test::ctor_final`` (the constructor-finalization funnel) are the in-tree
pilots.

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
exchange for reuse-proof diagnostics.  ``test::type_cast`` follows the
expression policy exactly: a ``reinterpret_cast`` of a non-dependent operand
fires at the definition, of a dependent operand once per instantiation.
An expression check whose source names flow-tracked storage is the CFG
pass's and follows the per-instantiation rule ("Flow-Tracked Storage
(std::init)" below).

Funnels
-------

An *expression site* is reached while an expression is built
(``test::type_cast`` in ``Sema::BuildCXXNamedCast``).  It has no declaration
to anchor the templated-declaration rung, so its own wrapper defers in a
dependent context and it follows the expression policy above.

A *declaration site* is reached while a declaration is acted on.  It passes
the declaration as the gate's ``D``, so it fires once per instantiation and
never on a template pattern.

*Class completion* is
``SemaProfiles::checkProfileViolationsAtClassFinalization``, run from
``Sema::CheckCompletedCXXClass``, the single function every
class-completion path funnels through (parsing, template instantiation,
lambda completion).  The wrapper filters out dependent classes (the checks
re-fire on each instantiation), invalid ones, and lambdas, and calls every
profile's class-completion check in turn; adding a profile is one call
there plus the check itself.  Class completion runs *before any constructor
body or member-initializer list has been parsed*, so a class-completion
check must not inspect a constructor's ``inits()``.

*Constructor finalization* is
``SemaProfiles::checkProfileViolationsAtConstructorFinalization``, run from
the three functions every user-provided constructor definition funnels
through -- ``ActOnMemInitializers``, ``ActOnDefaultCtorInitializers``, and
``SetDeclDefaulted`` (an out-of-line ``= default``) -- including
instantiation, with ``inits()`` fully populated.  The wrapper filters out
dependent constructors, invalid ones, and delegating constructors (which
leave member initialization to their target) and calls every profile's
constructor check in turn.  An out-of-line ``= default`` copy or move
constructor arrives here like any other; a check that must not see one
filters for itself, as does any filter that is one profile's policy rather
than the funnel's contract (``std::init`` exempts defaulted copy/move
constructors, which initialize member-wise while writing no initializer).
Each check gates its diagnostics on
``shouldEmitProfileViolation`` with the finalized declaration and its
location.  Rules that depend on what a constructor initializes belong at
this funnel; rules that need flow analysis are CFG riders.


CFG Riders
==========

A *CFG rider* is a rule that needs whole-function analysis and rides Clang's
uninitialized-variables analysis as a row of the ``CFGProfiles`` table, the
framework's one table-driven extension point.  The analysis owns the small
opt-in table of the profiles that ride it, one row per profile (profile
name, diagnostic); the framework never learns the profile's name.
``test::uninit_read`` is the in-tree example:

.. code-block:: c++

   constexpr CFGProfileEntry CFGProfiles[] = {
       {"my::profile", diag::err_my_profile_rule},
   };

The analysis's diagnostic reporter walks the live rows calling the single
gate as ``shouldEmitProfileViolation(Name, DiagID, Loc, /*D=*/nullptr,
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
the live rows' ``ConfigureCFG`` hooks add) and the fully linearized
shape (another analysis in the same run forced ``setAllAlwaysAdd``, which
adds arbitrary extra elements).  A pass's extraction arms may therefore only
match always-add classes or unconditional CFG elements: an element class
that is neither is present in one shape and absent in the other, so matching
it would produce diagnostics on one path only.  ``test::cfg_hooks`` is the
in-tree pilot for the hook columns.

Profile rules are errors, so the pass must also run where the warning
pipeline is skipped.  ``AnalysisBasedWarnings::hasLiveCFGProfile()``
gates those paths: after an uncompilable TU error, and when warnings are
disabled for the declaration (``-w``, or a system-header declaration under
``-fno-profiles-exempt-system-headers``), the per-function dispatch runs
``runProfileOnlyCFGAnalysis`` instead of skipping -- the same analysis with
the reporter in ``ProfileOnly`` mode, so an ordinary warning the error or
flag is meant to silence cannot resurface.  The CFG builder and the riders
assume a well-formed AST, which the warning pipeline ensures by analyzing no
body after an error; ``isProfileCFGAnalyzableBody`` keeps that assumption on
these paths by skipping a body an error may have left malformed (see
`Known Gaps`_).  ``Sema::ActOnFinishFunctionBody``
likewise keeps running per-function analysis after a TU error when such a
profile is live; without this, the first error would disable the profile for
every later function.

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


Runtime Check Sites
===================

A CodeGen check site is for a rule whose enforcement means emitting a
*runtime check* during code generation rather than a compile-time diagnostic
-- P3589R2 sanctions
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

``EmitProfileRuntimeCheck`` checks that the profile is enforced by this
unit or by an imported module unit whose code is emitted here under its own
enforcement (``ASTContext::isProfileEnforcedByAnyUnit``: a diagnostic
option can preview a compile-time rule; only an enforcement instruments
code) and that the rule
is enforced and not suppressed at the check site and the site is not in an
exempt system header (``ASTContext::isProfileRuleActiveAt``: the trap
diagnostic's mapping at that location, so a body deserialized from an AST
file is decided by the state its own unit recorded, and an NSDMI or default
argument by its member's or parameter's dominion wherever it is emitted).
Only when the
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
one handler kind and merge their locations, like UBSan's trap mode.  No
sanitizer facility consults the check: a profile's runtime check is a
language guarantee, not opt-in instrumentation.


Enforcement and Suppression State
=================================

An enforcement's dominion (P3589R2 [decl.attr.enforce]p4: the tokens from
the attribute to the end of the translation unit) is represented as
diagnostic state: ``SemaProfiles::addProfileEnforcement`` maps every
diagnostic of the profile's group (:ref:`profiles-diagnostic-groups` in
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
profile the implementation does not know has no entry in the registry, so
nothing is mapped for it, which is the specified behavior of an unknown
profile.

The engine's own rungs then apply: the rule diagnostics are latent, so
``-w`` keeps an enforced rule, ``-Weverything`` in either form leaves the
rules alone, and ``-Wprofile-...`` and the diagnostic pragmas move a rule's
severity like any other diagnostic's.  An enforcement's mapping is
installed after the command-line options, so ``-Wno-profiles`` and
``-Wno-error=profile-...`` cannot undo it.  Nothing is mapped until an
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

A profile is *live* in a compilation when it is enforced by this unit or by
an imported module unit -- recorded in this unit's ``ASTContext`` list by
attribute, by ``-fprofiles-enforce=``, or by inheritance, or in the
``Module::DominionProfiles`` of any module unit the compilation loaded
(``ASTContext::isProfileEnforcedByAnyUnit``) -- or a rule diagnostic of it
is enabled by a command-line option of this compilation
(``-Wprofile-<profile>``, ``-Wprofile-<profile>-<rule>``,
``-Werror=profile-...``, ``-Wprofiles``;
``ASTContext::isProfileRuleEnabledByOption``).  An inert ``test::`` profile
is never live.  ``SemaProfiles::isProfileLive`` combines the two halves and
is the first rung of ``shouldEmitProfileViolation``; an entry point that
does work before its first gate hoists the same test.  Liveness is a
whole-compilation superset pre-filter: a live profile's check sites still
decide by the positional mapping at their location, and a profile that is
not live does no work at all.  The imported-unit half is what keeps module
code's checks when the module enforced a profile and the importer did not:
the module's positional mappings travel with its files, and this unit's
list, which is per unit, would otherwise filter the site off before they
are consulted.  The option half is snapshotted in the ``ASTContext``
constructor, before ``-fprofiles-enforce=`` maps anything and before any
diagnostic state is recorded, where an unlocated
``DiagnosticsEngine::isIgnored`` query reads the command-line state; it is
a constant of the compilation, so ``isProfileLive`` caches its per-profile
answer (``RulesEnabledByOption``), while the enforcement half is evaluated
on every call because an in-source enforcement or a lazily loaded import
can turn it on mid-unit.  A ``#pragma clang diagnostic warning
"-Wprofile-..."`` with neither an enforcement nor an option does not make a
profile live: the preview is a build-flag decision, and honoring a pragma
alone would need a query of the engine's latest state and a cache keyed on
state identity at every hot entry point.  A pragma changes the severity of
a live profile's rule and enables nothing by itself.  Liveness gates
compile-time rules only: a diagnostic option can preview a compile-time
rule; only an enforcement instruments code (`Runtime Check Sites`_).

A suppression's dominion ([decl.attr.suppress]p3: the attribute's tokens
through the last token of the declaration or statement it appertains to) is
recorded the same way, by ``Parser::ProfileSuppressionDominion`` guards.
``SemaProfiles::beginSuppression`` maps every diagnostic of the attribute's
rule group (the profile's group for a rule-less attribute) from its mapping
at the attribute's expansion location to ignored, from there on, and
``endSuppression`` maps it back from the end of the last consumed token on.
Both go through ``DiagnosticsEngine::replaceDiagnosticMappingsAt``, which
replaces a mapping only where it is still the one replaced: in the state at
the location, in the states a ``#pragma clang diagnostic`` lexed ahead of
the parser recorded after it, and in the states a ``push`` saved, which a
``pop`` can bring back.  The second set is bounded by the parser's
frontier, the last position its tokens were lexed to: unbounded in an
in-order parse, where every state recorded after the location was lexed
ahead, and, while a token-cached region is parsed late -- an inline member
function body, default argument, exception specification, member
initializer, late-parsed attribute, delayed template, or a method or
function body in an ``@implementation`` -- that region's last token
(``Parser::LateParsedRegionEnd``).  The region's tokens were lexed when it
was cached, so a pragma among them is one the parser lexed ahead: a
dominion that begins in the region covers it from the begin on, before the
check sites inside are judged at parse time.  The states recorded beyond
the region belong to the constructs that follow it, which were parsed
before it, and keep their dominions.
A pragma's mapping always carries the no-``-Werror`` and
no-``-Wfatal-errors`` flags and the framework's never does, so a pragma
inside the dominion that maps a suppressed rule keeps its effect after the
dominion.  A guard whose attributes suppress nothing new (an enclosing
construct already suppresses the rule) records nothing, so the doubled
guards of a block-scope declaration cost nothing.  Because the state is
keyed on positions, a token-cached body, default argument, or member
initializer parsed after its class, and a template instantiated at any
later point, find the dominion their tokens lie in without re-establishing
anything: ``replaceDiagnosticMappingsAt`` inserts a transition when the lexer
has already passed the location.  The engine creates the state of a macro
expansion or included file when a diagnostic is first queried in it, and a
query can run ahead of a guard -- the misleading-indentation check reads the
token after an ``if``, ``while``, or ``for`` body while the statement's guard
is still open -- so ``DiagStateMap::append`` and ``insert`` also give each
such file created after the new transition its new entry state.

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

The corners where the recorded state departs from the dominions' token
ranges are listed in `Known Gaps`_.


Modules and Serialization
=========================

``[[profiles::enforce]]`` on a module interface declaration *advertises* the
enforced designators: they are recorded on
``Module::AdvertisedProfiles``, which is what
``[[profiles::require]]`` on an import validates against.  A header unit
advertises the same way from the empty-declaration form P3589R2 prescribes
for headers.  A non-partition implementation unit inherits the
interface's enforcements through its implicit import of the primary
interface.  A partition implementation unit inherits them only from an
interface module already loaded when its module-declaration is processed
(see `Known Gaps`_).
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
check below reads it, ``[[profiles::require]]`` never does.  The ASTReader
also records each loaded module's names into
``ASTContext::ImportedDominionProfiles``, the imported-unit half of
``isProfileEnforcedByAnyUnit``.
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
``Module::DominionProfiles`` (``SUBMODULE_DOMINION_PROFILES``).  It is distinct
from the advertised set that ``[[profiles::require]]`` reads.  A previous
declaration from the same module family is skipped: this unit inherits the
interface's enforcements, and an implementation unit's additional TU-local
enforcements do not obligate the interface.  A declaration in an explicit
global module fragment is skipped too (see `Known Gaps`_).  A previous
declaration owned by a module-map module (a Clang header module, ``-fmodules``)
is not checked at all: that is textual inclusion wearing an AST file, not a
separate TU in the standard's model, so it has no dominion of its own -- header
units, which are real TUs with recorded designators, stay checked.  A textual
or PCH previous declaration shares the current TU's dominion and is not
checked; implicit template instantiations are exempt.  A previous declaration
covered by the system-header exemption stopgap is skipped as well -- a header
unit built from a system header has no recorded designators, and redeclaring or
specializing its entities must not draw the error while every other check
exempts that code (``-fno-profiles-exempt-system-headers`` restores spec-exact
checking).  Profiles gated off in this compilation (``test::`` names without
``-fprofiles-test-profiles``) are inert on either side of the comparison.


.. _profiles-known-gaps:

Known Gaps
==========

Each entry is a place where the framework misses a check that P3589R2 or
:doc:`ProfilesFramework` calls for, or applies one that they exempt, or
(`Compile-Time Cost`_) a shape whose cost grows faster than its input.

Templates
---------

- Under ``-fdelayed-template-parsing`` the body of a never-instantiated
  template is never parsed, so the pattern-time half of the expression
  policy (`Check Sites`_) does not occur in that mode.

Suppression Dominions
---------------------

Where the recorded dominion departs from the construct's token range:

- Positions are expansion sites, so a dominion that begins or ends inside a
  macro expansion covers the whole expansion: a suppression spelled inside a
  macro, or whose construct ends inside one, also suppresses the other
  tokens of that expansion (the one under-check corner).
- A check site with an invalid location sees the latest recorded state:
  command-line enforcement plus whatever enforcement or suppression was most
  recently recorded anywhere in the unit -- an over- or under-check
  depending on what was last recorded.
- A function or Objective-C method defined inside an ``@implementation`` has
  its body token-cached (``StashAwayMethodOrFunctionBodyTokens``) without a
  Decl-keyed guard, so a suppression on its declarator-id, or on the
  method's declaration, covers none of the body.  The prefix form
  (``[[profiles::suppress(...)]] void g() { ... }``) covers it.
- An in-class OpenMP ``declare reduction`` or ``declare mapper`` directive
  (``Parser::ParseLexedPragma``) and a late-parsed C attribute
  (``Parser::ParseLexedCAttribute``) are parsed from cached tokens without a
  region bound, so a dominion that begins inside one replaces the states
  recorded after it as in an in-order parse.  Its end then restores the rule
  inside the dominions of the class's later members: a check site there
  judged after the class is complete (a template instantiation) is not
  suppressed.

Compile-Time Cost
-----------------

- Inside one token-cached region parsed late, every suppression's begin and
  end replace, by copying, each ``#pragma clang diagnostic`` state recorded
  after it in the region (`Enforcement and Suppression State`_).  A region
  with *N* suppressions and *N* such states -- an inline member function
  body that repeats a suppressed statement beside a ``_Pragma``-spelled
  ``push``/``ignored``/``pop`` -- takes time and memory quadratic in *N*;
  the same body parsed in order is linear.

Analysis-Based Rules
--------------------

- After an error, a function body the error may have left malformed gets no
  CFG-rider diagnostics: one whose own function scope reported an
  unrecoverable error, or one that reaches an expression containing errors --
  in its statements, a nested lambda body, or, for a constructor, the default
  member initializers it uses.  An error elsewhere can leave such an
  expression in an otherwise valid body, as a ``case`` label naming a
  ``constexpr`` variable whose initializer failed.  The error rejects the
  translation unit, so no violation is accepted; the body's rider
  diagnostics appear once the error is fixed.

Modules
-------

- A partition implementation unit (``module M:P;``) inherits the
  interface's enforcements only from an interface module already loaded
  when its module-declaration is processed: ``ModuleMap::findModule`` does
  not load one, and its absence is not diagnosed.  With the lazy
  ``-fmodule-file=M=<path>`` and ``-fprebuilt-module-path=`` forms the
  interface is never loaded there, so the partition is not enforced; the
  eager ``-fmodule-file=<path>`` form inherits.  An ``import M;`` in the
  partition comes after the module-declaration and does not help, and an
  internal partition that the primary interface imports is compiled before
  the interface's BMI exists.  Each is a missed diagnostic, never a wrong
  one.
- A declaration in an explicit global module fragment is not checked for
  redeclaration compatibility (`Redeclaration Compatibility`_): an
  enforcement written on the module-declaration does not cover it while one
  written before the fragment's declarations does, and the recorded
  enforcement set cannot tell the two apart.

Runtime Checks
--------------

- The ``test::arith`` zero-divisor check does not cover GCC
  vector-extension integer division or ``_Complex int`` division, the
  operations UBSan's integer-divide-by-zero check leaves unchecked as well.


Test Profiles
=============

The six built-in ``test::`` profiles exist only to exercise the framework in
the test suite.  They are gated on the ``-cc1``-only
``-fprofiles-test-profiles`` flag: under ``-fprofiles`` alone their
designators are still parsed, recorded, and exported across modules, but
``isProfileEnforced`` and ``isProfileLive`` report any ``test::``-prefixed
profile as not enforced and not live, so no ``test::`` rule ever fires.
Because that gate keys on the
``test::`` prefix, a new test-only profile must also live under ``test::``.

- ``test::type_cast`` -- an expression site; diagnoses ``reinterpret_cast<>``
  (the keyword form only).
- ``test::uninit_read`` -- a CFG rider; rides the existing CFG
  uninitialized-variables analysis.
- ``test::cfg_hooks`` -- a CFG rider; exercises the optional hook columns: its
  ``VarExempt`` hook exempts variables named with an ``exempt`` prefix from
  its uninitialized-read rule, its ``ConfigureCFG`` hook always-adds lambda
  expressions, and its ``ExtraPass`` diagnoses every lambda-expression CFG
  element under its ``lambda`` rule.
- ``test::class_final`` -- the class-completion funnel; fires on completion
  of every non-lambda class, on instantiations rather than dependent
  patterns.
- ``test::ctor_final`` -- the constructor-finalization funnel; fires once per
  user-provided, non-delegating constructor.
- ``test::arith`` -- a CodeGen check site; its ``zero_divide`` rule emits a
  runtime zero-divisor trap check on integer division and remainder.

The names ``test::other``, ``test::bounds``, ``test::new_profile``, and
``test::not_enforced`` are deliberately *not* implemented and appear in
negative tests as "some other profile" stand-ins; adding a real profile under
any of them would invalidate those tests.


The std::init Implementation Map
================================

``std::init`` (documented in :doc:`ProfilesFramework`) hosts its rules as
follows:

.. list-table::
   :header-rows: 1
   :widths: 22 20 58

   * - Rule
     - Host
     - Primary entry points
   * - ``uninit_read``
     - CFG rider; expression site
     - ``CFGProfiles`` row for local variables; ``runStdInitMemberReadChecks``
       for the same row through its ``ExtraPass`` hook: one
       ``TrackedStorage`` entity table (the current object's and tracked
       locals' ``[[uninit]]`` members: scalars, and class-type members as
       whole objects), one ``extractStdInitEvents``
       loop whose arms resolve every leaf of the one shape walk
       (``SemaProfiles::forEachTargetLeaf``) through
       ``TrackedStorage::resolve``, and one definite-assignment run over a
       CFG of its own, built with exception edges over a fully linearized
       shape (the shared CFG the other analyses see is untouched); the
       ``CallExpr`` arm turns a ``[[now_init]]`` call into a ``Gen`` bit for
       the storage bound to the callee's marked parameters (P4222R2 §6.2)
       and a ``[[now_uninit]]`` call into a ``Kill`` that clears the
       assigned bit again (a local's escape credit already subsumes
       ``[[now_init]]``).  Constructors cannot carry either attribute: a
       construction can run with no call spelling (copy-initialization, a
       by-value argument, a member initializer), so a lifecycle effect there
       would be invisible at the site, and the engine would have to
       enumerate every construction spelling.  A "destroyed here"
       note on the read is deferred -- a kill witness would have to be
       carried per bit through the meet and both engine replays, roughly
       doubling the engine state and touching the enqueue-skip invariant);
       ``checkInitProfileReadThrough`` at the lvalue-to-rvalue chokepoint,
       plus compound-assignment and increment/decrement hooks, for a
       glvalue with no flow-tracked leaf (a rebuilt default argument or
       initializer is skipped, as for ``ref_to_uninit``, and so is a read the
       template pattern judged, where an instantiation rebuilds it,
       ``PatternReadSites``); the ReadThrough sites of
       ``extractStdInitEvents`` (``judgeAccessSite``, which requires
       initialization on every path) otherwise
   * - ``uninit_decl``
     - declaration site
     - ``checkInitProfileUninitDecl``
   * - ``uninit_with_initializer``
     - declaration site; class completion
     - ``checkInitProfileUninitWithInitializer``;
       ``checkStdInitUninitFieldMarker`` from the class-completion wrapper
       for an initializer-less ``[[uninit]]`` data member
   * - ``static_runtime_init``
     - declaration site
     - ``checkInitProfileStaticRuntimeInit``
   * - ``static_marker``
     - declaration site
     - ``checkInitProfileStaticMarker``, hosted at every point a
       static-duration declaration's initializer state becomes final: both
       arms of ``ActOnUninitializedDecl`` (definitions and non-defining
       declarations) and the instantiated in-class static data member arm of
       ``InstantiateVariableInitializer``
   * - ``union_marker``, ``pointer_marker``
     - attribute handler
     - ``checkInitProfileMarkerPlacement`` (a ``va_list`` is exempt from
       ``pointer_marker``: ``isBuiltinVaListSugar``)
   * - ``ctor_uninit_member``
     - constructor finalization; class completion for inherited
       constructors
     - ``checkStdInitCtorUninitMember`` from the constructor-finalization
       wrapper for user-provided constructors;
       ``checkStdInitInheritedCtorUninitMember`` from the class-completion
       wrapper checks the members and non-nominated bases an inherited
       constructor leaves uninitialized, once per class at the
       ``using``-declaration
   * - ``ref_to_uninit``
     - CFG rider for flow-tracked sources, expression site otherwise
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
       state (``judgeBindingSite``); the call, construction,
       new-expression, and Objective-C message arms zip arguments with
       parameters (a placement argument with its allocation function's)
       through one walk,
       ``forEachArgumentBinding``, which the lifecycle arm
       (``appendLifecycleCallEvents``) shares; a handle-typed target
       (``isHandleType``) classifies its source through the handle walk, and
       a std-table construction's transfer parameters
       (``CalleeLifecycleRoles::transfersParam``) are judged where the
       handle lands, and a handle reseat -- an ``operator=`` or an owner's
       ``reset`` -- is a PointerAssignment binding
       (``checkInitProfileHandleReseat``, from
       ``Sema::CreateOverloadedBinOp`` and ``Sema::CheckFunctionCall``);
       ``classifyPointerGlvalue`` judges a
       reference-to-pointer binding by the pointer's value (a read-only
       alias) or not at all (a mutable alias); a defaulted argument is a
       Parameter binding at its declaration
       (``Sema::ConvertParamDefaultArgument``), judged once there and once
       per instantiation of a template, never at a call; a rebuild of a
       default argument or initializer and a SFINAE context are skipped
   * - ``double_destroy``, ``destroy_uninit``
     - CFG rider for flow-tracked sources, expression site otherwise
     - the Destroy sites of ``extractStdInitEvents`` -- a ``[[now_uninit]]``
       or destroy-row argument, an explicit or pseudo-destructor call's
       object, the operand of a ``delete`` that runs a non-trivial destructor
       (``appendLifecycleCallEvents``), the non-trivial scope-exit
       destructor of an ``[[uninit]]`` local (its ``CFGAutomaticObjDtor``,
       gated at the declaration, unless a marker rule rejects the marker) or
       of a read-tracked aggregate local's whole member,
       and the scope exit (a by-value parameter's at the function's exit),
       ``reset``, reassignment, and destructor call of a marked
       ``std::unique_ptr``, which destroy its referent unless the owner is
       null (``isUniqueOwnerType``, ``FlowState::Null``) and are no
       ``double_destroy`` while it is null on some path
       (``FlowState::MayNull``) -- for a source with a
       flow-tracked leaf (``judgeDestroySite``: ``double_destroy`` on
       ``MayDestroyed``, ``destroy_uninit`` unless ``May``, a reinitializer, or
       a marked parameter); otherwise ``checkInitProfileDestroy``, entered
       from the destroy arm of ``checkInitProfileBinding`` (its Parameter
       kind) and from ``checkInitProfileDestructorCall``, where
       ``classifyUninitSource`` -- run exactly as for an
       unmarked binding target -- answers ``destroy_uninit`` by form and no
       destroyed state exists; a ``delete`` is not judged by form
   * - ``uninit_write``
     - CFG rider for flow-tracked targets, expression site otherwise
     - the SubobjectWrite sites of ``extractStdInitEvents`` for a target
       with a flow-tracked leaf (``judgeAccessSite``, which requires
       initialization on every path); otherwise
       ``checkInitProfileSubobjectWrite`` (its store preset trusts
       ``[[ref_to_uninit]]`` at the top level only: the member arm of the
       glvalue recognizer clears the trust, so subobject writes below the marker
       classify uninitialized)

Three recognizers and a helper are shared across the rules.  The pointer,
glvalue, and handle recognizers -- ``classifyUninitSource`` dispatches a
binding source to one by its bound type, the handle recognizer taking a
handle (`Handles (std::init)`_) -- classify an expression as referring to
initialized, uninitialized, or unknown storage purely from its syntactic form
(the flow state of tracked storage is the CFG pass's, "Flow-Tracked Storage
(std::init)" below); their ``UninitAccessOpts`` presets distinguish a *binding*
source (markers count everywhere), a value *read*, and a scalar *store* (which
differ in whether the top-level ``[[uninit]]`` marker counts and whether
``[[ref_to_uninit]]`` storage is trusted; the read and store presets also ignore
a data member's ``[[uninit]]`` marker at every level, ``TrustUninitMembers``,
outside a default member initializer, ``trustsUninitMembers``).  Storage
reached through pointer arithmetic -- the glvalue recognizer's indirection,
arrow, and subscript arms -- is classified by the arithmetic's pointer operand
(``SemaProfiles::pointerArithmeticBase``), as through ``p[n]``; the pointer
recognizer leaves the arithmetic's value unknown, so a binding of an end
iterator is unclassified.  The by-form destroy judgment skips an element of
what a pointer or handle refers to (``designatesReferentElement``).
``defaultInitLeavesScalarIndeterminate`` answers whether a type's
default-initialization leaves an unacknowledged scalar subobject indeterminate,
trusting user-provided default constructors -- the paper's trust-the-constructor
principle (P4222R2 §5.1), which is also why members of objects initialized by a
user-provided constructor are not flow-tracked.

Clang itself supplies the standard library's lifecycle roles: the std lifecycle
functions are rows of ``KnownCallees`` keyed by namespace-std name and arity
(``std::construct_at`` and ``std::destroy_at`` of any arity).  A std row names
one role parameter -- ``Construct`` for ``construct_at`` and the
``std::uninitialized_*`` output range, ``AssertUninit`` for ``std::now_init``'s
parameter, ``Destroy`` for ``destroy_at`` and the first iterator of
``std::destroy`` and ``std::destroy_n`` -- and matches only where that parameter
of the called declaration has the row's shape (``ParamShape``), a pointer for
every namespace-std row -- or, for an ``uninitialized_*`` row, also a handle
(``KnownCalleeEntry::AcceptsHandle``) -- so a specialization whose iterator is
of another shape falls outside it.  A range row also names the range's end
iterator (``EndParam``; ``CalleeLifecycleRoles::isEndParam``), whose binding is
accepted in any state and which earns no credit and no destroy.  The
``Destroy`` role names one parameter, so the execution-policy overloads of
``std::destroy`` and ``std::destroy_n`` are ordinary rows.  The rows are looked
up at each call
(``getCalleeLifecycleRoles``) and never attached as attributes: the library's
declarations stay as written, so a module or header unit built without
``-fprofiles`` reads the same, and a library definition is judged against its
author's markers alone.  The ``Construct`` rows are bidirectional like
``construct_at`` (P4222R2 §4.7 rejects an initialized range).  The
default-construct rows are ``Construct`` rows only where default-initialization
of the called specialization's range value type (``rangeValueType``: a
pointer's pointee, an array's element, or a class iterator's or range's
``handleValueType``: its or its bases' ``element_type`` or ``value_type``, else
what its ``operator*`` returns, else its ``begin()`` iterator's) leaves nothing
indeterminate
(``defaultInitLeavesScalarIndeterminate``, which skips ``[[uninit]]`` members)
and ``AssertUninit`` rows otherwise: a credit there would legalize reads of the
indeterminate scalars that default-initialization leaves.  The ``std::ranges``
lifecycle CPOs are rows keyed by their object's name and their call operator's
arity (``findRangesCPORow``): the callee is the call operator, static or not,
of a class that ``std::ranges`` encloses, and the namespace's member of the
row's name -- seen through an inline namespace such as libc++'s ``__cpo`` -- is
a variable of that class, so the class's own name, which differs between
libstdc++ and libc++, never matters.  An iterator overload's row names its
sentinel as the end iterator, and a range overload's row matches where the
forwarding reference binds an array (``ParamShape::ArrayReference``) or, for
an ``uninitialized_*`` object, a class.  A class taken by such a reference is
judged as a range (``HandleLeaf::Mode::Range``: its storage, then its handle
state), not as a reference to the class object, and a class-type iterator as
a handle (`Handles (std::init)`_); a credit reaches the storage of each of its
transfer leaves that is not ``Partial``, designated by the value type.

Library callees whose semantics the profile needs without an attribute are
rows of the known-callee table (``KnownCallees`` in SemaProfiles.cpp), keyed
by builtin ID, by form for the ``operator new`` / ``operator delete`` families,
or by the name of a namespace-std function or ``std::ranges`` object and its
arity (``KnownCalleeKey``), with a name-only *untrusted* fallback where the ID
is absent; the roles reach the funnels and the CFG pass through
``getCalleeLifecycleRoles``.  A ``Derive`` row (``std::begin``,
``std::cbegin``, ``std::data``, ``std::to_address``, ``std::rbegin``,
``std::crbegin``, and, offsetting their argument, ``std::end``, ``std::cend``,
``std::rend``, ``std::crend``, ``std::next``, ``std::prev``, and the
``std::ranges`` access objects of the same names, ``cdata`` included) and an
``AnyState`` row (``std::size``, ``std::ssize``, ``std::empty``,
``std::distance``, and their ``std::ranges`` objects) bind no argument
(``SemaProfiles::bindsNoArgument``, as the identity functions bind none); a
``Derive`` call's result is classified as its argument
(``SemaProfiles::derivedArgument``, after a ``std::ranges`` object's own) --
an array as it decays, a pointer by its value, a class as a range -- by the
recognizers, the handle walk, the shape walk (an offsetting row as an element
step), the predicate, and the resolver.  A row may describe only what the
callee returns:
the ``std::*_for_overwrite`` factories return a handle to a default-initialized
object (``ReturnsDefaultInitialized``), which the handle recognizer classifies
from the factory's first template argument as the new-expression arm
classifies ``new T``.  A row names its *role parameter*, the one
parameter the call acts on, and what the call does to the storage bound to it
(``KnownParamRole``); the roles are per parameter, so the callee's other
arguments are ordinary bindings.  A row's *initializing parameter* is a role
parameter whose storage the callee initializes whatever its prior state (the
destination of ``memset``, ``memcpy``, and ``memmove``, and of their fortified
``_chk`` builtins, and the ``va_list`` of ``va_start`` and the destination of
``va_copy``).  The binding to it is accepted in any live state, and the
flow pass writes the argument's storage whole on a trusted match only: by name
the acceptance survives but the credit does not, mirroring the release rows'
``Kill``, because a credit suppresses later diagnostics.  Unlike
``[[ref_to_uninit]]`` with ``[[now_init]]``, the role is one-directional: it
asserts nothing about the prior state, so an initialized destination is legal
and a repeated call is no double initialization.


Handles (std::init)
===================

A *handle* is an object of non-union class type
(``SemaProfiles::isHandleType``): a smart pointer, a span, an iterator.  On
one, ``[[ref_to_uninit]]`` says the memory it refers to is uninitialized
(P4222R2 §4.6, §9.3); the handle object itself is initialized.  The handle
recognizer classifies a handle expression from its *leaves*, which one walk
enumerates for every consumer (``SemaProfiles::forEachHandleSourceLeaf``,
``HandleLeaf``).  The walk peels parentheses, full expressions, temporaries,
the no-op, constructor, derived-to-base, and user-defined conversions, and the
pass-through forms; it follows a copy or move construction to its source, a
member call that returns the handle itself -- by reference to the method's
own class or a base (``++it``), or by value as its own class, another
specialization of its class template, a table class of its kind, or a class
of its value type (``s.subspan(1)``, ``s.begin()``; ``handleSelfReturnObject``)
-- to its object, and a non-member ``+`` or ``-`` returning an operand's class
(``n + it``) to that operand, marking the leaf ``Partial`` when the call refers
to part of the handle (it has explicit arguments or template arguments, is
``++``, ``--``, ``+=``, ``-=``, or a non-member ``+`` or ``-``, or is a member
named for a position past the start or a part: ``end``, ``cend``, ``rend``,
``crend``, ``first``, ``last``, ``subspan``, ``next``, ``prev``, ``advance``),
and an implicit local (range-for's ``__range`` and ``__begin``) or an
unmarked local reference to its initializer, followed once per outermost
walk, the walks its ``Range`` leaves start included (``HandleWalkMemo``),
so a self-referential initializer ends; and it distributes a conditional
into one leaf per arm, combined as the recognizers combine arms, so a marked
and an unmarked arm are Mixed (P4222R2 §4.9).  A terminal classifies by its
form: a named variable, parameter, or member is uninitialized when marked; a
marked reference to a class is Unknown, since it is the handle object that is
uninitialized there; a call takes its marked callee's state; a construction is
uninitialized when its constructor has an author-marked parameter; an
initializer list is Unknown; and a handle reached through other storage is
initialized when that storage is, and Unknown otherwise.

A pointer or reference a member function of a handle returns is *derived* from
the handle: the call arm of the pointer and glvalue recognizers gives an
unmarked dot call, member operator, or explicit-object method that returns a
pointer to an object, or a reference to an object of a type other than the
method's class and its bases (``derivingObject``), the state of its object as a
handle when that is uninitialized or Mixed -- Unknown under the store preset's
marker trust, so ``*p = 5`` stores like ``*q = 5`` through a marked pointer.
The library's members carry no marker, and nothing tells the memory a handle
refers to apart from the handle's own parts, so every such result counts (see
`Known Gaps (std::init)`_).  An arrow call reaches its object through a pointer,
and a call operator's object is the callable (a ``std::ranges`` object):
neither derives anything.  The object-argument rule still judges the handle
object's own storage, so ``s.size()`` on a marked span is accepted.

Raw storage enters a handle through the construction of a class in the std
handle table (``getStdHandleKind``): a *view* -- ``std::span``,
``std::ranges::subrange``, ``std::reverse_iterator``, ``std::move_iterator``,
``std::basic_const_iterator``, libc++'s ``std::__wrap_iter``, and
libstdc++'s ``__gnu_cxx::__normal_iterator`` -- or an *owner* --
``std::unique_ptr`` and ``std::shared_ptr``.  Its *transfer parameters*
(``CalleeLifecycleRoles::transfersParam``) are, for a view's constructors other
than copy and move, every parameter not of integral, enumeration, floating, or
``std::nullptr_t`` type, and for an owner's, the first object pointer taken by
value -- so never a deleter or an allocator, and only the pointer of the
aliasing constructor -- or, with none, a parameter whose class is an owner.
The walk makes the first transfer argument a leaf (a later one, a last
iterator or a sentinel, bounds the range): a pointer, or a read-only alias
of one, by its value (``PointerValue``); a class, or a reference to one, by its
storage and then its own handle state (``Range``); any other reference by its
storage (``Storage``).  A table construction with no transfer argument -- a
default, null, or ``weak_ptr`` construction -- is Unknown.

The transfer arguments are judged where the handle *lands*, not at the
construction: neither the parse-time funnel nor the CFG pass's construction arm
binds a transfer parameter (the pass still records the mutable aliases an
argument hands out), and every binding site admits a handle target, whose
source the walk re-traverses down to the construction.  A base subobject is
a binding site judged unmarked, and so is the handle an unmarked reference to
a handle refers to (``SemaProfiles::bindsHandleByReference``), besides the
reference's own binding of the handle object -- except the implicit object
parameter, a comparison's or a ``+``'s or ``-``'s parameter, and the source of
a copy or an assignment, judged where the copy lands (see
`Known Gaps (std::init)`_).  This also covers an
explicit temporary (``span<int>(buf, n)`` is bound where its value lands).
Handing the outer target's marking to the constructor call instead fails for
an explicit temporary, for a conversion built during overload resolution, and
under TreeTransform's reuse of a construction.  A discarded temporary handle
lands nowhere (see `Known Gaps (std::init)`_).  An implicit handle local
(range-for's ``__begin`` and ``__end``) is not a binding of its own: the walk
sees through it to the range, so the elements a range-for binds are judged
instead.  An author-marked constructor parameter makes a construction a
terminal that refers to uninitialized memory, while the parameter's own
binding is judged as usual.

A ``std::array`` is storage, not a handle: its ``begin``, ``end``,
``cbegin``, ``cend``, and ``data`` (``SemaProfiles::isStdArrayAddressMember``)
are exempt from the object-argument rule, and their result designates the
array's element storage -- the pointer recognizer classifies the array as a
decayed array, the predicate and the resolver take the array whole (from
``end``, a subobject), the shape walk steps through them to an element of the
array, and the walk makes it a ``Range`` leaf; called through a pointer they
designate its referent instead -- while ``designationCovers`` counts a
``std::array`` by its elements, so an ``int`` credit covers a
``std::array<int, 4>``.  Its element members (``operator[]``, ``at``,
``front``, ``back``; ``isStdArrayElementMember``) bind the array on every
path, as a built-in array's element read is judged.

A handle is reseated by a one-parameter ``operator=`` whose source is a class
object, as an operator or called by name, and an owner also by ``reset`` with
a written argument (``SemaProfiles::getHandleReseat``), arrow calls included:
the source -- for ``reset``, its pointer, a transfer parameter -- binds the
handle like a pointer assignment, judged against the marking
``resolveHandleReseatMarking`` reads off the handle object.  A handle reached
through a reference, a pointer, or an element names no declaration that can
carry the marker, so it is unmarked; only arms of disagreeing marking leave
the reseat unjudged.  A handle an ``operator=`` takes by value (copy-and-swap)
binds where it is assigned, not as a parameter (``isAssignmentSource``).
Libstdc++ declares ``std::shared_ptr``'s ``reset`` in its base
``std::__shared_ptr``, which the table lists as an owner for that reason.


Flow-Tracked Storage (std::init)
================================

The ``std::init`` recognizers classify storage from an expression's
syntactic form alone.  Inside a function body that answer is refined by the
definite-assignment engine in AnalysisBasedWarnings.cpp: the ``ExtraPass``
of the ``std::init`` ``CFGProfiles`` row harvests the body's flow-tracked
entities (``TrackedEntity``: ``[[uninit]]`` locals, the referents of marked
local pointers and references, and marked scalar members of locals and of
the current object, also inside member subobjects), extracts one event stream
from a CFG of its own
(exception edges, fully linearized), runs a forward dataflow over one word
of flags per entity (``FlowState::Bit``) -- ``Must`` (assigned on every
path), ``May`` (assigned on some path), ``MayUnassigned`` (unassigned on
some path that knows the entity's state), ``Esc`` (escaped; read leniency
for local aggregates), ``MayDestroyed`` (destroyed on some path and not
stored since), and the ones described where they are used -- plus, per
marked pointer or reference, ``Target``, the entity it refers to at that
point: an ``[[uninit]]`` local, a tracked member, another pointer's
anonymous referent, itself (an anonymous referent whose state is the
entity's own flags), or unidentified.

The states of a body's blocks share structure.  ``FlowState`` keeps the flag
words and the targets in two ``SharedArray`` tries of reference-counted
nodes, 64 entries wide and copied on write: a block's entry and exit states
store only the nodes its events change, a comparison of two states skips the
nodes they share, and a join builds nodes only where its operands differ.
The memory of a body's flow states is therefore linear in its events, not in
its blocks times its entities.  The join identity, the state of a
predecessor not yet reached, is a state without nodes
(``FlowState::Reached``), so a flag or a ``Target`` has no top value of its
own: a ``Target`` joins to itself or to unidentified.  A block joins its
predecessors' exit states as a balanced tree, so P predecessors that each
change different nodes cost on the order of P log P leaf joins rather than P
squared.

The transfer function resolves every event and site leaf on a marked
pointer's or reference's entity through ``Target`` before applying
it, and a ``Reseat`` event carries the new referent (``ReseatTarget``): the
entity a single-leaf source names, what another marked pointer refers to,
a fresh anonymous referent for an untracked source, the target every arm of the
source agrees on, or unidentified when the arms disagree and one is tracked.
A reseat to a fresh or escaped referent leaves every pointer that refers to
the replaced one unidentified; only a pointer that a chain of ``ViaPointer``
reseats connects to it can (``TrackedStorage::AliasClasses``).  An
identified referent named by a marked binding is asserted unassigned (``Must``,
``May``, and ``MembersPending`` cleared, ``MayUnassigned`` set,
``MayDestroyed`` and ``Esc`` kept); the ``Reseat`` follows the binding's
``Binding`` event in block order, so the judgment sees the pre-assertion
state.  A binding to a marked parameter is a ``Binding`` event only and
asserts nothing.  A tracked local's DeclStmt emits a ``Reset`` of its
``[[uninit]]`` entity and one over its member entities ahead of the
declaration's own binding and copy events, clearing every flag but
``MayUnassigned``, which it sets: each execution of the declaration starts a
new, unassigned object.  A marked pointer or reference is reseated by its
initializer instead.  The pass reports the ``ref_to_uninit`` judgments of
the body's bindings, the ``double_destroy`` / ``destroy_uninit`` judgments of
its ``[[now_uninit]]`` calls, and the ``uninit_read`` / ``uninit_write``
judgments of its reads through and stores below tracked storage at their
program points through the shared violation gate.  The parse-time checks and
the pass split the work by one predicate, ``SemaProfiles::isFlowTrackedLeaf``:
a binding, destroy, read, or store with a tracked leaf is the pass's,
everything else -- and everything outside a function body -- is judged at
parse time without flow state.  Each deferral is recorded
(``SemaProfiles::DeferredInitSite``) under the check's *analysis owner*: the
function, method, or block whose flow pass judges it -- the innermost one
enclosing the check, looking through captured regions
(``SemaProfiles::flowAnalysisOwner``).  One walk, ``forEachAnalyzedStmt``,
defines the statements of an owner's body for the harvest, the exemption
walk, and the constructor-body set: those the engine's CFG visits -- a
coroutine's user-written body, a constructor's written initializers, an
OpenMP directive's clause expressions and region, a lambda's capture
initializers but not its body -- so a local declared inside an OpenMP region
is tracked like any other.  The walk visits each statement once: the source of
an opaque value, which both the opaque value and its owner list, is not walked
again for each use.  After the pass, every recorded site
must have produced a Binding, Destroy, ReadThrough, or SubobjectWrite site
on one of its tracked leaves (``verifyDeferredInitSites``).  A leftover none
of whose leaves is an element of the engine's CFG lies in code the CFG does
not contain -- an argument of a builtin that does not evaluate it or of a
side-effecting ``__builtin_assume``, a ``[[assume]]`` assumption, an
unselected ``_Generic`` association or ``__builtin_choose_expr`` arm, an
OpenMP clause expression the directive evaluates through a helper, a
``#pragma clang __debug captured`` region's statements, a coroutine's parameter
copies and promise construction, or a body whose CFG cannot be built -- and is
dropped.  So is a leftover below a construct the CFG contains but the engine
does not enumerate (``collectUnenumeratedExprs``): an argument of a builtin with
custom type checking, an atomic builtin's operand, an Objective-C boxed
expression's operand, and a captured statement's capture initializer.  Apart
from a capture initializer, which binds nothing the user wrote, neither side
judges such a check (`Known Gaps (std::init)`_).  Any other leftover is an
enumeration bug: ``-fprofiles-verify-deferred-checks`` (``-cc1`` only; the
``std::init`` tests pass it) reports each one as an error, and in every build it
is then judged by form.  The record is verification only, never the source of
the pass's sites: TreeTransform reuses non-dependent nodes, which never re-enter
the funnels, so the CFG derivation stays authoritative and the invariant checked
is that recorded sites are a subset of judged ones.  No site is recorded where
no flow pass judges it -- an evaluation context other than a
potentially-evaluated one, a discarded statement, an immediate function context,
a dependent context (each instantiation records its own) -- and the record of a
body whose pass does not run is dropped (``Sema::PopFunctionScopeInfo``), as is
that of a body with an error that is not a profile violation, whose recovery can
discard the recorded expressions.  A defaulted function's synthesized body is
judged by neither side: it has no tokens of its own, so the parse-time checks
skip it as they skip a rebuilt default argument or initializer
(``SemaProfiles::inRebuiltOrSynthesizedCode``), and its flow pass never runs.
One walk enumerates the leaves of a shape
(``SemaProfiles::forEachTargetLeaf``) and one chain resolver interprets each
(``SemaProfiles::flowLeafShape``, shared by ``TrackedStorage::resolve`` and the
predicate), so no binding falls between the two.  Pointer arithmetic is an
element step there but by a literal 0, and so is the indirection of a
decayed array (``*(a + 1)`` is ``a[1]``, ``*(p + 0)`` is ``*p``, ``*a`` is
``a[0]``); a pointer arithmetic value
credited, destroyed, or bound to a marked target is resolved as the element
it points to (``flowLeafShape``'s ``Pointee``), while its other bindings are
not flow-tracked.  A destroy site drops a leaf naming an element of what a
pointer or handle refers to (``ArgLeaf::Element``) and judges no destroy
through an offset handle, which kills nothing.  A member entity is keyed by
its member path: per member step, the derived-to-base path from the step's
object to the member's class, then the member.  A class's tracked member paths
form a table (``SemaProfiles::flowTrackedMembers``), enumerated depth-first so
the paths under any prefix are contiguous, and an object's member entities are
that table in order; an anonymous struct's step is transparent, and an
anonymous union is never entered.  A class-type or array ``[[uninit]]`` member
is an entry tracked whole (``TrackedEntity::Whole``; a decayed array is the
array it decays from): it is initialized by a credit,
a written initializer, or a whole-object credit of its object, never by a
store, and a read or store below it is an access site, judged on every path
where the entity is read-tracked and trusted elsewhere -- also below a local's
member once the local escapes, as a scalar member's read is.  Outside its
constructors, the current object as a whole is an entity of its own
(``TrackedEntity::Kind::CurrentObject``) when the body destroys it or binds it
to a lifecycle parameter (``isCurrentObjectLifecycleUse``) or the parse-time
funnel deferred a marked binding of it (a body with none builds no CFG for it):
it enters initialized, its credits and destroys apply
beside its members', and an access below it is judged against it on every
path.  In every member function, each ``[[ref_to_uninit]]`` pointer member
reachable from the current object through members by value
(``SemaProfiles::markedPointerMembers``) has a ``MemberPointee`` entity for
its referent: it enters asserted uninitialized (``FlowState::EntryAsserted``,
which only the destroy judgment reads) unless a constructor's initializer
reseats it -- its written initializer, its default member initializer, or the
written initializer of a member subobject or base holding it, by the braced
element naming it or else to an unidentified referent; a store to the member
reseats it; and a member call on the object holding it, a pointer or
reference binding of that object, and a ``this`` capture escape it.  A
class-type ``[[uninit]]`` local's storage, and a marked local pointer's,
reference's, or pointer member's referent, carries one ``SubMember`` entity
per flow-tracked member of its class (the same table), when its
default-initialization runs no user-provided constructor
(``TrackedStorage::addSubMembers``): every event on the storage applies to
them too, except a default-initialization's credit
(``CalleeLifecycleRoles::DefaultInitCredit``), which leaves the storage's
members pending (``FlowState::MembersPending``), and a store or credit naming
one writes it alone; a read below the storage takes the worse of the storage's
and the member's state -- through a base view, the derived class's SubMember
for the same member; an ``[[uninit]]`` member of storage with no SubMember for
it (a ``void`` referent) is uninitialized while members are pending, until a
store naming one such member -- and a whole copy the worst of all of them.  A
marked binding is judged by the storage alone, or, when it names one SubMember
exactly, by that member; an unmarked one of live storage whose SubMembers
alone are unassigned is Unknown unless it names one exactly.  The enumeration
enters a *container* -- a named, unmarked, non-reference, non-array member with
no default member initializer whose default constructor is not user-provided
(``SemaProfiles::getDefaultInitTrackableClass``) -- up to eight member steps
deep and until 64 paths and containers are visited, so a credit, kill, or
written initializer of a container covers the contiguous range under it, and a
value copy of it (``FlowLeafUse::ValueCopy``) reads the range on every path.
In a constructor body the current object's containers are tracked whole like a
local's.
One matcher (``SemaProfiles::matchFlowTrackedMember``) finds the entry an
lvalue's member steps start with, for both the predicate and the resolver.  For
a marked target the predicate also takes a directly named local or by-value
parameter with tracked members as a whole, or a subobject enclosing some of them
(``SemaProfiles::flowTrackedMemberRange``), when its default-initialization is
trivial, and the pass classifies that leaf from the member entities below it
and below any base its casts convert it to (``TrackedStorage::objectPart``,
``BindingLeaf::Range``): Initialized when every one is ``Must``, Uninitialized
when none is ``May``, and Unknown otherwise.  An unmarked binding of it keeps
the form's answer: a copy, an escape, or a placement ``new`` of the object is
judged by form.  A marked pointer or reference bound to it gets an
unidentified referent (``ReseatTarget::Unknown``).
The walk distributes each
conditional at every step of the chain, not only at the top -- also behind an
implicit handle local or an unmarked local reference to a handle -- and
records the arm each leaf takes (``SemaProfiles::ShapeLeaf``), which every
interpreter follows: a walk that stopped at a nested conditional while the
recognizers saw through it would send tracked storage to the marker-only form
judgment.  The
standard identity functions (``std::move``,
``std::forward``, ``std::forward_like``, ``std::move_if_noexcept``,
``std::as_const``) are a step of that walk, recognized by builtin ID
(``SemaProfiles::isShapeTransparentBuiltin``), and a call to one binds nothing:
judging its argument's binding to the parameter too would hand ``std::move``'s
``T &&`` a mutable alias of a marked pointer, which makes the pointer's referent
unknown, so ``*std::move(p)`` would read nothing.  An opaque value is its
source: the CFG evaluates the source once, where the pseudo-object, array-copy,
``?:``, or ``co_await`` expression puts it; each use -- an element copy, an
accessor call, an awaiter call -- is a site of its own on that storage, and the
walk descends the source the same way.  Disagreeing arms are kept apart rather
than collapsed to an unidentified referent, which would lose the mix of tracked
and untracked arms P4222R2 §4.9 rejects.  A templated body is never analyzed;
each instantiation is analyzed on its own CFG, in which statements TreeTransform
reused from the pattern are ordinary elements.  Consumers read the lattices as
follows: ``double_destroy`` fires on ``MayDestroyed``, a marked-target binding
on ``Must``; an unmarked-target binding and ``destroy_uninit`` are suppressed
by ``May``; a read through a marker or of a subobject of an ``[[uninit]]``
object, a subobject write, and the unmarked binding of a value copy's source
(``isValueCopySource``; ``BindingSite::EveryPath``: a whole copy reads every
subobject, so it is trusted below a member whose reads are, and by form it
ignores data member markers as a read does) fire on ``MayUnassigned`` without
``Must``: every
path must initialize the storage (P4222R2 §1.3, §9.2), and a path that does
not know its state does not count against the read, just as an unidentified
referent fires in neither direction.  A destroy or release whose arms name
different storage leaves each possibly assigned, possibly unassigned, and not
destroyed; arms that all name one storage destroy or release it.  A mutable
alias of a marked pointer handed out gives the pointer an anonymous referent of
unknown state (``May`` set, ``Must`` and ``MayUnassigned`` clear).  A variable
of an enclosing function reached by capture, whose state the enclosing body
decides, enters in that unknown state, so no binding, destroy, or read fires on
it until the body itself stores to it or reseats it.  A local copied or moved
from a read-tracked local starts in the source's per-member state at the copy
point: a copy does not inherit initialization, it copies the source's bits,
initialized or not.  A credit, store, or access designates a type -- a store's
or access's glvalue type, a call credit's parameter pointee or referenced type
-- and covers an entity whole only when no derived-to-base step intervenes and
the type agrees with the entity's (``designationCovers``: canonical and
unqualified, same-size signed and unsigned integer variants alike, an array by
its base element type -- and by its extent, ``designationExtent``, when the
designation is an array -- a ``void`` referent by any type); otherwise it
designates a subobject, which earns no credit and is a subobject store or read
(P4222R2 §4.2, §5.4).  A ``[[now_init]]`` or initializing parameter whose
pointee is ``void``, ``char``, ``unsigned char``, or ``std::byte`` claims the
bytes and covers any entity it reaches without a derived-to-base step.  A
Construct row whose callee takes constructor arguments
(``constructsOneObject``) designates one element, ``T[1]``, and a marked
pointer to a base class reaches only the base subobject of a referent of a
derived class.  A marked pointer's or reference's referent is checked
against its anonymous referent's type at extraction and against the storage it
refers to at each transfer (``DefAssignEvent::Designated``,
``BindingLeaf::WholeIf``), and an object a credit takes as a whole is credited
only below the base subobject its argument converts to.  A handle binding
(`Handles (std::init)`_) is the pass's when a leaf below its walk's
``PointerValue``, ``Storage``, or ``Range`` leaves is tracked, as a pointer
value or a glvalue, or a ``Handle`` leaf names a tracked handle; its other
leaves are classified by form (``classifyInitHandleLeaf``).  A marked local
handle (``SemaProfiles::isFlowTrackedHandleVar``) is a ``Pointee`` entity like
a marked pointer, its referent of the handle's value type
(``SemaProfiles::handleValueType``).  A member call deriving a pointer or
reference from it (``SemaProfiles::isHandleDerefCall``) is the chain's
through-pointer step (``FlowLeafShape::ThroughHandle``, which
``SemaProfiles::handleChainBase`` takes down to the variable through
self-returning calls, copies, a std-table construction's first transfer
argument -- a handle built over a marked pointer reaches the pointer's
referent, over storage an element of it -- and implicit locals and unmarked
local references not reached by capture), shared by the predicate and the
resolver as every other step is: ``*up``, ``up->m``, ``*it``, ``up.get()``,
and ``s.data()`` name the referent whole, also through a prefix
(``s.first(2)``; a credit through it is partial, and a destroy through it is
judged against the referent but records nothing, so a second destroy or a
read after it is missed), and ``s[i]``,
``front()``, ``back()``, an accessor with arguments, or a handle past its
referent's start an element,
which, as a marked pointer's element (P4222R2 §5.4), is never tracked and is
judged by its form.  A handle's initializer, assignment, or ``reset`` with an
argument reseats it to what its source's walk names (a tracked handle's
referent, or storage a transfer leaf names whole); a ``reset()`` or an
assignment of a non-class gives it an anonymous referent; ``++``, ``--``,
``+=``, ``-=``, and ``std::advance`` keep it but mark it offset
(``FlowState::Offset``), so a credit or destroy through it is partial; and
any other non-const member call -- but one deriving a pointer or reference,
and ``release()`` -- or a non-const reference or pointer bound to it escapes
it, except the source of a copy or move, a transfer argument, and a standard
library function's reference parameter other than ``std::swap``'s,
``std::exchange``'s, and ``std::iter_swap``'s.  A member call on a conditional
naming several handles leaves each referent unidentified, and a destructor
call on a handle ends the handle object only.  An implicit local hands out no
alias.  A
credit through a handle argument reaches the referent of a ``Handle`` leaf that
is not ``Partial``.  The remaining blind spots are listed in
`Known Gaps (std::init)`_.


.. _profiles-init-known-gaps:

Known Gaps (std::init)
======================

The ``std::init`` slice is conservative in both directions: it rejects some
correct code that it cannot prove safe, where ``[[profiles::suppress]]`` is
the escape hatch, and it accepts some flows it does not check, which never
cause a rejection.

Deliberate Strictnesses
-----------------------

- Inside a constructor body, only a plain assignment to an ``[[uninit]]``
  member (including through a transparent reference cast such as
  ``(int &)m = 1`` -- the same casts the recognizers see through)
  or a call to a ``[[now_init]]`` function counts as its
  initialization -- the latter for the current-object storage bound to the
  callee's ``[[ref_to_uninit]]`` parameters (``&m``, ``m``, or ``this``
  itself, which credits every tracked member), as a genuine dataflow fact,
  so a ``[[now_init]]`` call on one branch still does not satisfy a read at
  the join (P4222R2 §1.3).  A ``[[now_uninit]]`` call is the credit's
  destruction twin: a real kill bit for the same storage shapes, so a
  destroy on one branch already spoils a read at the join, even though the
  destroy may not have run.  Taking the member's address, binding a
  reference to it, calling a member function, letting ``this`` escape, or
  passing ``&m`` to a ``[[ref_to_uninit]]`` parameter of an *ordinary*
  function earns no credit, so a later read of the member is rejected: the
  paper rejects complex constructor code (P4222R2 §5.1) and credits a
  callee's initialization only through a function marked ``[[now_init]]``
  (P4222R2 §6); the remedy for an intended flow is ``[[now_init]]`` on the
  callee or ``[[profiles::suppress]]``.
- A by-value *parameter* of a tracked class is a copy of the caller's
  argument, so its marked members are tracked from an unassigned start: a
  read before a local assignment (or an escape of the parameter) is
  rejected even if the caller assigned the member first (see "Reads of
  Uninitialized Objects" in :doc:`ProfilesFramework`).  That is the
  call-boundary twin of the constructor-body strictness: the paper hands
  uninitialized-capable storage across calls through marked pointers and
  references (P4222R2 §4.3), not by-value slots.  A parameter that declares
  a default argument is not tracked: a defaulted call initializes the
  parameter object directly (guaranteed elision -- no copy).  A *const*
  by-value parameter cannot be assigned locally at all, so no code change
  can satisfy the analysis -- suppression is the only remedy there.
- A ``this``-capturing lambda might run immediately, so member reads in
  its body count at the point the lambda is created, and writes there earn
  no credit (see "Reads of Uninitialized Objects" in
  :doc:`ProfilesFramework`): a lambda that is only ever run later, after
  the members are assigned, is rejected the same way, and so is a generic
  one, in each of its call operators instantiated by the end of the
  constructor body.  A by-copy capture of a tracked local (``[a]``,
  ``[b = a]``) copies it at the lambda's creation, so the member reads of
  the copy in the body count there too, and a write in the body earns the
  copy no credit for them.  A ``*this`` capture
  copy-constructs the whole object at the lambda's creation, reading every
  member right there: it is rejected before every tracked member is
  assigned, and its body accesses go to the copy, not the original.
- The flow-based read checks -- of a plain local, a member, a subobject of an
  ``[[uninit]]`` object, and through a ``[[ref_to_uninit]]`` pointer or
  reference -- promote every use a path *may* reach uninitialized to the profile
  error, path-insensitively: with two correlated conditions
  (``if (c) x = 1; ... if (c) use(x);``) the read is rejected although no
  executable path reads uninitialized memory -- the shape ``-Wuninitialized``
  reports as only "may be uninitialized" is a hard error under the profile.
  Destroys over-flag the same way: with correlated conditions
  (``if (c) destroy_at(&m); ... if (!c) use(m);``) the member read is rejected
  although no executable path reads destroyed memory.  The subobject-write check
  over-flags the same way
  (``if (c) std::construct_at(&s, v); ... if (c) s.x = 1;``).

- Destroying through a ``[[ref_to_uninit]]`` pointer that was never stored
  through is rejected by ``destroy_uninit``: the marker asserts an
  uninitialized referent at entry or at its latest marked binding.  If a
  helper filled the referent first, mark the helper ``[[now_init]]``, store
  through the marker before the destroy, or suppress.
- Passing ``&u`` to a ``[[ref_to_uninit]]`` parameter of an *ordinary*
  function initializes nothing (see the constructor-body entry above), so a
  later ``destroy_at(&u)`` is rejected by ``destroy_uninit`` even if the
  callee filled the storage -- the same strictness, and the same remedies,
  as the unmarked-direction binding after such a call: ``[[now_init]]`` on
  the callee or suppression.
- Storage destroyed whole stays destroyed until it is initialized whole:
  constructing its elements one by one after ``std::destroy(arr, arr + 2)``
  leaves it destroyed, so its scope-exit destructor or a second whole destroy
  is rejected by ``double_destroy``.  The remedies are a whole-range
  ``std::uninitialized_*`` credit or suppression.  A credit through an
  argument naming several storages initializes none of them whole either:
  after ``std::destroy_at(c ? &a : &b)``, ``std::construct_at(c ? &a : &b)``
  leaves both destroyed on some path, so both scope-exit destructors are
  rejected.
- Member-wise initialization of a class-type referent through
  ``[[ref_to_uninit]]`` is rejected even for trivially-copyable referents
  (``ptr->x = 5``): below a member step only whole-object ``construct_at``
  could initialize, which is unmodeled.  The remedies are ``construct_at``,
  a whole scalar write for a scalar referent, or suppression.  One corner
  is suppress-only: an element access (``ptr[i].x``) is not flow-tracked, so
  no prior fill legalizes it.
- A marked pointer member's referent is asserted uninitialized at the entry of
  every member function other than a constructor that initializes the
  member: a read through it is rejected until the same body stores or
  constructs it (``std::now_init`` is the remedy for a referent another
  member function initialized), and a member call on the object (a const one
  too: its pointee is not const) leaves it unknown.
- Placement ``new`` into marked storage (``new (&u) T``) is rejected while the
  storage is uninitialized on every path reaching it -- the address binds an
  unmarked placement parameter, judged by flow like any binding, so a store on
  some path first makes it an ordinary placement -- and earns no credit;
  ``std::construct_at`` is the sanctioned spelling for constructing into
  ``[[uninit]]`` storage (Clang knows its lifecycle roles; see
  "Binding Pointers and References" in :doc:`ProfilesFramework`).

- A user-defined allocator's ``allocate`` called directly returns initialized
  memory (only ``std::allocator`` and ``std::allocator_traits`` are
  recognized); libstdc++ before C++20 declares ``allocate`` in an internal
  base, unrecognized; the remedies are a ``[[ref_to_uninit]]``-returning
  wrapper or suppression.
- The ``std::uninitialized_*`` credit reaches only what the whole-storage
  write reaches: an element-form range (``&arr[0]``, P4222R2 §1.3) or an
  offset class-type iterator (``s.begin() + 1``, ``std::next(s.begin())``)
  earns nothing, and a marked pointer's further elements (``p[1]``) are never
  credited.  ``std::fill`` assigns rather than initializes: into a
  still-uninitialized ``[[uninit]]`` array it is rejected like any unmarked
  binding (``std::uninitialized_fill`` and ``memset`` initialize).
- A marked handle's referent is one object, as a marked pointer's is, and an
  element through the handle (``s[i]``, ``s.front()``) is judged by its form,
  as ``p[i]`` is: a read of one is rejected even after the whole range is
  filled -- ``*s.data()`` or an unmarked copy of the handle reads it -- and an
  element store or ``std::construct_at(&s[i])`` initializes nothing, so a fill
  of the whole range is the way to initialize it.
- A reference to a marked handle that a lambda or block reaches by capture
  (``const auto &r = s; [&] { return *r.data(); }``) is judged by its form
  there, as the marked handle: the referent's state is the enclosing
  function's.
- ``__builtin_operator_delete`` binds its operand with no parameter
  declaration in sight, so the storage-release relaxation cannot recognize
  it: passing ``[[ref_to_uninit]]`` storage keeps the unmarked-direction
  error.  ``_aligned_free`` and ``reallocarray`` carry no Clang builtin ID
  and are likewise unrecognized; declaring such a function
  ``[[now_uninit]]``, with its pointer parameter marked
  ``[[ref_to_uninit]]``, is the workaround -- the parameter marker keeps
  ``destroy_uninit`` from rejecting the release of a buffer that was
  never written (a release function's contract, unlike a destroy's).
- A ``void*`` escape of a marked pointer's address (``&p``) is not
  recognized as a reseat: the referent stays definitely initialized, so a
  later marked binding through the pointer can be rejected.
- ``[[uninit]]`` on a type whose default constructor is explicitly defaulted
  *after* its first declaration is accepted only once the ``= default``
  definition has been parsed: a marker written between the class definition
  and the constructor's still sees a declared-but-undefined constructor and
  is rejected as running a constructor.
- A ``construct_at`` or ``[[now_init]]`` credit of a base subobject of an
  ``[[uninit]]`` local or a marked pointer's referent, and a store through a
  narrower type, initialize nothing: the storage is one entity, so even the
  constructed base's own members stay uninitialized for a later read.  The
  remedies are a credit of the whole object or suppression.
- A store through a marked pointer bound to one ``[[uninit]]`` member of
  default-initialized storage (``int *pa [[ref_to_uninit]] = &p->a; *pa = 1;``)
  initializes the pointer's own referent, not the member, so a later read of
  ``p->a`` is rejected.  The remedies are a store naming the member
  (``p->a = 1``) or suppression.
- A class-type or array ``[[uninit]]`` member, and a member subobject holding
  tracked members, is initialized only whole -- ``std::construct_at``, the
  ``std::uninitialized_*`` family, a ``[[now_init]]`` call, ``memset``,
  ``memcpy``, ``memmove``, or a written mem-initializer: a class's assignment
  (``m = Mid{1}`` in a constructor body) is a member call, never an
  initialization, and a member-wise or element store is a subobject write
  (P4222R2 §4.6, §5.4, §5.5).  Outside its
  constructor, its bindings, member calls, and destroys are judged from an
  uninitialized start, like a scalar member's, because the constructor's
  initialization is not visible there (the P4222R2 §5 exposure analysis is not
  implemented).
- Every pointer or reference a member function of an uninitialized handle
  returns is presumed to refer to uninitialized memory -- the deleter
  ``up.get_deleter()`` included -- and every handle to the same memory a member
  returns (``s.begin() + 1``, ``s.subspan(1)``) inherits the handle's state:
  the library's members carry no marker.
  The remedies are a marked result or suppression.
- A constructor with a ``[[ref_to_uninit]]`` parameter makes its object a
  handle to the memory that parameter takes, so the object must be marked even
  when the constructor initializes that memory.  The remedy is a
  ``[[now_init]]`` static factory
  (``[[now_init]] static Filler make(int *p [[ref_to_uninit]]);``), which
  initializes the storage and returns an object referring to initialized
  memory.
- An unmarked reference to a handle is judged as the handle it refers to: no
  marker spells a reference to a handle that refers to uninitialized memory,
  so a marked handle whose memory is still uninitialized is rejected as a
  ``const R &`` or forwarding-reference argument (most of ``<ranges>`` and
  ``<algorithm>``, ``std::swap``), by a local reference, and by a
  by-reference capture.  The remedies are passing it by value to a marked
  parameter, or suppression.
- A base subobject cannot carry the marker, so a class deriving from a
  standard handle built over raw storage (``struct S : std::span<int>``) is
  rejected at its base initializer; a marked member is the remedy.  A base
  built from raw storage by an author-marked constructor (``D() : B(raw())``)
  is rejected there too; the remedies are a ``[[now_init]]`` factory of the
  base (``D() : B(B::make(raw()))``) or suppression.
- Storage that is not flow-tracked -- the referent of a marked pointer member
  of an object other than the current one (``o.p``, ``q->p``) -- is judged by
  its form alone at every read, write, and destroy: a ``construct_at`` into
  it legalizes no later access.
- A range-for over an ``[[uninit]]`` array or ``std::array`` binds the
  range to its implicit ``__range`` reference, an unmarked binding, so before
  the array is filled the loop is rejected there whatever its body does; a
  marked loop variable (``for (int &x [[ref_to_uninit]] : a)``) is rejected
  there and again as a marked binding of an element.  A fill of the whole
  array before the loop is the remedy.
- In a constructor, a binding of, or a destroy through, a member reached by a
  pointer-to-data-member step on the current object (``&(this->*pm).d``) is
  judged by its form, so it is rejected even after the member is constructed,
  while a read through the step is flow-judged.
- ``std::construct_at(&r[1], v)`` on a ``std::array`` that is not yet filled is
  rejected as a member call (``operator[]``) on uninitialized storage and
  again as a marked binding to initialized memory.

Missed Diagnostics
------------------

- A read of an ``[[uninit]]`` local declared inside an OpenMP region is not
  checked: the plain-local read analysis tracks only the variables the
  function declares directly, and a region's belong to its ``CapturedDecl``.
- The statements of a ``#pragma clang __debug captured`` region are not
  flow-checked: the CFG does not contain them, so their bindings, destroys,
  reads, and stores of flow-tracked storage are accepted.
- A marked pointer whose referent the analysis cannot identify -- a copy
  (``int *p2 [[ref_to_uninit]] = q;``) whose source ``q`` is later reseated to
  an untracked source, a conditional initializer whose arms name different
  storage (``c ? &u : &v``), a store whose target names several pointers or a
  marked pointer on only some of its arms (``(c ? p : q) = &u``,
  ``(c ? p : r) = &u``), a ``__block`` pointer, a marked pointer or reference
  bound to a whole local with tracked members (``C *p [[ref_to_uninit]] = &c``,
  which also escapes the local) -- fires in neither direction through the
  pointer, a store or ``[[now_init]]`` call through it credits nothing, and a
  marked binding to it asserts nothing: a later ``int *w = &u`` stays the
  unmarked-direction error.
- The identity functions are recognized by builtin ID: under
  ``-fno-builtin`` (or ``-fno-builtin-std-<name>``) and for a user-written
  identity function (``template <class T> T &&fwd(T &)``), the call is an
  ordinary call -- its result is trusted, and a ``T *&`` parameter makes a
  marked pointer's referent unknown.
- A read of a plain ``[[uninit]]`` local through ``std::forward``,
  ``std::forward_like``, ``std::as_const``, or ``std::move_if_noexcept``
  (``return std::as_const(u);``), and a later read after such a call, is not
  diagnosed: the plain-local analysis counts the call as an escape that
  initializes the local (it recognizes ``std::move`` alone).
- A jump past a tracked local's declaration into its scope -- a ``goto`` to a
  label after it, a ``switch`` case label below it -- skips the declaration's
  ``Reset``, so in a loop the storage keeps the state the previous iteration
  left:
  ``while (n--) { if (c) goto m; { int u [[uninit]]; m: sink(&u); u = 1; } }``
  is accepted.
- An unmarked binding of storage initialized on some paths only is accepted,
  and reads through the unmarked name are trusted: after ``if (c) *p = 1;``
  through a marked ``p``, ``int *q = p; return *q;`` is accepted while
  ``return *p;`` is rejected.
- A read through a marked pointer or reference that a lambda or block
  captures is not diagnosed unless the body itself leaves the referent
  uninitialized on some path: the captured storage enters the body in an
  unknown state (``[&p] { return *p; }`` is accepted).
- ``construct_at``/``destroy_at`` flow is modeled through their known roles:
  ``construct_at`` checks the lifecycle start (double construction via the
  reverse-direction rule) and ``destroy_at`` the end (double destruction,
  use-after-destroy where the storage is *bound*, and a member read after the
  destroy).  A direct named read of a plain ``[[uninit]]`` *local* after a
  destroy stays with the local-variable analysis, which treats the call as an
  escape, so it is a missed diagnostic.
- Clang's knowledge of ``std::construct_at``, ``std::destroy_at``, and
  ``std::now_init`` applies where their parameter is a pointer, and of their
  ``std::ranges`` objects likewise; a call with a class-type argument checks
  nothing there.  The ``std::uninitialized_*`` family, ``std::destroy``, and
  ``std::destroy_n`` and their ``std::ranges`` objects take a pointer or a
  class-type iterator, or an array or class-type range.  A whole
  ``[[uninit]]`` array passed as a range is initialized or destroyed whatever
  the range's length.
- A ``std::shared_ptr``'s destruction is not judged (its ownership is
  shared), nor is a ``std::unique_ptr``'s reset or reassignment through a
  conditional naming several owners.
- A marked ``std::unique_ptr`` null on some path is no ``double_destroy`` at
  its reset, reassignment, destructor call, or destruction
  (``FlowState::MayNull``): a referent another path destroyed explicitly is
  rejected there only when it is uninitialized on every path
  (``destroy_uninit``).
- Storage of no type -- a marked ``void *``'s referent, such as a ``malloc``
  block -- is covered by a credit or store of any type: after
  ``std::construct_at((int *)v, 1)`` the whole block counts as initialized, so a
  read of a wider object through an unmarked alias, ``((Triv *)v)->y``, is
  accepted.
- After a default-initialization's credit of storage of no type viewed through
  a marked class pointer (``M *p [[ref_to_uninit]] = static_cast<M *>(v)``), a
  store naming one ``[[uninit]]`` member ends the pending state of every
  member: a later read of another one, still indeterminate, is accepted.
- ``memset``, ``memcpy``, and ``memmove`` initialize their whole destination
  whatever the byte count and the type: after ``memset(arr, 0, sizeof(int))`` a
  read of ``arr[3]`` is accepted, and a class object written by ``memcpy`` is
  credited although P4222R2 §6 asks for ``construct_at``.
- ``va_arg`` on an array or pointer ``va_list`` (x86-64, i386, Windows) binds
  nothing, so a ``va_arg`` before ``va_start`` is accepted there.
- An argument of a builtin that Clang type-checks itself (other than
  ``va_start``'s ``va_list``: ``__builtin_launder(&u)``,
  ``__builtin_align_up(&u, 16)``, an atomic builtin's value operand), an
  Objective-C boxed expression's operand, and a coroutine parameter passed to
  its promise's constructor are not checked when they name flow-tracked
  storage.
- ``va_end`` ends nothing: a ``va_arg`` or a second ``va_end`` after it is
  accepted.
- ``allocate_at_least`` returns a class and is unrecognized.
- A function body that contains an Objective-C ``@finally`` or an SEH
  ``__finally`` anywhere has no CFG (``CFGBuilder`` models neither), so none of
  its flow-based checks run: reads of ``[[uninit]]`` locals and of tracked
  members before their initialization, and every binding, destroy
  (``destroy_uninit``, ``double_destroy``), read through a marker, and subobject
  store of flow-tracked storage are accepted -- ``verifyDeferredInitSites``
  drops the body's deferred checks.  A lambda or block inside such a body is
  analyzed on its own, and the parse-time checks -- the declaration and
  constructor rules, and every check of storage that is not flow-tracked --
  still apply.
- For *locals*, the flow engine (for a local aggregate's members) and the
  uninitialized-variables analysis (for a plain local) treat any escape of
  the variable as an assignment (member accesses are not escapes, including
  accesses through anonymous aggregates and below a class or array member
  whose type holds no pointer or reference; an access through a member that
  does hold one is an escape, as is one below a pointer-to-data-member step,
  ``(a.*pm).x``; nor is the source of a tracked copy or of a by-copy
  capture), so a read after the escape is accepted.  The
  paper credits a callee's initialization only through a function marked
  ``[[now_init]]`` (P4222R2 §6).
- A destroy of *unknown* storage is accepted (``destroy_uninit`` fires only
  on an affirmative classification), and so is a destroy after a merely
  *conditional* store: storage initialized on some path is accepted, short
  of the paper's "for acceptance all alternatives must provide the desired
  solution" (P4222R2 §9.2).  Reads are stricter: a destroy on any path
  spoils a read at the join (P4222R2 §1.3); acceptance errs away from false
  positives, reads toward the paper.
- A reinitializer's exemption from ``destroy_uninit`` is call-wide, so its
  *unmarked* destroy-only pointer parameters -- whose storage the paper
  requires live -- are exempt too; a parameter that itself carries
  ``[[ref_to_uninit]]`` is exempt per parameter (the release-function
  spelling under Deliberate Strictnesses).
- Under ``-fno-builtin`` or ``-ffreestanding``, a ``free`` or ``realloc``
  recognized by name accepts a pointer in any state but records no
  release, so a later read or destroy through the pointer is not diagnosed
  there.  The acceptance survives by name because accepting a pointer never
  diagnoses; the release does not, because later diagnostics fire on it.
- The read analysis of a plain ``[[uninit]]`` local considers no exception
  paths: after ``try { x = f(); } catch (...) {}`` a read of ``x`` is
  accepted although the handler path skips the assignment.  The flow engine
  considers them.
- An element write through the marker (``p[3] = 0``) is accepted and never
  credited -- a gap against the paper's random-access ban (P4222R2 §1.3,
  §9.2); no read is legalized by it.  The asymmetry with ``[[uninit]] int
  a[2]; a[0] = 1;`` -- rejected under the same paper rule -- follows the
  paper's own span example (P4222R2 §4.6), which accepts the constant-index
  element initialization and flags only reliance on the partial state.
  Rejecting the element write too would turn every typed raw-buffer fill
  loop through a marked pointer into a hard error, with range algorithms,
  ``construct_at``, or suppression as the only remedies (``std::byte``
  buffers excepted, P4222R2 §4.6).
- A destroy of an element of what a marked pointer or handle refers to --
  ``std::destroy_at(&p[i])``, ``std::destroy_at(p + i)``,
  ``std::destroy(p + 1, p + n)``, a destroy through an offset iterator -- is
  not judged and destroys nothing tracked, as an element credit initializes
  nothing: an element never constructed, or destroyed twice, is destroyed
  without a diagnostic (P4222R2 §5.4's element form).
- The value of pointer arithmetic bound to an unmarked target is not
  classified, so that an end iterator binds in any state:
  ``int *q = p + 1;`` through a marked ``p``, or ``int *q = a + 1;`` before an
  ``[[uninit]]`` array is filled, is accepted, while ``&p[1]`` and ``&a[1]``
  are rejected.
- An offsetting Derive call over pointer arithmetic through a marked ``p``
  (``*std::prev(p + 2)``, ``*std::next(p + 1, -1)``) reads unjudged: the
  call's argument is an arithmetic value, which is not classified.  Without
  the arithmetic, ``std::next(p)`` as a value is ``p``'s referent: an
  unmarked ``int *q = std::next(p);`` is rejected before a fill and accepted
  after it, where ``int *q = p + 1;`` is accepted in any state.
- A ``new`` expression whose result is not bound to anything (``new int;``)
  is not checked.
- Outside the contexts whose member reads are flow-checked (a constructor
  body, a constructor-less aggregate local or by-value parameter) and past the
  depth and path caps of the member enumeration, a read or store of an
  ``[[uninit]]`` member or below it is trusted: whether a
  constructor initialized the member before the object is exposed
  (P4222R2 §5) is not checked.
- A ``delete`` expression is judged as a destroy only when its operand is
  flow-tracked: through a global or another object's marked pointer member,
  ``delete p`` of never-constructed storage is not ``destroy_uninit``, while
  ``p->~T()`` is.
- A destroy of a marked pointer member's referent that only the entry
  assertion calls uninitialized is accepted (the destructor's
  ``std::destroy(elem, elem + n)``), so a member function that destroys a
  referent nothing constructed is not ``destroy_uninit``; a second destroy in
  the same body is ``double_destroy``.
- The member-wise copies of a defaulted copy or move function and the member
  calls inside a defaulted comparison are not judged; only the binding of the
  source object at the defaulted function's use is.
- A call through a function pointer cannot see parameter markers on the
  pointed-to function, and a member call through a pointer-to-member bypasses
  the object-argument check.
- Members of anonymous unions and arrays of aggregates are not flow-tracked,
  nor are members more than eight member steps deep or inside a member
  subobject entered after a class's 64th tracked member or member subobject.
- ``[[uninit]]`` members of locals copied from untracked sources (a call
  result, a member other than a container, an element) are not flow-tracked: a
  copy does not inherit initialization -- it copies indeterminate bits -- but
  the source's per-member state is unknown, so reads of such members are
  trusted (a missed diagnostic, never a false positive).  Copies of tracked
  locals and by-value parameters, by-copy captures included, *are* tracked
  (see "Reads of Uninitialized Objects" in :doc:`ProfilesFramework`).
- A by-value parameter with a default argument is not tracked (see the
  by-value entry under Deliberate Strictnesses), so an *explicit* call
  passing an uninitialized copy into such a parameter is not detected --
  reads of its marked members in the callee are trusted.
- A ``*this`` capture in a constructor of a class with a *user-provided* copy
  constructor reads no member at the lambda's creation, although a copy of an
  object elsewhere reads its source whole whatever its copy constructor.
- A ``this``-capturing *generic* lambda created in a constructor counts the
  member reads of its call operators instantiated by the end of the
  constructor body only: one the constructor never calls, or calls only
  through a function template instantiated later
  (``template <class F> void run(F f) { f(1); }``), counts none.
- An *alias* of ``this`` in a constructor body -- a stored
  ``T *self = this;``, an init-capture ``[p = this]``, ``&*this`` -- is
  invisible to the flow engine: reads through the alias are not attributed
  to the members, and writes through it earn no credit (that half is a
  strictness, never a false positive).
- Virtual base subobjects are not checked by ``ctor_uninit_member`` (they are
  initialized by the most-derived class).
- The scope-exit destructor of an ``[[uninit]]`` local is judged on the
  normal paths only: the CFG has no destructor elements on exception edges,
  so ``WD w [[uninit]]; mayThrow(); std::construct_at(&w);``, whose unwinding
  destroys the never-constructed ``w``, is accepted.
- The implicit destruction of members and bases at the end of a destructor
  is not judged: an ``[[uninit]]`` member of a class type with a non-trivial
  destructor may be destroyed although it was never constructed, which only
  the P4222R2 §5 exposure analysis, not implemented, could tell.
- A read of a tracked member inside another member's default initializer is
  not detected.
- A store through an alias of a pointer object (``int *&r = p; r = &u;``,
  ``*pp = &u``) is not checked: the alias cannot carry the target's marking.
- An assignment through a conditional, comma, or GNU ``?:`` target whose
  arms differ in marking accepts either source: any single answer would
  reject a legal combination.
- A second ``destroy_at`` of storage that is not flow-tracked (the referent
  of another object's marked pointer member) is not a double destroy.
- A pointer passed through a non-const reference to it -- a forwarding
  reference (``v.emplace_back(raw, 4)``,
  ``std::make_unique<std::span<int>>(raw, 4)``) -- binds the pointer object,
  not its value, so the handle the library builds from it is never judged.
- A discarded temporary handle (``std::span<int>(arr, 4);``) is never judged.
- Views and adaptors outside the std handle table -- ``std::views::*``,
  ``std::ranges::ref_view``, ``std::mdspan``, ``std::string_view`` -- and
  aggregates that hold a pointer refer to initialized memory as far as the
  analysis knows: raw storage enters them only through ordinary parameter
  bindings, and one a library function builds from a marked handle
  (``std::views::take(s, 2)``) is trusted, so reads through it are accepted.
