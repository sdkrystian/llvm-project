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
- ``[[profiles::require(profile-designator-list)]]`` on a module import
  verifies that the imported module advertises the named profiles.

Profiles do not change the meaning of well-formed programs with no undefined
behavior.  Their effects are conceptually applied only after translation
phase 7: a profile cannot change the outcome of overload resolution or
template instantiation, and it is not possible to SFINAE on a profile
violation.  A rule that needs a declaration, a completed class or
constructor, or whole-function analysis is applied once per template
instantiation and never to an uninstantiated template.  A rule checked at a
single expression is applied to a template as written when the expression
does not depend on the template's parameters, and once per instantiation
otherwise (see :doc:`ProfilesFrameworkInternals`).

Profile names are open-ended: standard (``std::``-prefixed),
implementation-defined, and third-party profiles are all requested with the
same syntax, and enforcing a profile the implementation does not know is not
an error -- it simply has no rules to enforce.  Clang currently implements
one real profile on this branch: an initial slice of the proposed
``std::core_ub`` undefined-behavior profile (see `The std::core_ub
Profile`_), whose rules are checked at run time.  The feature is
experimental: attribute spellings, rule names, and diagnostics may
change.


Usage
=====

The framework is gated on the C++-only ``-fprofiles`` flag, which defaults to
off; ``-fprofiles-enforce=`` (see `Enforcing Profiles`_) implies it:

.. code-block:: console

   clang++ -std=c++23 -fprofiles example.cpp
   clang++ -std=c++23 -fprofiles-enforce=std::safety example.cpp

Despite the similar spelling, the ``-fprofiles`` family of flags is
unrelated to the ``-fprofile-*`` profile-guided-optimization and coverage
options.

The attributes accept both the ``[[profiles::name(...)]]`` and the
``[[using profiles: name(...)]]`` spelling and always require an argument
clause; see the :doc:`AttributeReference` for each attribute's reference
entry.

Without ``-fprofiles`` the attributes are ignored with a warning, and their
argument clauses are not checked against the P3589R2 grammar -- like any
standard attribute the implementation does not act on, an arbitrary
balanced-token argument clause is accepted.  Code annotated for a
profiles-enabled build therefore still compiles (modulo the warning) with the
feature off, and no profile rule ever fires.


Enforcing Profiles
==================

``[[profiles::enforce(profile-designator-list)]]`` requests enforcement of
the named profiles for the remainder of the translation unit -- everything
after the attribute; code before it, such as a global module fragment, is
not checked.  It may appear only on an *empty-declaration* that precedes
every other declaration at translation-unit scope, or on a
*module-declaration* (see `Profiles and Modules`_).  A module-declaration
itself does not count as a preceding declaration, nor does the implicit
import of the interface that a ``module M;`` implementation unit performs,
so an implementation unit may open with its own empty-declaration enforce:

.. code-block:: c++

   [[profiles::enforce(std::safety)]];
   [[profiles::enforce(vendor::hardened(fortify: 3))]];  // designator arguments

   #include <my/lib.h>

   int main() { /* ... */ }

A *profile-designator* is a profile name -- one or more identifiers joined
by ``::`` -- optionally followed by a parenthesized argument list.  The
arguments are not subject to name lookup; their interpretation is up to the
profile.  Repeating an
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

The same enforcement can be requested from the command line.
``-fprofiles-enforce=`` takes a comma-separated list of profile *names* (no
designator arguments) and may be repeated; it implies ``-fprofiles``, and
``-fno-profiles`` disables the framework together with any enforcement
requested this way:

.. code-block:: console

   clang++ -std=c++23 -fprofiles-enforce=std::safety,acme::hardened example.cpp

A command-line enforcement covers the entire translation unit -- there is no
attribute for code to precede, so a global module fragment is checked too --
and otherwise behaves like an enforcement written first in the source:
repeating it in source has no effect, a designator that names the same
profile with arguments is the mismatch error above (the note names the
option), and other profiles may be enforced in source alongside it.  As in
source, a name the implementation does not know is accepted and enforces
nothing; only the spelling is checked (identifiers joined by ``::``).  The
enforcement is local to the translation unit it is given to (see `Profiles
and Modules`_).


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
appertains to, nothing more; P3589R2 calls this token range the
suppression's *dominion*.  The declarator-id position works too: on a
function definition it covers the whole definition, mem-initializers and
body included, for free functions, inline members, and out-of-line
definitions alike (the attribute must be on the *definition*; one written
on a previous declaration does not carry over):

