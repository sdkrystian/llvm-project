======================
C++ Profiles Framework
======================

.. contents::
   :depth: 2
   :local:


Introduction
============

The C++ Profiles framework (`P3589R2
<https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3589r2.pdf>`_) lets a
translation unit opt into additional language restrictions called *profiles*.
A profile is a named set of rules enforced by the compiler, each formulated
to keep the program free of a certain class of problems -- for example, use
of uninitialized memory.  Three attributes control it:

- ``[[profiles::enforce(...)]]`` requests enforcement of one or more profiles
  for the translation unit.
- ``[[profiles::suppress(...)]]`` locally exempts a declaration or statement
  from an enforced profile, or from a single rule of it.
- ``[[profiles::require(...)]]`` on a module import verifies that the
  imported module advertises a profile.

Profiles do not change the meaning of well-formed programs with no undefined
behavior.  Their effects are conceptually applied only after translation
phase 7: a profile cannot change the outcome of overload resolution or
template instantiation, and it is not possible to SFINAE on a profile
violation.

Profile names are open-ended: standard (``std::``-prefixed),
implementation-defined, and third-party profiles are all requested with the
same syntax, and enforcing a profile the implementation does not know is not
an error -- it simply has no rules to enforce.  The feature is experimental:
attribute spellings, rule names, and diagnostics may change.


Usage
=====

The framework is gated on the C++-only ``-fprofiles`` flag, which defaults to
off:

.. code-block:: console

   clang++ -std=c++23 -fprofiles example.cpp

The attributes accept both the ``[[profiles::name(...)]]`` and the
``[[using profiles: name(...)]]`` spelling and always require an argument
clause; see the :doc:`AttributeReference` for the per-attribute reference.

Without ``-fprofiles`` the attributes are ignored with a warning, and their
argument clauses are not checked against the P3589R2 grammar -- like any
standard attribute the implementation does not act on, an arbitrary
balanced-token argument clause is accepted.  Code annotated for a
profiles-enabled build therefore still compiles (modulo the warning) with the
feature off, and no profile rule ever fires.


Enforcing Profiles
==================

``[[profiles::enforce(profile-designator-list)]]`` requests enforcement of
the named profiles for the whole translation unit.  It may appear only on an
*empty-declaration* that precedes every other declaration at translation-unit
scope, or on a *module-declaration* (see `Profiles and Modules`_):

.. code-block:: c++

   [[profiles::enforce(std::safety)]];
   [[profiles::enforce(vendor::hardened(fortify: 3))]];  // designator arguments

   #include <my/lib.h>

   int main() { /* ... */ }

A *profile-designator* is a ``::``-qualified profile name, optionally
followed by a parenthesized argument list.  The arguments are not subject to
name lookup; their interpretation is up to the profile.  Repeating an
enforcement with the same designator is allowed and has no effect, but
requesting the same profile with a different designator is an error, as is an
enforcement placed after another declaration:

.. code-block:: c++

   [[profiles::enforce(vendor::hardened(fortify: 3))]];
   [[profiles::enforce(vendor::hardened(fortify: 3))]];  // OK: no effect
   [[profiles::enforce(vendor::hardened(fortify: 2))]];  // error: same profile,
                                                         // different designator
   int x;
   [[profiles::enforce(std::safety)]];  // error: does not precede 'x'


Suppressing Enforcement
=======================

``[[profiles::suppress(profile-name)]]`` on a declaration or statement
exempts it from the named profile's rules.  An optional ``rule:`` argument
narrows the suppression to a single named rule, and an optional
``justification:`` argument (a string literal) records why the suppression is
there:

.. code-block:: c++

   [[profiles::enforce(std::safety)]];

   void fill(char *buf, int n);

   int main() {
     [[profiles::suppress(std::safety,
                          rule: "some_rule",
                          justification: "buffer is filled in by fill()")]]
     char buffer[1024];
     fill(buffer, 1024);
   }

A suppression covers exactly the tokens of the declaration or statement it
appertains to -- nothing more.  For a variable declaration that includes the
initializer, so violations inside the initializer are silenced; but the
variable is *not* marked as exempt at later uses, which appear in other
declarations or statements and are checked normally:

.. code-block:: c++

   [[profiles::suppress(std::safety)]] int x;  // declaration-site rules
                                               // suppressed for this decl
   int y = x;  // uses of 'x' elsewhere are checked normally

To exempt an object from a profile's checks everywhere it is used, a profile
must provide its own per-object, decl-scoped marker attribute.


Profiles and Modules
====================

A module interface advertises the profiles it enforces through
``[[profiles::enforce]]`` on its module-declaration, and importers can insist
on that advertisement with ``[[profiles::require]]``, which may appear only
on a module-import-declaration:

.. code-block:: c++

   // M.cppm
   export module M [[profiles::enforce(std::safety)]];

   // user.cpp
   import M [[profiles::require(std::safety)]];  // OK: M enforces it
   import N [[profiles::require(std::safety)]];  // error unless N does too

``[[profiles::require]]`` only verifies the advertisement; importing an
enforcing module does **not** enforce its profiles in the importer.
Enforcement is always explicit and local.  A header unit participates the
same way: an ``[[profiles::enforce(...)]];`` empty-declaration in the header
is exported by the corresponding header unit and validated by
``[[profiles::require]]`` on its import.

``-fprofiles`` does not have to be uniform across a build: a module built
without the flag imports fine into a profiles-enabled compile -- it simply
advertises no profiles, so a ``[[profiles::require]]`` on the import reports
the profile as not enforced -- and an enforcing module loads fine into a
compile with the feature off.  A PCH is stricter (like other compatible
language options, it must be built with the same ``-fprofiles`` setting as
its consumer).

Enforcement on a module interface extends to the module's implementation
units:

- A non-partition implementation unit (``module M;``) inherits the
  interface's enforcements automatically.
- A partition implementation unit (``module M:P;``) inherits them only on a
  best-effort basis, because the interface's BMI is usually not built yet
  when the partition is compiled.  Repeat the ``[[profiles::enforce]]`` there
  for guaranteed enforcement.

A declaration and its redeclarations must appear under mutually *compatible*
profiles (P3589R2 [decl.attr.enforce]p5): redeclaring an entity from a module
or header unit that was compiled without a compatible profile is diagnosed.
Two profiles are compatible when they have the same name (designator
arguments configure a profile without changing its identity), and all
standard ``std::`` profiles are mutually compatible.


Test Profiles
=============

Clang also ships four ``test::`` profiles (``test::type_cast``,
``test::uninit_read``, ``test::class_final``, and ``test::ctor_final``) that
exist only to exercise the framework in the test suite.  They are inert
without an additional ``-cc1``-only flag; see
:doc:`ProfilesFrameworkInternals`.


Not Yet Implemented
===================

``[[profiles::exempt(...)]]`` (P3589R2 §1.1.6), which would exempt named
included source files from the enforcement of a profile, is not implemented.

As a **temporary stopgap** until ``[[profiles::exempt]]`` has wording and an
implementation, Clang exempts code that originates in a *system header* from
profile enforcement.  This is on by default whenever a profile is enforced, so
enforcing a profile on a translation unit does not report violations inside the
standard library and the other system headers it transitively includes.  Pass
``-fno-profiles-exempt-system-headers`` to disable the exemption and enforce
profiles in system-header code as well (spec-exact behavior).


Extending the Framework
=======================

The framework is profile-agnostic: profile names are opaque strings, there is
no central registry, and adding a new profile requires no changes to the
framework itself.  See :doc:`ProfilesFrameworkInternals` for the
implementation patterns and the API for adding a new profile.
