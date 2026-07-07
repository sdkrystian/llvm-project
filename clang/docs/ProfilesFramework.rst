===========================
C++ Profiles Framework
===========================

.. contents::
   :depth: 3
   :local:


Introduction
============

The C++ Profiles framework (`P3589R2
<https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3589r2.pdf>`_) allows a
translation unit to opt into additional language restrictions called *profiles*.
A profile is a named set of rules enforced by the compiler. A translation unit
requests enforcement with ``[[profiles::enforce(...)]]``; individual
declarations or statements can suppress enforcement with
``[[profiles::suppress(...)]]``; and module imports can require that an imported
module enforces a profile with ``[[profiles::require(...)]]``.

Profiles do not change the meaning of well-formed programs with no undefined
behavior.  Their static semantic effects are conceptually applied only after
translation phase 7: a profile cannot change the outcome of overload resolution
or template instantiation, and it is not possible to SFINAE on a profile
violation.

The framework is profile-agnostic.  It handles attribute parsing, enforcement
tracking, suppression scoping, module integration, and serialization.
Individual profiles only need to call a single API at the appropriate semantic
check sites (``SemaProfiles::checkProfileViolation`` for parse-time checks, or
``SemaProfiles::shouldEmitProfileViolation`` from a per-pass dispatch table for
post-parse analyses).  Everything else -- suppression, template instantiation,
SFINAE exclusion, module propagation, and PCH/BMI serialization -- is handled
by the framework automatically.


Driver Flag
-----------

The entire framework is gated on the ``-fprofiles`` command-line flag, which
sets ``LangOpts.Profiles``.  The flag is C++-only (declared with
``ShouldParseIf<cplusplus.KeyPath>`` in ``clang/include/clang/Options/Options.td``)
and defaults to off.

.. code-block:: bash

   clang++ -std=c++23 -fprofiles example.cpp

Without ``-fprofiles``:

- ``[[profiles::enforce]]``, ``[[profiles::suppress]]``, and
  ``[[profiles::require]]`` are diagnosed as ``warn_attribute_ignored`` and
  have no semantic effect.
- Their argument clauses are **not** checked against the P3589R2 profile
  grammar: like any standard attribute the implementation does not act on,
  an arbitrary balanced-token argument clause -- or none at all -- is
  accepted, so code annotated for a profiles-enabled build compiles cleanly
  (modulo the warning) with the feature off.  P3589R2's grammar is enforced
  only under ``-fprofiles``.
- No profile rule check ever fires, even at sites that call
  ``checkProfileViolation``.

The framework's parse-time bookkeeping (``ProfileSuppressScope``, attribute
custom parsing, etc.) is also no-ops when ``LangOpts.Profiles`` is false, so
the flag is the single switch that turns the entire feature on or off.

The built-in ``test::`` profiles (see `Built-in Profiles`_) exist only to
exercise the framework and are additionally gated on the ``-fprofiles-test-profiles``
flag, which sets ``LangOpts.ProfilesTestProfiles``.  This flag is ``-cc1``-only
(not exposed by the driver) and is intended solely for running the test suite.
Under ``-fprofiles`` alone, ``[[profiles::enforce(test::...)]]`` is still
recognized (it is not ``warn_attribute_ignored``) and its designator is still
recorded and exported across modules, but ``SemaProfiles::isProfileEnforced`` reports
any ``test::``-prefixed profile as not enforced, so no ``test::`` rule ever
fires.  Real (non-``test::``) profiles are unaffected by this flag.


Attribute Reference
===================

The three attributes are spelled in the ``profiles`` scope and accept either
the ``[[profiles::name(...)]]`` or ``[[using profiles: name(...)]]`` syntax.
Each attribute requires an argument clause; ``[[profiles::enforce]]`` and
``[[profiles::require]]`` with no parentheses are diagnosed.

``[[profiles::enforce(profile-designator-list)]]``
   Allowed only on an *empty-declaration* at translation-unit scope or on a
   *module-declaration*.  At TU scope, it must precede every non-empty
   declaration in the translation unit.  Each profile-designator is a
   ``::``-separated identifier sequence optionally followed by an
   argument-clause (e.g. ``vendor(fortify: 3)``).  Repeating the same name
   with the same canonical designator is allowed; repeating it with a
   different canonical designator is an error.