.. code-block:: c++

   void legacy_code [[profiles::suppress(std::safety)]] () { /* exempt */ }

For a variable declaration the covered tokens include the initializer, so
violations inside the initializer are silenced; but the variable is *not*
marked as exempt at later uses, which appear in other declarations or
statements and are checked normally:

.. code-block:: c++

   [[profiles::suppress(std::safety)]] int x;  // declaration-site rules
                                               // suppressed for this decl
   int y = x;  // uses of 'x' elsewhere are checked normally

In a declaration declaring multiple declarators, a suppression written on one
declarator covers that declarator only -- its declarator-id through the end
of its initializer -- never its siblings; a suppression written in the
declaration's prefix covers every declarator.

To exempt an object from a profile's checks everywhere it is used, a profile
must provide its own per-object, decl-scoped marker attribute.


Diagnostic Groups
=================

Every profile rule's diagnostic belongs to the diagnostic group
``-Wprofile-<profile>-<rule>``, nested in ``-Wprofile-<profile>`` and, with
every other profile, in ``-Wprofiles``; the group name lowercases the
profile and rule names and spells ``::`` and ``_`` as ``-``
(``std::init`` / ``uninit_read`` is ``-Wprofile-std-init-uninit-read``).
Enforcement and suppression are expressed through these groups, so the
ordinary diagnostic controls apply to profile rules as well:

- A violation message ends in its group name, ``[-Wprofile-...]``, unless
  ``-fno-diagnostics-show-option`` is given.
- ``-Wprofile-<profile>`` or ``-Wprofile-<profile>-<rule>`` reports the
  rules as *warnings* without any enforcement, and ``-Werror=profile-...``
  as errors; ``-Weverything`` does not enable them.
- ``#pragma clang diagnostic ignored|warning|error "-Wprofile-..."``, with
  ``push`` and ``pop``, changes a rule's severity for the rest of the file,
  under an enforcement or without one; ``-Weverything``, as an option or a
  pragma, never touches a rule.
- ``--warning-suppression-mappings`` exempts the listed paths from a
  ``-fprofiles-enforce=`` enforcement, not from one written in source.

``-Wno-profiles`` (or a narrower ``-Wno-profile-...``) does not disable an
enforcement, whose mapping is installed after the command-line options; a
violation stays an error under ``-w`` as well, and ``-Wno-error=profile-...``
has no effect on an enforced rule.  A ``#pragma clang diagnostic push``
before an enforcement and ``pop`` after it leave the enforcement in place.


System Headers
==============

As a temporary stopgap until ``[[profiles::exempt]]`` is implemented (see
`Not Yet Implemented`_), code originating in a *system header* is exempt
from profile enforcement.  The exemption is on by default, so enforcing a
profile does not report violations inside the standard library and the
other system headers a translation unit includes.  Pass
``-fno-profiles-exempt-system-headers`` to disable it and enforce profiles
in system-header code as well (spec-exact behavior).


Profiles and Modules
====================

A module interface advertises the profiles it enforces through
``[[profiles::enforce]]`` on its module-declaration, and importers can insist
on that advertisement with
``[[profiles::require(profile-designator-list)]]``, which may appear only on
a module-import-declaration.  Each listed designator must be advertised with
exactly the same spelling, arguments included; each one that is not is
diagnosed individually:

.. code-block:: c++

   // M.cppm
   export module M
       [[profiles::enforce(std::safety, vendor::hardened(fortify: 3))]];

   // user.cpp
   import M [[profiles::require(std::safety)]];        // OK: M advertises it
   import M [[profiles::require(
       std::safety, vendor::hardened(fortify: 3))]];   // OK: exact spellings
   import M [[profiles::require(vendor::hardened)]];   // error: not the
                                                       // advertised spelling
   import N [[profiles::require(std::safety)]];        // error unless N
                                                       // advertises it too

