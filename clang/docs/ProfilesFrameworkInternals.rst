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
`Enforcement State`_.

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
class rather than plain ``Error``.  A rule diagnostic is a ``Warning``-class
diagnostic mapped to an error by default, so that it belongs to a diagnostic
group (an ``Error`` cannot) while a violation remains an error that ``-w``
does not silence.  The ``Warning`` class also marks it SFINAE-suppressed:
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
3. Add the check: a ``checkProfileViolation`` or ``EmitProfileRuntimeCheck``
   call at the check site (patterns 1 and 5), or a row in the analysis's
   opt-in table (patterns 2-4).
4. Add tests; a test-only profile must be named under ``test::`` (see `Test
   Profiles`_).

Enforcement, suppression, module propagation, and serialization then work
without further profile-specific code.


Pattern 1: Parse-Time Check Sites
=================================

For a rule checkable at a single semantic entry point, the entire profile
implementation is one call at that site:

.. code-block:: c++

   checkProfileViolation("my::profile", "my_rule", Loc,
                         diag::err_my_profile_rule);

The call runs the single violation gate,
``SemaProfiles::shouldEmitProfileViolation``: the shared
enforce/exempt/suppress ladder ``profiles::shouldEmitProfileViolation``
(``clang/AST/Profiles.h``; CodeGen's runtime checks run it as is) plus the
parse-time rungs -- a templated declaration, an unevaluated context, and a
discarded statement never fire.  Every pattern's check site goes through
that one gate.  ``test::type_cast`` is the in-tree example.

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


Pattern 2: Post-Parse / CFG-Based
=================================

For a rule that needs whole-function analysis.  Each post-parse analysis owns
a small opt-in table of the profiles that ride it, one row per profile
(profile name, rule name, diagnostic); the framework never learns the
profile's name.  ``test::uninit_read`` is the in-tree example:

.. code-block:: c++

   constexpr CFGProfileEntry CFGProfiles[] = {
       {"my::profile", /*Rule=*/"", diag::err_my_profile_rule},
   };

The analysis's diagnostic reporter walks the table calling the single gate
as ``shouldEmitProfileViolation(Name, Rule, Loc, /*D=*/nullptr, Stmt,
&AnalysisDeclContext)`` per use site, emitting the entry's diagnostic (and
skipping the default warning) when it returns true.

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

That overload is the post-parse counterpart of the parse-time suppress
stack: by the time the analysis runs the stack has unwound, so it walks the
AST upward from the use site -- enclosing ``AttributedStmt``\ s,
``DeclStmt``-declared variables, and the lexical ``Decl`` chain -- for a
matching ``[[profiles::suppress]]``.


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
one profile's policy rather than the pattern's contract lives in that
profile's callback.  Each callback gates its diagnostics on
``shouldEmitProfileViolation`` with the finalized declaration, whose lexical
chain is walked for a suppression.

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
profile implementation is one call at the check site -- the runtime
counterpart of pattern 1's ``checkProfileViolation`` one-liner -- naming the
profile, the rule, and a trap diagnostic (``Trap`` class,
``DiagnosticTrapKinds.td``, category "C++ Profiles"), and passing the
check's "no violation" predicate as a lazily-invoked builder.  The site
guards on its own applicability conditions; the integer div/rem site is the
in-tree pilot:

.. code-block:: c++

   if (Ops.Ty->isIntegerType() && Ops.mayHaveIntegerDivisionByZero())
     CGF.EmitProfileRuntimeCheck(
         "test::arith", "zero_divide", diag::trap_profile_zero_divide,
         Ops.E->getExprLoc(), [&] {
           return Builder.CreateICmpNE(
               Ops.RHS, llvm::Constant::getNullValue(Ops.RHS->getType()));
         });

``EmitProfileRuntimeCheck`` checks that the rule is enforced at the check
site (its trap diagnostic's mapping there, ``ASTContext::isProfileRuleActiveAt``,
so a body deserialized from an AST file is decided by the state its own unit
recorded), that the given location is not in an exempt system header, and
that the rule is not suppressed for the code being emitted (the CodeGen
suppression state above).  Only when the
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


Enforcement State
=================

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

The engine's own rungs then apply: ``-w`` keeps an enforced rule because the
diagnostic's default mapping is an error, ``-Weverything`` leaves the
ignored rules alone because their initial ignore is a user mapping
(``ProcessWarningOptions``), and ``-Wprofile-...`` and the diagnostic pragmas
move a rule's severity like any other diagnostic's.  Enforcement therefore
travels with the diagnostic state: through a PCH or preamble as the state
the main file continues from, through a BMI compiled to object code as the
unit's own file transitions, and through an imported module as transitions
installed for the module's files -- so module code is checked under the
module's enforcement wherever it is instantiated or emitted, never under
the importer's, and importer code never under the module's.  A module-map
module (``-fmodules``) has no initial state of its own and takes the
importer's, so the importer's ``-fprofiles-enforce=`` reaches it.