``[[profiles::suppress(profile-name [, justification: "..."] [, rule: "..."])]]``
   Allowed on declarations and statements.  Suppresses violations of the named
   profile (optionally narrowed to a single rule) within the appertaining
   declaration or statement; see :ref:`profiles-token-dominion` below.  The
   ``justification:`` argument, if present, must be a string literal; the
   ``rule:`` argument may be a string literal or a bare token.  Both
   arguments are recorded but otherwise opaque to the framework.

``[[profiles::require(profile-designator)]]``
   Allowed only on a *module-import-declaration*.  Diagnoses if the imported
   module's exported enforced-profile set does not contain a designator
   matching the requested one.  Importing a module does **not** retroactively
   enforce its profiles in the importer.

See the auto-generated :doc:`AttributeReference` for the AttrDocs entries
linked from these attributes.


Implementing a New Profile
==========================

Adding a new profile requires no changes to the framework itself.  A profile is
defined entirely by:

1. A **profile name** (a ``::``-separated identifier sequence such as
   ``vendor::safety`` or ``std::type``).
2. Zero or more **rule names** (string identifiers such as
   ``"reinterpret_cast"``).
3. **Diagnostics** emitted when a rule is violated.
4. **Check sites** in the compiler where the framework is consulted.

There is no central registry of profiles.  The framework treats profile names
as opaque strings; a profile is considered "enforced" simply because the user
wrote ``[[profiles::enforce(name)]]`` in their source.  Each rule within a
profile is likewise just a string identifier; users can suppress individual
rules with ``[[profiles::suppress(profile_name, rule: "rule_name")]]``.

There are two implementation patterns, depending on when the rule is checked.


Define Diagnostics
------------------

Add diagnostics to ``clang/include/clang/Basic/DiagnosticSemaKinds.td`` in the
``// C++ Profiles framework (P3589R2)`` group.  Each diagnostic should accept
``%0`` for the profile name, since the framework passes the profile name as
the first diagnostic argument.  The group declares the helper class

.. code-block:: text

   class ProfileRuleError<string str> : Error<str> {
     let SFINAE = SFINAE_Suppress;
   }

so that profile-rule diagnostics participate in the SFINAE machinery as
suppressed errors -- they do not count as substitution failures and cannot
change overload resolution, but selected specializations replay them when
they are actually used.  Define new rules using ``ProfileRuleError`` rather
than ``Error`` directly:

.. code-block:: text

   def err_profile_type_cast_reinterpret : ProfileRuleError<
     "'reinterpret_cast' is unsafe under profile '%0'">;


Pattern 1: Sema Check-Site Profile
----------------------------------

Used when the rule can be checked at a single, well-defined parse-time site
in Sema (typically inside a ``Sema::Build*`` or ``Sema::Act*`` routine).
``test::type_cast`` is the in-tree example.

At each such site, call ``SemaProfiles::checkProfileViolation``:

.. code-block:: c++

   checkProfileViolation("my::profile", "my_rule", Loc,
                         diag::err_my_profile_rule);

This function checks whether the profile is enforced and not suppressed.
During template argument deduction, profile rule diagnostics are suppressed
by Clang's normal SFINAE machinery so they cannot affect overload resolution
or template instantiation; selected specializations replay suppressed
diagnostics when used.  Unevaluated and discarded-statement contexts are
skipped.  The profile name is passed as ``%0``.

Suppression for parse-time check sites is consulted via the
``ProfileSuppressStack`` maintained by the parser-side ``ProfileSuppressScope``
RAII guards (see :ref:`profiles-internals` below).  Profile implementers do
not need to interact with the stack directly.


Pattern 2: Post-Parse / CFG-Based Profile
-----------------------------------------

Used when the rule cannot be checked at a single Sema entry point because it
depends on whole-function analysis -- typically a CFG-based analysis run
after a function body is complete.  ``test::uninit_read`` is the in-tree
example: it diagnoses reads of uninitialized variables on top of Clang's
existing CFG-based uninitialized-variables analysis.

This pattern needs three pieces, all colocated with the analysis pass (the
framework intentionally does not learn the profile name).