``[[profiles::require]]`` only verifies the advertisement; importing an
enforcing module does **not** enforce its profiles in the importer.
Enforcement is always explicit and local.  A module's own code keeps the
module's enforcement wherever it is used: a template or inline function
from an enforcing module is checked -- and gets its runtime checks -- when
it is instantiated or emitted in a non-enforcing importer, and code from a
non-enforcing module is not checked in an enforcing importer.  (A
module-map module under ``-fmodules`` has no enforcement of its own and
takes the importer's ``-fprofiles-enforce=``.)  A header unit participates the
same way: an ``[[profiles::enforce(...)]];`` empty-declaration in the header
is exported by the corresponding header unit and validated by
``[[profiles::require]]`` on its import.

``-fprofiles`` does not have to be uniform across a build: a module built
without the flag imports fine into a profiles-enabled compile -- it simply
advertises no profiles, so a ``[[profiles::require]]`` on the import reports
the profile as not enforced -- and an enforcing module loads fine into a
compile with the feature off.  A PCH is stricter (like other compatible
language options, it must be built with the same ``-fprofiles`` and
``-fprofiles-enforce=`` settings as its consumer).

Enforcement on a module interface extends to the module's implementation
units:

- A non-partition implementation unit (``module M;``) inherits the
  interface's enforcements automatically.
- A partition implementation unit (``module M:P;``) inherits them only on a
  best-effort basis, because the interface's BMI is usually not built yet
  when the partition is compiled.  Repeat the ``[[profiles::enforce]]`` there
  for guaranteed enforcement.

``-fprofiles-enforce=`` is local to the translation unit it is given to: a
module interface or header unit built with it advertises nothing for
``[[profiles::require]]``, and an implementation unit does not inherit it --
each unit that is to be enforced is given the option.  It does count as the
unit's enforcement for redeclaration compatibility (below).

A declaration and its redeclarations must appear under mutually *compatible*
profiles (P3589R2 [decl.attr.enforce]p5): redeclaring an entity from a module
or header unit that was compiled without a compatible profile is diagnosed.
Two profiles are compatible when they have the same name -- here, unlike
``[[profiles::require]]``'s exact designator matching, arguments configure a
profile without changing its identity -- and all standard ``std::`` profiles
are mutually compatible.


Runtime-Checked Rules
=====================

P3589R2 anticipates profiles with *dynamic semantics*: rules enforced by a
runtime check in the generated code rather than by a compile-time diagnostic
(§1.1, §2.2.2).  Enforcing such a profile makes the compiler emit the
checks, and a failed check *traps* -- deterministically stopping the program
(typically ``SIGILL``) before the guarded operation executes.  There is no
handler library, no runtime error message, and no way to continue past a
failed check.  When debug info is enabled, the trap's debug location carries
a message naming the violated profile, which debuggers display.  At ``-O0``
every check site gets its own trap instruction with an exact source
location; optimized builds merge a function's profile trap blocks, like
UBSan's trap mode.  ``std::core_ub`` (see `The std::core_ub Profile`_) is
the real profile built from such rules.

Behavior of a runtime check:

- **Suppression applies identically.**  ``[[profiles::suppress]]`` removes a
  runtime check under the same dominion rule as a compile-time diagnostic: a
  suppression around a use site does not silence checks in a default member
  initializer or default argument emitted there -- those belong to the
  member's or parameter's construct, so suppress on the member or parameter
  instead.  The corners where a suppression's recorded dominion departs from
  its construct's tokens are listed in :doc:`ProfilesFrameworkInternals`.
- **System-header exemption.**  As for compile-time rules, code originating
  in a system header gets no checks by default (see `System Headers`_).
- **Sanitizer independence.**  The checks are emitted regardless of
  sanitizer configuration: a sanitizer guarding the same operation
  instruments it twice (redundant, never wrong), and no sanitizer facility
  (ignorelists, ``__attribute__((no_sanitize))``, hot cutoffs) can disable a
  profile's check, which is a language guarantee rather than opt-in
  instrumentation.
- **Per-unit enforcement.**  A check is emitted exactly when the
  translation unit *whose tokens the code is* enforces the profile there.
  An inline function defined in a textual header is therefore compiled with
  checks in enforcing TUs and without them elsewhere, and the linker keeps
  one copy arbitrarily -- as when mixing sanitized and unsanitized TUs.
  Code from a named module keeps the module's own enforcement: it is
  checked wherever it is emitted if the module interface enforced the
  profile, and not otherwise, whatever the importing TU enforces (see
  `Profiles and Modules`_).

The in-tree pilot rule (``test::arith`` / ``zero_divide``, which traps on
integer division or remainder by a runtime zero) also exists to exercise the
machinery and is inert outside the test suite (see `Test Profiles`_).  Trap
emission is described in :doc:`ProfilesFrameworkInternals`.

The zero-divisor checks (``std::core_ub``'s and the pilot's) deliberately
mirror UBSan's blind spots: GCC vector-extension integer division and
``_Complex int`` division are not checked.


The ``std::core_ub`` Profile
============================

``std::core_ub`` is an initial slice of the proposed profile from Vinnie
Falco's "A Profile for Runtime-Checkable Core-Language Undefined Behavior:
std::core_ub" (P4317; the case identifiers below, in braces, are from its
Appendix A).  Its guarantee: **a checkable core-language operation whose
precondition is violated traps rather than proceeding into undefined
behavior**.  It acts at run time: every rule is a
runtime-checked rule with exactly the semantics of the previous section --
the trap-only response, suppression of the whole profile or a single rule
at any granularity `Suppressing Enforcement`_ describes, the system-header
exemption, sanitizer independence, and per-TU enforcement.

