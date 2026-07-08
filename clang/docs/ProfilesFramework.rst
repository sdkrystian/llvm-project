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


Extending the Framework
=======================

The framework is profile-agnostic: profile names are opaque strings, there is
no central registry, and adding a new profile requires no changes to the
framework itself.  See :doc:`ProfilesFrameworkInternals` for the
implementation patterns and the API for adding a new profile.


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
the token range of the construct its attribute appertains to, and a
violation matches an entry only if its location falls within that range (in
translation-unit token order).  This is what keeps a live suppress scope
from leaking into code whose tokens it does not cover.  A check can fire
under an *unrelated* construct's scope in two ways: a template pattern
instantiated synchronously while the scope is live (instantiated code
retains the pattern's source locations, which lie outside the suppressed
construct wherever the pattern is declared -- before it, or first declared
after it), and a class or constructor finalized as a side effect of such an
instantiation (patterns 3 and 4).  In both cases the violation's location is
outside the entry's range, so the suppression correctly does not apply;
conversely, a local class or lambda *defined inside* the suppressed
construct is covered, whichever path re-enters it.

The range's end is recorded only when the construct was already fully
parsed when the entry was pushed -- a completed pattern or lexical parent at
an instantiation site, or a transformed ``AttributedStmt``.  For a construct
still being parsed no end is recorded (its end location would be
misleadingly early: a mid-parse class collapses to its name token, a
body-pending function to its declarator) and the entry's
``ProfileSuppressScope`` lifetime bounds the dominion instead.  That
fallback is exact mid-parse: the construct's later tokens do not exist yet,
and instantiation of a template that has no definition yet is deferred past
the scope's death.


Intentional Omissions
=====================

The following parts of P3589R2 are deliberately not implemented:

- ``[[profiles::exempt(...)]]`` (P3589R2 section 1.1.6), which would exempt
  named included source files from profile enforcement. Implementing it
  requires bookkeeping that connects the original spelling of an ``#include``
  to the source locations of constructs in the included file, and the feature
  is not needed to exercise or validate the rest of the framework.


Built-in Profiles
=================

The tree ships four built-in ``test::`` profiles, all gated on
``-fprofiles``.  They exist only to exercise the framework: they are
additionally gated on the ``-cc1``-only ``-fprofiles-test-profiles`` flag,
are inert under ``-fprofiles`` alone, and are described in
:doc:`ProfilesFrameworkInternals`.