1. **Add the profile to the analysis pass's per-pass opt-in table.**
   Each post-parse analysis owns a small table of the profiles that ride it,
   one row per profile (profile name, rule name, diagnostic id).  The
   in-tree example is ``CFGUninitProfiles`` in
   ``clang/lib/Sema/AnalysisBasedWarnings.cpp``:

   .. code-block:: c++

      struct CFGUninitProfileEntry {
        StringRef Name;
        StringRef Rule;
        unsigned DiagID;
      };
      constexpr CFGUninitProfileEntry CFGUninitProfiles[] = {
          {"my::profile", /*Rule=*/"", diag::err_my_profile_rule},
      };

2. **Gate the analysis pass on the table.**
   Analysis-based passes in ``AnalysisBasedWarnings.cpp::IssueWarnings`` are
   normally run only when their corresponding warning flag is enabled.  To
   run the pass for an enforced profile even when the underlying warning is
   silenced, OR an ``S.anyProfileEnforced(Table)`` check (the shared
   ``SemaProfiles::anyProfileEnforced`` gate, also used by the finalization dispatch)
   into the existing pass guard.  The in-tree example's pass guard becomes
   ``hasEnforcedCFGUninitProfile() || !Diags.isIgnored(...)`` (a small
   accessor over ``S.anyProfileEnforced(CFGUninitProfiles)``).

3. **Walk the table in the analysis's diagnostic reporter.**
   For each use site the analysis would have warned about, iterate the
   table and call
   ``SemaProfiles::shouldEmitProfileViolation(name, rule, Stmt*, AnalysisDeclContext&)``,
   which walks parent statements and lexical declaration contexts to honor
   ``[[profiles::suppress]]`` on enclosing AST nodes (the post-parse
   counterpart to ``ProfileSuppressStack``).  Emit the entry's diagnostic
   when it returns true, and skip the default warning path.  In the
   in-tree example (``UninitValsDiagReporter`` in
   ``AnalysisBasedWarnings.cpp``):

   .. code-block:: c++

      for (const auto &U : *vec) {
        for (const CFGUninitProfileEntry &E : CFGUninitProfiles) {
          if (!S.shouldEmitProfileViolation(E.Name, E.Rule, U.getUser(), AC))
            continue;
          S.Diag(U.getUser()->getBeginLoc(), E.DiagID)
              << E.Name << vd->getDeclName();
          S.Diag(vd->getLocation(), diag::note_var_declared_here)
              << vd->getDeclName();
          return;
        }
      }

The diagnostic itself is defined with ``ProfileRuleError`` as in pattern 1.

The post-parse Stmt-walking suppression check is intentionally separate from
the parse-time stack check: by the time CFG analysis runs, the parse stack
has been unwound, so the framework instead walks the AST.  The Stmt-walking
overload of ``isProfileSuppressed`` examines:

- ``AttributedStmt`` ancestors of the use site.
- ``DeclStmt`` ancestors (whose declared ``VarDecl``\ s carry the attribute,
  not the enclosing statement).
- The enclosing ``Decl`` chain via ``getLexicalDeclContext()``.


Pattern 3: Class-Finalization Profile
-------------------------------------

Used when the rule applies to a class as a whole and needs to run once after
the class definition is complete -- for example, "every non-private field of
this class must satisfy property X" or "this class's field set must look
like Y."  ``test::class_final`` is the in-tree example.

The dispatch point is ``SemaProfiles::checkProfileViolationsAtClassFinalization``,
called from the end of ``Sema::CheckCompletedCXXClass`` in
``clang/lib/Sema/SemaDeclCXX.cpp``.  ``CheckCompletedCXXClass`` is the
single function reached from every class-completion path -- the parser
(``ActOnFinishCXXMemberSpecification``), template instantiation
(``InstantiateClass``), and lambda completion -- so wiring the hook there
covers all of them with no extra plumbing.

The class-finalization entry point
``checkProfileViolationsAtClassFinalization`` filters out classes the rules
are not meant to see:

- Dependent classes (``isDependentType()``).  The hook will re-fire on each
  instantiation via the template-instantiation completion path.