This slice covers locally checkable cases of P4317 Appendix A.1:

.. list-table::
   :header-rows: 1
   :widths: 22 34 44

   * - Rule
     - P4317 case
     - Traps on
   * - ``zero_divide``
     - ``{expr.mul.div.by.zero}``
     - Integer division or remainder by zero.
   * - ``signed_overflow``
     - ``{expr.mul.representable.type.result}``
     - Signed ``+``, ``-``, ``*``, ``++``/``--``, and unary ``-`` overflow,
       and ``INT_MIN`` divided (or remaindered) by ``-1``.  The additive and
       multiplicative forms are silent when the dialect defines signed
       overflow (``-fwrapv``); division overflow is undefined even there and
       stays checked.
   * - ``invalid_shift``
     - ``{expr.shift.neg.and.width}``
     - A shift by a negative amount or by at least the width of the
       (promoted) left operand; before C++20, additionally a signed left
       shift moving a set bit out of the sign bit (from C++20 on that is
       defined, leaving the width arm only).
   * - ``misaligned_access``
     - ``{basic.align.object.alignment}``
     - An access through a pointer not suitably aligned for its type (a
       local whose storage is known to satisfy the alignment is not
       checked).
   * - ``null_dereference``
     - ``{expr.unary.dereference}``
     - An access through a null pointer -- dereference, member access,
       member call.  Operations where a null pointer is legal (a pointer
       downcast, ``dynamic_cast``, ``typeid``) are not checked, nor is a
       pointer statically known to be non-null (a local's address).
   * - ``out_of_bounds``
     - ``{expr.add.out.of.bounds}``
     - A subscript outside an array whose bound is known at the access --
       below the bound for an access, at most one past the end for a mere
       address -- and pointer arithmetic off such an array.  A constant
       index statically inside a constant bound is not checked.
   * - ``float_cast_overflow``
     - ``{conv.fpint.*}``, ``{conv.double.out.of.range}``
     - A floating-point value converted to an integer type that cannot
       represent it after truncation toward zero (including ±Inf and NaN).
       Conversions to a floating-point type are never checked: every
       floating range is [-inf, +inf].
   * - ``enum_out_of_range``
     - ``{expr.static.cast.enum.outside.range}``
     - A load of an enumeration value outside the enumeration's
       representable range.  An enumeration with a fixed underlying type
       has no such restriction and is never checked; ``bool`` loads are
       not this rule's concern.
   * - ``missing_return``
     - ``{stmt.return.flow.off}``
     - Flowing off the end of a value-returning function.

The many cases of P4317 that require whole-program bookkeeping or a
support runtime (heap bounds, lifetime, type confusion) are out of scope
for this slice.


Test Profiles
=============

Clang also ships six ``test::`` profiles (``test::type_cast``,
``test::uninit_read``, ``test::cfg_hooks``, ``test::class_final``,
``test::ctor_final``, and ``test::arith``) that exist only to exercise the
framework in the test suite.  They are inert without an additional ``-cc1``-only flag; see
:doc:`ProfilesFrameworkInternals`.


Not Yet Implemented
===================

``[[profiles::exempt(...)]]`` (P3589R2 §1.1.6), which would exempt named
included source files from the enforcement of a profile, is not implemented;
the `System Headers`_ exemption stands in for it.