Suppression Dominion Mechanics
==============================

A ``[[profiles::suppress]]`` attribute's dominion is the token range of the
construct it appertains to (the user-level rule is stated in
:doc:`ProfilesFramework`).  The parse-time suppress stack -- pushed and
popped by ``ProfileSuppressScope`` RAII guards in the parser and the
template-instantiation machinery -- enforces this positionally: each entry
records its construct's token range, and a violation matches an entry only if
its location falls within that range.  This keeps a live suppress scope from
leaking into code its tokens do not cover, which would otherwise happen in
two ways: a template pattern instantiated synchronously while the scope is
live (instantiated code retains the pattern's source locations), and a class
or constructor finalized as a side effect of such an instantiation.
Conversely, a local class or lambda defined *inside* the suppressed construct
is covered, whichever path re-enters it.

A declaration's guard is pushed before its decl-specifier-seq is parsed, so
the dominion covers a class or enum defined there -- NSDMIs and late-parsed
member bodies included.  A block-scope declaration is covered twice, once by
the enclosing statement's guard and once by its own; the duplicate entries
are harmless, since any matching entry suppresses.

A suppression written on a *declarator* rather than in the declaration's
prefix reaches the parse-time stack only once the declarator's Decl exists,
so the parser pushes a second, Decl-keyed guard at each point where a
freshly created Decl's initializer or default argument is about to be parsed
(or instantiated): the declarator initializer
(``Parser::ParseDeclarationAfterDeclaratorAndAttributes``), an immediately
parsed default argument (``Parser::ParseParameterDeclarationClause``), a
late-parsed member default argument
(``Parser::ParseLexedMethodDeclaration``), an instantiated default argument
(``Sema::SubstDefaultArgument``), and a condition variable
(``Parser::ParseCondition``).  Declarator-id attributes are the one case
where the tokens to cover precede the Decl -- the parameter clause is parsed
before the function is declared -- so ``Parser::ParseDirectDeclarator``
pushes them from the ``Declarator`` itself, which makes the prefix,
declarator-id, and member declarator-id spellings agree on a declaration.

An enum body gets the same two-guard treatment in
``Parser::ParseEnumBody``: a Decl-keyed guard covering the body from the
enum-head's attributes (mirroring the class and namespace body guards), and,
per enumerator, a guard around the initializer parse built from the
enumerator's parsed attributes -- its ``EnumConstantDecl`` is created only
after the initializer, so the attribute-keyed constructor is the one that
works, exactly as for statements.

The range's end is recorded only when the construct was already fully parsed
when the entry was pushed.  For a construct still being parsed no end exists
yet (a mid-parse end location would be misleadingly early), so the entry's
scope lifetime bounds the dominion instead -- exact mid-parse, because the
construct's later tokens do not exist yet, and instantiation of a template
that has no definition yet is deferred past the scope's death.

Suppression written on a template pattern or its lexical parents is
re-established around instantiation, so it applies to instantiated code.  The
reverse is not propagated: a scope live at the *point of instantiation*
covers the trigger's tokens, not the pattern's, and the positional match
above keeps it from suppressing checks inside a synchronously instantiated
body, NSDMI, default argument, or marker re-check.

For the same reason ``ProfilesSuppressAttr`` is deliberately not inherited
by redeclarations (``mergeDeclAttribute`` skips it): each redeclaration's
tokens form their own dominion, so a suppression written on a previous
declaration does not cover the definition.

Every consumer of suppression state builds one ``profiles::SuppressionQuery``
-- a stack of live entries, a statement walked upward through a
``ParentMap``, and a declaration whose lexical chain is walked -- and
``profiles::isSuppressed`` composes the sources; the composition order
(stack, then statement walk, then chain) is the query's, not the caller's,
and is not observable, since any match suppresses.  The parse-time gate
queries the live stack and the checked declaration.  The post-parse gate
used by the CFG passes adds the two sources that cover the analyzed
function's own interior -- the use statement's enclosing statements and the
function's lexical declaration chain -- and keeps the live parse-time stack,
which covers enclosing constructs still mid-parse.  The latter matters
because a function can be analyzed before its enclosing construct finishes
parsing -- a local class's method body runs its CFG passes at the end of the
method, while the ``ProfileSuppressScope`` of the statement the class is
declared in is still live.  The stack consult is dominion-checked as above,
so an unrelated live scope never matches.

The AST walk reconstructs the same positional rule for a ``DeclStmt``: a
suppression there is attached to a ``VarDecl``, so the walk bounds it to that
declarator's tokens (``profiles::declaratorDominion`` -- the declarator-id
through the end of its initializer), keeping one declarator's suppression off
its siblings in a multi-declarator group.  A prefix suppression is attached
to every declarator by the attribute machinery, so per-declarator containment
reproduces whole-declaration coverage.  One accepted corner: decl-specifier
tokens precede every declarator-id, so the AST-walk consumers do not cover
them; expressions there do not execute in the enclosing function's CFG or IR,
and the parse-time stack still covers them at parse time.