- Invalid classes (``isInvalidDecl()``).
- Lambdas (``isLambda()``).  Closure types have no user-controlled field
  shape, so class-finalization rules do not apply.

This pattern needs two pieces, both colocated with the dispatcher.

1. **Add the profile to the class-finalization opt-in table.**
   ``ClassFinalizationProfiles`` in ``clang/lib/Sema/SemaProfiles.cpp`` is a
   small per-pass table of profile name plus callback.  One row per profile:

   .. code-block:: c++

      // FinalizationProfile<Node> is shared with pattern 4.
      template <class Node> struct FinalizationProfile {
        StringRef Name;
        void (*Callback)(Sema &, Node *);
      };
      constexpr FinalizationProfile<CXXRecordDecl> ClassFinalizationProfiles[] = {
          {"my::profile", &runMyProfileCallback},
      };

   The shared ``dispatchFinalizationProfiles`` dispatcher (used by both
   patterns 3 and 4) checks ``anyProfileEnforced(Table)``, sets up a
   ``ProfileSuppressScope(S, RD, /*WalkLexicalParents=*/true)``, iterates the
   table, skips entries whose profile is not enforced, and invokes the
   callback.  Each callback passes the finalized ``Decl`` (here the
   ``CXXRecordDecl``) to the decl-aware ``shouldEmitProfileViolation``
   overload, which walks the declaration and its lexical parents for a
   matching ``[[profiles::suppress]]``, so suppression on the class or any
   enclosing lexical ``Decl`` works without the dispatcher establishing a
   suppress scope.  Finalization can run as a side effect of an *unrelated*
   template instantiation whose ``[[profiles::suppress]]`` scope is still on
   the transient parse-time ``ProfileSuppressStack``; because stack entries
   are matched against the violation's location (see
   :ref:`profiles-token-dominion`), such a scope -- whose construct's tokens
   do not cover the finalized class -- does not suppress the callback's
   diagnostics.

2. **Emit diagnostics from the callback via**
   ``SemaProfiles::shouldEmitProfileViolation``.  Each callback decides where on
   the class to point and which diagnostic to use, possibly with notes:

   .. code-block:: c++

      void runMyProfileCallback(Sema &S, CXXRecordDecl *RD) {
        if (!S.shouldEmitProfileViolation("my::profile", /*Rule=*/"",
                                          RD->getLocation()))
          return;
        S.Diag(RD->getLocation(), diag::err_my_profile_rule)
            << "my::profile" << RD;
      }

The diagnostic itself is defined with ``ProfileRuleError`` as in patterns 1
and 2.

Class-finalization is for **structural** rules -- those answerable from the
class's declared members, their types, and their attributes.  The callbacks
run while the ``CXXRecordDecl`` is being finalized (immediately before
``CheckCompletedCXXClass`` returns), which is *before any constructor body or
member-initializer list has been parsed* -- inline member bodies are
late-parsed afterward, and out-of-line and template member constructors later
still.  A class-finalization callback therefore must not inspect a
constructor's ``inits()`` (they are empty here).  Rules that depend on what a
constructor initializes belong on the constructor-finalization dispatch
(pattern 4); rules that need whole-function flow analysis belong on a
post-parse CFG pass (pattern 2).


Pattern 4: Constructor-Finalization Profile
-------------------------------------------

Used when the rule applies to a single constructor and needs that
constructor's complete member-initializer list -- for example, "every member
must be initialized by this constructor."  ``test::ctor_final`` is the in-tree
example.

The dispatch point is ``SemaProfiles::checkProfileViolationsAtConstructorFinalization``,
called right after ``DiagnoseUninitializedFields`` in
``Sema::ActOnMemInitializers`` and ``Sema::ActOnDefaultCtorInitializers`` in
``clang/lib/Sema/SemaDeclCXX.cpp``.  Those two functions are the funnel for
every user-defined constructor -- written or implicit member-initializer
list, inline or out-of-line -- and template instantiation reaches the first
of them through ``Sema::InstantiateMemInitializers``, so the hook sees every
constructor at the point its ``inits()`` (including synthesized entries) is
complete.

The constructor-finalization entry point
``checkProfileViolationsAtConstructorFinalization`` filters out constructors
the rules are not meant to see:

- Dependent constructors (``isDependentContext()``).  The hook re-fires on
  each instantiation.
- Invalid constructors (``isInvalidDecl()``).
- Delegating constructors (``isDelegatingConstructor()``), which leave member
  initialization to their target.

The two pieces mirror pattern 3 and share its machinery: a per-pass opt-in
table ``ConstructorFinalizationProfiles`` of the same
``FinalizationProfile<Node>`` row (here
``FinalizationProfile<CXXConstructorDecl>``), and a callback that emits via
``SemaProfiles::shouldEmitProfileViolation``.  The same shared
``dispatchFinalizationProfiles`` dispatcher invokes each callback, which
passes the ``CXXConstructorDecl`` to the decl-aware
``shouldEmitProfileViolation`` overload; that overload walks the declaration
and its lexical parents, so ``[[profiles::suppress]]`` on the constructor,
the class, or an enclosing lexical ``Decl`` works.  As for pattern 3, a
transient parse-time suppress scope belonging to an unrelated construct does
not reach these callbacks, because stack entries only match violations whose
tokens their construct covers (see :ref:`profiles-token-dominion`).  A
constructor body is normally instantiated lazily -- outside any unrelated
suppress scope -- so for pattern 4 this matters rarely; the scenario it
actually handles arises in pattern 3, where a class completes synchronously
inside an enclosing instantiation.  A callback that should only apply to
user-written constructors checks ``Ctor->isUserProvided()``.


.. _profiles-token-dominion:

Suppression Dominion is Token-Based
===================================

A ``[[profiles::suppress(P)]]`` attribute suppresses profile ``P`` in the
token range of the declaration or statement it appertains to -- nothing more.
For a variable declaration that range covers the initializer expression (so
``[[profiles::suppress(P)]] T x = init();`` silences violations inside
``init()``), but it does *not* tag the variable as permitted-uninitialized for
subsequent uses; those uses appear in different declarations or statements
and are checked normally at their own source location. Profiles that need
per-object "opt-out of this check everywhere this value is used" semantics
(for example, the proposed ``[[uninitialized]]`` attribute of the
initialization profile) must introduce their own, separate, decl-scoped
marker.

This applies identically to the parse-time suppression stack and the
post-parse Stmt-tree walker described in pattern 2.

The parse-time stack enforces the dominion positionally: each entry records
the begin location of the construct its attribute appertains to, a violation
matches an entry only if its location is at or after that begin (in
translation-unit token order), and the entry's ``ProfileSuppressScope``
lifetime bounds the dominion's end.  This is what keeps a live suppress scope
from leaking into code whose tokens it does not cover.  A check can fire
under an *unrelated* construct's scope in two ways: a template pattern
instantiated synchronously while the scope is live (the pattern's tokens --
which instantiated code retains as its source locations -- precede the
suppressed construct), and a class or constructor finalized as a side effect
of such an instantiation (patterns 3 and 4).  In both cases the entry's
begin location is after the violation's, so the suppression correctly does
not apply; conversely, a local class or lambda *defined inside* the
suppressed construct is covered, whichever path re-enters it.  One known
over-approximation: scope liveness bounds the dominion's end, so a pattern
forward-declared before but *defined after* the suppressed construct is
wrongly treated as covered while the scope is live.


.. _profiles-internals:

Framework Internals Reference
=============================

This section describes the framework mechanisms that profile implementers
benefit from understanding, even though they do not interact with them directly.

``ProfileSuppressScope``
------------------------

An RAII guard that pushes suppression entries onto ``Sema::ProfileSuppressStack``
and pops them on destruction.  It is used by the parser and template
instantiation machinery to make ``[[profiles::suppress]]`` attributes active
during the appropriate region.  Each entry records the begin location of the
construct its attribute appertains to, and matches only violations located at
or after it (see :ref:`profiles-token-dominion`).  ``checkProfileViolation``
consults ``ProfileSuppressStack`` directly, so profile implementers never need
to create ``ProfileSuppressScope`` objects.

Stmt-Tree Suppression Walker
----------------------------