Suppression During Code Generation
==================================

A profile check site that runs during IR emission needs suppression state
long after the parse-time stack has unwound, and it may be emitting a body
deserialized from an AST file that was never parsed in this compilation.
``CodeGenFunction`` therefore mirrors the post-parse walker's two-part
structure over the AST it is emitting:

- A *statement-suppression stack* (``ProfileStmtSuppressions``, of the same
  ``profiles::SuppressionEntry`` element type as Sema's parse-time stack,
  consulted through the same ``profiles::anyEntryCovers`` loop), pushed and
  popped by ``ProfileSuppressionScope`` RAII guards in every
  statement-emission path that can carry ``[[profiles::suppress]]``:
  ``EmitAttributedStmt``, ``EmitDeclStmt``, and the local-variable arm of
  ``EmitDecl`` (an if/while/for/switch condition variable is emitted
  directly, never through ``EmitDeclStmt``).  A declaration-carried entry
  records its owning declarator's dominion
  (``profiles::declaratorDominion``) and matches only check sites located
  inside it, so one declarator's suppression stays off its siblings in a
  multi-declarator group -- the same positional rule the post-parse walk
  applies; an AttributedStmt entry records no range, since the emitting
  scope's lifetime already bounds it.
- The lexical declaration chain, reached through the shared walk in
  ``clang/AST/Profiles.h``.  Because Sema propagates active suppressions onto
  lambda call operators as implicit attributes, the chain walk also recovers
  statement-level suppression around a lambda body.

``CodeGenFunction::profileSuppressionQuery`` builds the same
``profiles::SuppressionQuery`` as the Sema gate *lazily at each check
site*, and ``EmitProfileRuntimeCheck`` runs the shared ladder
``profiles::shouldEmitProfileViolation`` over it: the stack above its floor,
and the chain from the suppression anchor or, absent one, from
``CurCodeDecl`` (it has no statement to walk; the emitting scopes' stack
stands in for that source).  Whatever code is
being emitted -- a function, lambda, global dynamic initializer, coroutine
body, or OpenMP captured region -- ``CurCodeDecl`` is the declaration whose
chain carries its suppressions.  A null ``CurCodeDecl`` (synthesized
helpers such as block copy/dispose functions) carries none.  Lazy querying is a
correctness requirement, not a convenience: an inlined inheriting
constructor swaps ``CurCodeDecl`` mid-function without a ``StartFunction``,
so per-function seeding would apply the wrong declaration's suppressions.

The dominion rule -- a suppression covers only its construct's tokens -- is
enforced structurally rather than positionally: emitting an NSDMI
(``CXXDefaultInitExprScope``), a default argument
(``CXXDefaultArgExprScope``), or an inlined inherited constructor
(``InlinedInheritingConstructorScope``) raises ``ProfileSuppressionFloor``
to the current stack size, hiding the statement suppressions of the function
whose emission reached the construct, and the first two set
``ProfileSuppressionAnchor`` to the field or parameter whose construct the
emitted tokens belong to (the inherited-constructor scope needs no anchor;
it already swaps ``CurCodeDecl``).  For a default argument that anchor is
the parameter of the redeclaration that *wrote* it
(``ParmVarDecl::getDefaultArgOwningParam``), not the parameter of the
declaration the call resolved to, whose dominion holds no default-argument
tokens when it inherits the default.  A suppression written on the field,
parameter, or a lexical parent is honored through the anchored chain walk;
one written around the use site is not -- the same answer Sema's positional
dominion check gives.  Nesting composes automatically as the scopes save and
restore both values.

Known over-check-only gaps (a check that suppression fails to remove, never
a missing check):
member functions of a local class defined inside a suppressed *statement*,
ObjC blocks (no lambda-style implicit-attribute propagation exists for
``BlockDecl``), and C++26 structured-binding condition variables, whose
holding-variable initializer is emitted deferred, outside the variable's
suppression scope.


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
``DIAG_PRAGMA_MAPPINGS`` record (`Enforcement State`_).  Command-line
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