The ``isProfileSuppressed(name, rule, Stmt*, AnalysisDeclContext&)`` overload
is the post-parse counterpart to ``ProfileSuppressStack``.  It is used by
analyses that run after parsing (when the parse-time stack no longer
reflects the enclosing region) and walks the AST upward from a use site to
find any matching ``[[profiles::suppress]]`` attribute on an enclosing
``AttributedStmt``, ``DeclStmt``-declared ``VarDecl``, or lexical
``Decl`` parent.

Template Instantiation
----------------------

During template instantiation, the framework ensures that
``[[profiles::suppress]]`` on the template pattern and its lexical parents
applies to instantiated code.  This is done via ``ProfileSuppressScope`` with
``WalkLexicalParents=true`` at several sites:

- ``SemaTemplateInstantiateDecl.cpp`` -- function and variable template
  instantiation.
- ``SemaTemplateInstantiate.cpp`` -- default member initializer instantiation.
- ``TreeTransform.h`` -- ``TransformAttributedStmt`` (suppress on statements)
  and ``TransformDeclStmt`` (suppress on declarations within a ``DeclStmt``).

The reverse direction is *not* propagated: a suppress scope live at the
*point of instantiation* (for example on the declaration whose initializer
triggers it) covers the trigger's tokens, not the pattern's.  Instantiated
code retains the pattern's source locations, so the dominion check on stack
entries (see :ref:`profiles-token-dominion`) keeps such a scope from
suppressing checks that fire inside a synchronously instantiated body, NSDMI,
default argument, or instantiated marker re-check.

Module Enforcement
------------------

``[[profiles::enforce(...)]]`` on a module interface declaration records the
enforced profile designators on ``Module::EnforcedProfileDesignators``.  A
(non-partition) module implementation unit ``module M;`` automatically inherits
the interface's enforcements, because it implicitly imports the primary
interface unit of ``M``.
``[[profiles::require(...)]]`` on an import-declaration validates that the
imported module's ``EnforcedProfileDesignators`` contains a matching designator.

A *header unit* participates the same way: ``[[profiles::enforce(...)]]`` on
an empty-declaration in the header (the form P3589R2 §2.3 prescribes for
header units) is recorded on the header-unit module and serialized into its
BMI, so ``[[profiles::require]]`` on an ``import "header.h";`` validates
against it.  As with named modules, importing an enforced header unit does not
enforce the profile in the importer.

``[[profiles::enforce(...)]]`` on a *non-interface* module-declaration (a
``module M;`` implementation unit, or a ``module M:P;`` partition
implementation unit) is accepted but recorded only translation-unit-locally;
it is **not** added to ``Module::EnforcedProfileDesignators`` and so is not
visible to an importer's ``[[profiles::require]]``.

A module partition implementation unit ``module M:P;`` is also a module
implementation unit of ``M``, so the primary interface's enforcements apply to
it as well.  However, it does **not** implicitly import the primary interface,
and the primary interface is normally compiled *after* its partitions, so its
BMI is usually not available when the partition implementation unit is compiled.
Inheritance here is therefore **best-effort**: the enforcements are inherited
only when the primary interface's BMI is already resident in the compilation
(for example, supplied via an eager ``-fmodule-file=<path>``); the BMI is never
force-loaded and its absence is never diagnosed.  When it is not available the
partition implementation unit is simply not subject to the inherited profile --
a missed diagnostic, never a change to the meaning of a well-formed program.
For guaranteed enforcement, **repeat** ``[[profiles::enforce(...)]]`` in the
partition implementation unit rather than relying on inheritance.  (Best-effort
inheritance is silent when the interface BMI is absent; if the BMI *is* resident
and enforces a profile whose designator conflicts with a locally repeated
``enforce`` of the same name, that mismatch is still diagnosed.)

Importing a module that enforces a profile does **not** enforce that profile in
the importing translation unit.  Enforcement is always explicit and local.

Redeclaration Compatibility
---------------------------

P3589R2 [decl.attr.enforce]p5: a declaration and its redeclarations must
appear in the dominions of mutually compatible profiles.  The rule is
**symmetric** -- when a redeclaration is merged with a previous declaration
from another module unit, every profile whose dominion covered the previous
declaration must have a compatible counterpart covering the redeclaration,
and vice versa.  In particular, a profile-enforcing translation unit that
redeclares an entity from a module (or header unit) compiled *without* a
compatible profile is ill-formed; the paper's escape hatch for such headers
is ``[[profiles::exempt]]`` (not yet implemented, see `Intentional
Omissions`_).  The check
(``SemaProfiles::checkRedeclarationProfileCompatibility``) runs from
``Sema::CheckRedeclarationInModule``, the funnel for function, variable, tag,
alias, and class-template redeclarations; it is a framework rule -- a plain
error, not suppressible with ``[[profiles::suppress]]``, and diagnose-only
(the redeclaration still merges).

Two profiles are *compatible* if they have the same name -- designator
arguments configure a profile without changing its identity -- or if both are
standard (``std::``-prefixed) profiles, which P3589R2 proclaims mutually
compatible.  No further implementation-proclaimed compatibility is modeled.

The previous declaration's dominion is approximated by its top-level module's
exported ``EnforcedProfileDesignators``, which is exact for declarations in
the module purview -- including purview ``extern "C"``/``extern "C++"``
declarations (implicit global module), the common redeclarable case, since
module-attached entities cannot be redeclared in other translation units at
all.  Two cases have an *unknown* dominion and are skipped rather than
guessed at (a missed diagnostic, never a wrong one):

- A declaration in an **explicit global module fragment**: it precedes the
  module-declaration, so the exported enforcements do not cover it, and its
  TU's empty-declaration enforces are not serialized into the BMI.
- A previous declaration from the **same module family** (an implementation
  or partition unit merging with its own interface): the exported set
  under-approximates the interface TU's full dominion, and the interface's
  enforcements are inherited into the current unit anyway, so checking would
  false-positive on locally added profiles.

A textual or PCH previous declaration is not checked at all: it shares the
current TU's dominion (the placement rule makes a TU's dominion uniform, and
a PCH's enforcements are restored into the including TU).  Implicit template
instantiations are exempt, matching the module-ownership check.

Serialization
-------------

The framework serializes enforcement state automatically.  Profile implementers
do not need to add any serialization code.

- **PCH**: ``SemaProfiles::EnforcedProfiles`` is written as ``ENFORCED_PROFILES``
  records in the AST bitstream and restored when the PCH is loaded.
- **Module BMI**: ``Module::EnforcedProfileDesignators`` is written as
  ``SUBMODULE_ENFORCED_PROFILES`` records within each submodule block.

Intentional Omissions
=====================

The following parts of P3589R2 are deliberately not implemented:

- ``[[profiles::exempt(...)]]`` (P3589R2 section 1.1.6), which would exempt
  named included source files from profile enforcement. Implementing it
  requires bookkeeping that connects the original spelling of an ``#include``
  to the source locations of constructs in the included file, and the feature
  is not needed to exercise or validate the rest of the framework.


Built-in Test Profiles
======================

The tree ships four minimal, test-only profiles -- one per implementation
pattern -- so the framework's behavior can be exercised without depending on
any user-facing profile.  All are gated on ``-fprofiles`` and additionally on
the ``-cc1``-only ``-fprofiles-test-profiles`` flag (see `Driver Flag`_), and
are inert under ``-fprofiles`` alone:

- ``test::type_cast`` -- pattern-1 example.
- ``test::uninit_read`` -- pattern-2 example riding the existing
  CFG uninitialized-variables analysis.
- ``test::class_final`` -- pattern-3 example riding the
  class-finalization dispatch.
- ``test::ctor_final`` -- pattern-4 example riding the
  constructor-finalization dispatch.

By convention:

- Real test profiles live under the ``test::`` namespace.  Today there are
  four: ``test::type_cast``, ``test::uninit_read``, ``test::class_final``,
  and ``test::ctor_final``.  Because the ``test::`` prefix is what
  ``SemaProfiles::isProfileEnforced`` keys on to gate them behind
  ``-fprofiles-test-profiles``, any new test-only profile must also live
  under ``test::``.
- The names ``test::other``, ``test::bounds``, ``test::new_profile``, and
  ``test::not_enforced`` are deliberately *not* implemented and appear only
  in negative tests as stand-in "some other profile" names.  Adding a real
  profile under any of these names would invalidate those tests.


The ``test::type_cast`` Profile
-------------------------------

A pattern-1 (Sema check-site) profile.  Demonstrates the simple case where
a rule can be checked from a single Sema entry point.

- **Rules**: ``reinterpret_cast``.
- **Diagnostic**: ``err_profile_type_cast_reinterpret``
  ("'reinterpret_cast' is unsafe under profile '%0'").
- **Check site**: ``Sema::BuildCXXNamedCast`` in ``clang/lib/Sema/SemaCast.cpp``,
  inside the ``reinterpret_cast`` arm.  Only the ``reinterpret_cast<>`` keyword
  form is checked; a C-style or functional cast with reinterpret semantics goes
  through a different path and is not diagnosed.

The entire profile implementation is the single call:

.. code-block:: c++

   checkProfileViolation("test::type_cast", "reinterpret_cast", OpLoc,
                         diag::err_profile_type_cast_reinterpret);


The ``test::uninit_read`` Profile
---------------------------------

A pattern-2 (post-parse / CFG-based) profile.  Demonstrates the case where
the rule depends on whole-function analysis and must run after parsing.
It diagnoses reads of uninitialized variables by reusing Clang's existing
CFG-based uninitialized-variables analysis.

- **Rules**: none (the profile has a single implicit rule, so the rule
  string is empty).
- **Diagnostic**: ``err_profile_uninit_read``
  ("variable %1 is read before initialization under profile '%0'").
  A companion ``note_var_declared_here`` is emitted at the variable's
  declaration.
- **Opt-in table**: ``CFGUninitProfiles`` in
  ``clang/lib/Sema/AnalysisBasedWarnings.cpp``.  The ``IssueWarnings`` pass
  guard consults it via ``hasEnforcedCFGUninitProfile()`` so the analysis
  runs even when ``-Wuninitialized`` is silenced, and
  ``UninitValsDiagReporter::diagnoseUnitializedVar`` walks it *before* the
  default warning path -- when an entry's
  ``SemaProfiles::shouldEmitProfileViolation`` returns true the entry's diagnostic
  fires and the default warning is skipped entirely.

The Stmt-tree suppression walker is what makes ``[[profiles::suppress]]``
work for this profile: by the time the CFG analysis runs, the parse-time
``ProfileSuppressStack`` has been unwound, so the helper consults the AST
directly via ``shouldEmitProfileViolation(name, rule, Stmt*, AnalysisDeclContext&)``.


The ``test::class_final`` Profile
---------------------------------

A pattern-3 (class-finalization) profile.  Demonstrates the case where the
rule applies once per completed class definition and runs from the
class-finalization dispatch in ``Sema::CheckCompletedCXXClass``.

- **Rules**: none (the profile has a single implicit rule, so the rule
  string is empty).
- **Diagnostic**: ``err_profile_class_final_test`` ("test profile fired on
  completion of class %1 under profile '%0'").
- **Opt-in table**: ``ClassFinalizationProfiles`` in
  ``clang/lib/Sema/SemaProfiles.cpp``.

Because dependent classes are filtered out by the dispatcher, the
diagnostic fires on class template *instantiations* rather than on the
primary template.  Lambda closures are also skipped.
``[[profiles::suppress(test::class_final)]]`` on the class or any
enclosing lexical ``Decl`` silences the diagnostic via the
``ProfileSuppressScope(*this, RD, /*WalkLexicalParents=*/true)`` the
dispatcher establishes around each callback.


The ``test::ctor_final`` Profile
--------------------------------

A pattern-4 (constructor-finalization) profile.  Demonstrates the case where
the rule applies once per user-defined constructor, after its
member-initializer list is complete.

- **Rules**: none (single implicit rule, empty rule string).
- **Diagnostic**: ``err_profile_ctor_final_test`` ("test profile fired on
  finalization of a constructor for class %1 under profile '%0'").
- **Opt-in table**: ``ConstructorFinalizationProfiles`` in
  ``clang/lib/Sema/SemaProfiles.cpp``.

The diagnostic fires once per user-defined constructor -- written or implicit
member-initializer list, inline or out-of-line -- and on constructor template
*instantiations* rather than the dependent pattern.  Defaulted and implicit
constructors (no body) and delegating constructors are skipped.
