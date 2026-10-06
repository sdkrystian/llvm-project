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
one real profile, an initial slice of the proposed ``std::init``
initialization profile (see `The std::init Profile`_).  The
feature is experimental: attribute spellings, rule names, and diagnostics may
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

   [[profiles::enforce(std::init)]];
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
   [[profiles::enforce(std::init)]];  // error: does not precede 'x'

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

   [[profiles::enforce(std::init)]];

   void fill(char *buf, int n);

   int main() {
     [[profiles::suppress(std::init,
                          rule: "uninit_decl",
                          justification: "buffer is filled in by fill()")]]
     char buffer[1024];
     fill(buffer, 1024);
   }

A profile name the implementation does not know designates a profile with no
rules, and a ``rule:`` argument that names no rule of its profile suppresses
nothing.  ``-Wunknown-profiles`` warns about such a ``rule:`` of a profile
Clang implements, and about a profile name in an ``enforce`` or ``suppress``
attribute that is close to the name of one it implements.

A suppression covers exactly the tokens of the declaration or statement it
appertains to, nothing more; P3589R2 calls this token range the
suppression's *dominion*.  The declarator-id position works too: on a
function definition it covers the whole definition, mem-initializers and
body included, for free functions, inline members, and out-of-line
definitions alike (the attribute must be on the *definition*; one written
on a previous declaration does not carry over):

.. code-block:: c++

   void legacy_code [[profiles::suppress(std::init)]] () { /* exempt */ }

For a variable declaration the covered tokens include the initializer, so
violations inside the initializer are silenced; but the variable is *not*
marked as exempt at later uses, which appear in other declarations or
statements and are checked normally:

.. code-block:: c++

   [[profiles::suppress(std::init)]] int x;  // OK: uninit_decl suppressed
   int y = x;  // error: 'x' is read before initialization

In a declaration declaring multiple declarators, a suppression written on one
declarator covers that declarator only -- its declarator-id through the end
of its initializer -- never its siblings; a suppression written in the
declaration's prefix covers every declarator.

To exempt an object from a profile's checks everywhere it is used, use the
profile's own per-object marker instead (for ``std::init``, ``[[uninit]]``).


.. _profiles-diagnostic-groups:

Diagnostic Groups
=================

Every profile rule's diagnostic belongs to the diagnostic group
``-Wprofile-<profile>-<rule>``, nested in ``-Wprofile-<profile>`` and, with
every other profile, in ``-Wprofiles``; the group name lowercases the
profile and rule names and spells ``::``, ``_``, and ``.`` as ``-`` (a rule
``no_reinterpret`` of a profile ``acme::type`` is
``-Wprofile-acme-type-no-reinterpret``).  The group name only spells the
names: an attribute or ``-fprofiles-enforce=`` designates a profile, and a
``rule:`` argument a rule of it, by its exact name.  Enforcement and
suppression are expressed through these groups, so the ordinary diagnostic
controls apply to profile rules as well:

- A violation message ends in its group name, ``[-Wprofile-...]``, unless
  ``-fno-diagnostics-show-option`` is given.
- ``-Wprofile-<profile>`` or ``-Wprofile-<profile>-<rule>`` reports the
  rules as *warnings* without any enforcement, and ``-Werror=profile-...``
  as errors; ``-Weverything`` does not enable them.
- ``#pragma clang diagnostic ignored|warning|error "-Wprofile-..."``, with
  ``push`` and ``pop``, changes the severity of a rule that an enforcement
  or a ``-W`` option has enabled, for the rest of the file; a pragma alone
  does not enable a profile's checks.  ``-Weverything``, as an option or a
  pragma, never touches a rule.
- ``--warning-suppression-mappings`` exempts the listed paths from a
  ``-fprofiles-enforce=`` enforcement, not from one written in source.
- A ``-W`` option previews compile-time rules only; it never emits runtime
  checks (see `Runtime-Checked Rules`_).

``-Wno-profiles`` (or a narrower ``-Wno-profile-...``) does not disable an
enforcement; a violation stays an error under ``-w`` as well, and
``-Wno-error=profile-...`` has no effect on an enforced rule.  A
``#pragma clang diagnostic push`` before an enforcement and ``pop`` after it
leave the enforcement in place.


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
       [[profiles::enforce(std::init, vendor::hardened(fortify: 3))]];

   // user.cpp
   import M [[profiles::require(std::init)]];          // OK: M advertises it
   import M [[profiles::require(
       std::init, vendor::hardened(fortify: 3))]];     // OK: exact spellings
   import M [[profiles::require(vendor::hardened)]];   // error: not the
                                                       // advertised spelling
   import N [[profiles::require(std::init)]];          // error unless N
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
- A partition implementation unit (``module M:P;``) is not guaranteed to
  inherit them (see :ref:`profiles-known-gaps`): repeat the
  ``[[profiles::enforce]]`` there.

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
UBSan's trap mode.

Behavior of a runtime check:

- **Enforcement only.**  A runtime check is emitted only under an
  enforcement -- ``[[profiles::enforce]]`` or ``-fprofiles-enforce=``.
  Enabling a trap rule's diagnostic group with ``-W`` does nothing: an
  option can preview a compile-time rule but never instruments code.
- **Suppression applies identically.**  ``[[profiles::suppress]]`` removes a
  runtime check under the same dominion rule as a compile-time diagnostic: a
  suppression around a use site does not silence checks in a default member
  initializer or default argument emitted there -- those belong to the
  member's or parameter's construct, so suppress on the member or parameter
  instead.  The corners where a suppression's recorded dominion departs from
  its construct's tokens are listed in :ref:`profiles-known-gaps`.
- **System-header exemption.**  As for compile-time rules, code originating
  in a system header gets no checks by default (see `System Headers`_).
- **Sanitizer independence.**  The checks are emitted regardless of
  sanitizer configuration: a sanitizer guarding the same operation
  instruments it twice (redundant, never wrong), and no sanitizer facility
  (ignorelists, ``__attribute__((no_sanitize))``, hot cutoffs) can disable a
  profile's check.
- **Per-unit enforcement.**  A check is emitted exactly when the
  translation unit *whose tokens the code is* enforces the profile there.
  An inline function defined in a textual header is therefore compiled with
  checks in enforcing TUs and without them elsewhere, and the linker keeps
  one copy arbitrarily -- as when mixing sanitized and unsanitized TUs.
  Code from a named module keeps the module's own enforcement: it is
  checked wherever it is emitted if the module interface enforced the
  profile, and not otherwise, whatever the importing TU enforces (see
  `Profiles and Modules`_).

No real profile with runtime-checked rules ships yet; the in-tree pilot rule
(``test::arith`` / ``zero_divide``, which traps on integer division or
remainder by a runtime zero) exists to exercise the machinery and is inert
outside the test suite (see `Test Profiles`_).  Trap emission and the
pilot's known blind spots are described in :doc:`ProfilesFrameworkInternals`.


The ``std::init`` Profile
=========================

``std::init`` is an initial slice of the proposed initialization profile from
Bjarne Stroustrup's "An initialization profile" (P4222R1.1; the ``§``
references below are to that paper).  Its guarantee: **no object is read or
written before it is initialized**, enforced entirely at compile time.
Following the paper:

- Every object must be initialized at its point of definition, or be marked
  as intentionally uninitialized with the ``[[uninit]]`` attribute.
- An object marked ``[[uninit]]`` must be assigned before it is read, which
  is verified with simple, local flow analysis (§1.3).
- A pointer or reference to uninitialized memory must be marked with the
  ``[[ref_to_uninit]]`` attribute, and reads through it are rejected; a
  *handle* -- a smart pointer, a span, an iterator -- can carry the marker
  too (§4.6, §9.3).
- Non-local objects must be initialized at compile time (§3).
- Implicit initialization counts: a default constructor or the
  zero-initialization of statics initializes an object, and a class with a
  user-provided constructor is trusted to initialize its members (§5.1).

``[[uninit]]`` and ``[[ref_to_uninit]]`` are ordinary C++11 attributes,
recognized regardless of ``-fprofiles`` (see the :doc:`AttributeReference`
entries for their placement rules); the rules below carry weight only while
``std::init`` is enforced.  Reads and writes of ``std::byte`` objects are
exempt from all of them (§4.5), and a read of a ``std::nullptr_t`` object,
which accesses no storage ([conv.lval]p3), is never a read of uninitialized
memory.

**Marker placement.**  The marker family -- ``[[uninit]]``,
``[[ref_to_uninit]]``, ``[[now_init]]``, and ``[[now_uninit]]`` --
appertains to *declarations*, in either of the two standard positions: the
prefix decl-specifier position (``[[now_uninit]] void destroy_at(T* p);``)
or the declarator-id position (``int u [[uninit]];``,
``void destroy_at [[now_uninit]] (T* p);``).  The *type* positions some of
the paper's examples use -- after a ``*`` or ``&``, after an array bound,
or after a parameter list -- are rejected: neither
``int arr[10] [[uninit]];`` nor ``void destroy_at(T* p) [[now_uninit]];``
compiles.  A marker on a declaration whose type is deduced
(``auto p [[ref_to_uninit]] = ...;``, ``auto f [[ref_to_uninit]] () { ... }``)
is validated against the deduced type: a variable's at its initializer, a
function's or lambda's return type at the end of its body.  A structured
binding cannot carry ``[[ref_to_uninit]]``, whatever its type: the members it
names carry their own markers.  The markers' exact placement and spelling
track an open committee question (P4222R2 §4.7).

Each rule has a name, so it can be suppressed individually with
``[[profiles::suppress(std::init, rule: "name")]]`` (see `Suppressing
Enforcement`_):

=========================== ======================================================
Rule                        Diagnoses
=========================== ======================================================
``uninit_decl``             A variable left (partially) uninitialized without
                            ``[[uninit]]``.
``uninit_read``             A read of an uninitialized object, or of an
                            ``[[uninit]]`` object before it is assigned.
``uninit_write``            A write to a proper subobject of uninitialized
                            storage -- an ``[[uninit]]`` object, or storage
                            reached through ``[[ref_to_uninit]]`` or a
                            recognized allocator.
``ref_to_uninit``           A pointer or reference binding inconsistent with its
                            ``[[ref_to_uninit]]`` marking.
``double_destroy``          A ``[[now_uninit]]`` call or a destructor call on
                            storage already destroyed on some path.
``destroy_uninit``          A ``[[now_uninit]]`` call or a destructor call on
                            storage that is (still or again) uninitialized.
``ctor_uninit_member``      A constructor that leaves a member or base subobject
                            uninitialized.
``static_runtime_init``     A non-local variable with a runtime initializer.
``uninit_with_initializer`` ``[[uninit]]`` combined with an initializer, or
                            on an entity whose default-initialization is not
                            a no-op.
``pointer_marker``          ``[[uninit]]`` on a pointer.
``union_marker``            ``[[uninit]]`` on a union object or member.
``static_marker``           ``[[uninit]]`` on a variable with static or thread
                            storage duration.
=========================== ======================================================


Uninitialized Variables
-----------------------

An automatic-storage variable whose default-initialization would leave it --
or, for an aggregate, any scalar subobject (§5.4) -- indeterminate must
either be initialized or carry ``[[uninit]]`` (rule ``uninit_decl``):

.. code-block:: c++

   struct S { int x; };
   union U { int i; float f; };

   void f() {
     int a;             // error: uninitialized (uninit_decl)
     int b [[uninit]];  // OK: intentionally uninitialized
     int c = 3;         // OK
     S s;               // error: default-initialization leaves 's.x' indeterminate
     S t{};             // OK: value-initialized
     U u;               // error: an uninitialized union (§5.6)
     std::string str;   // OK: a user-provided default constructor is trusted
   }

A class type with a user-provided default constructor is trusted to
initialize its members (§5.1), and a data member that is itself marked
``[[uninit]]`` is acknowledged -- a type whose only indeterminate scalars are
all marked does not trigger the rule.  Marking a variable of such a type
``[[uninit]]`` is likewise consistent: its default-initialization is a
genuine no-op.  A union whose default-initialization activates a variant
through a default member initializer -- including initializers written on
the leaves of an anonymous-record variant, provided they initialize the
variant completely -- counts as initialized, and a union of only unnamed
bit-fields or only ``std::byte`` members has nothing to acknowledge (§4.5).


Where ``[[uninit]]`` May Not Go
-------------------------------

``[[uninit]]`` asserts that an object is genuinely uninitialized, so
placements that contradict that -- or that would defeat the profile's
guarantee -- are rejected:

.. code-block:: c++

   int g [[uninit]];    // error: statics are zero-initialized (static_marker)
   int *p [[uninit]];   // error: a pointer must be initialized, e.g. to
                        // nullptr (pointer_marker, §4.3)

   union U { int i; float f; };
   U u [[uninit]];      // error: delayed initialization of a union member
                        // would be erroneous (union_marker, §5.6)

   void f() {
     int x [[uninit]] = 4;      // error: marked *and* initialized
                                // (uninit_with_initializer, §4.2)
     std::string s [[uninit]];  // error: the default constructor initializes
                                // it (uninit_with_initializer)
   }

Each of these keys on the array element type, so an array of pointers or of
unions is rejected exactly like a single one.  A ``va_list`` is exempt from
``pointer_marker``: it is an opaque handle ``va_start`` initializes, so
``va_list ap [[uninit]];`` is accepted where ``va_list`` is a pointer (i386,
Windows) as elsewhere.  The rules apply in a fixed precedence, so a marked
entity draws exactly one of them: the subject's type first (a marked pointer or
union is rejected by ``pointer_marker`` / ``union_marker`` whatever its storage
duration or initializer), then static or thread storage duration, then a
contradicting initializer.  A no-op initialization -- the trivial
default-initialization of a scalar or aggregate that leaves a scalar subobject
indeterminate -- is consistent with the marker; any other synthesized
initialization contradicts it and is rejected (§5.3): a member or base with a
user-provided default constructor, a default member initializer, a virtual table
pointer, or a value-initialization (``= P()``), all of which initialize
something.  ``uninit_with_initializer`` reports the two cases differently: a
*written* initializer contradicts the marker outright, while a
*default*-initialization that is not a no-op is reported against the type,
with a note giving the reason (a constructor runs, a default member
initializer runs, nothing is left indeterminate, or the type has no usable
default constructor).  The same rule covers a marked data member whose type's
default-initialization is not a no-op:

.. code-block:: c++

   struct Str { Str() : cap(0) {} int cap; };
   struct S { int x; Str s; };

   void g() {
     S s4 [[uninit]];   // error: default-initialization of 'S' does not leave
                        // it uninitialized -- 's4.s' is default-constructed
                        // (uninit_with_initializer, §5.3)
   }

   struct Buf {
     Str s [[uninit]];  // error: default-initialization of 'Str' runs a
                        // constructor, so 's' cannot be left uninitialized
     int n [[uninit]];  // OK: a scalar member really is left uninitialized
   };


Reads of Uninitialized Objects
------------------------------

An uninitialized object must not be read (rule ``uninit_read``).  Local flow
analysis verifies reads of uninitialized locals, of ``[[uninit]]`` members
within the defining constructor's body, and of ``[[uninit]]`` members of
constructor-less aggregate locals -- also inside a member subobject
(``o.m.n``, ``this->m.n``) that has no default member initializer and whose
class has no user-provided default constructor, so that ``m``'s written
initializer,
``std::construct_at(&o.m)``, a ``[[now_init]]`` call, or ``memset`` of it
initializes ``n``, and an ``[[uninit]]`` member of a class type is tracked as
a whole object (below); reads through ``[[ref_to_uninit]]``
pointers and references and reads of subobjects of ``[[uninit]]`` objects
(copying an ``[[uninit]]`` array -- a by-copy capture ``[arr]``,
``auto [a, b] = arr;`` -- reads each element, and copying a whole class
object -- the source of a copy or move constructor or assignment, a by-value
argument, ``__builtin_bit_cast`` or ``std::bit_cast``, an operand of a
defaulted comparison, the source of ``memcpy`` or ``memmove``, an argument of
``std::construct_at`` or the ``std::uninitialized_*`` family of the type they
construct -- reads every subobject) are rejected unless
the storage is initialized on every path reaching the read (§1.3) -- by a
whole-entity store, a ``[[now_init]]`` call, or ``std::construct_at``; a path
past a mutable alias of the pointer, which leaves the referent's state
unknown, does not count against the read (see
`Binding Pointers and References`_):

.. code-block:: c++

   struct Buf { int n [[uninit]]; };

   int f(int *p [[ref_to_uninit]]) {
     int x [[uninit]];
     int a = x;    // error: 'x' is read before it is assigned
     x = 3;
     int b = x;    // OK: assigned on every path reaching the read

     Buf buf;
     int c = buf.n;  // error: 'buf.n' is not yet assigned
     buf.n = 1;
     int d = buf.n;  // OK

     [[uninit]] int arr[4];
     int e = arr[0]; // error: array elements are not tracked; only a
                     // whole-object initialization can give 'arr' a value

     return *p;      // error: read through [[ref_to_uninit]] (§4.5)
   }

   struct T {
     int m [[uninit]];
     T(int v) {
       int r = m;    // error: 'm' is read before the body assigns it
       m = v;        // OK: for a built-in type, a write is its
     }               // initialization (§4.5)
   };

An ``[[uninit]]`` member of a class type is initialized only as a whole --
by ``std::construct_at``, a ``[[now_init]]`` call, ``memset``, ``memcpy``,
``memmove``, or a written mem-initializer -- and a read of a subobject of it
is rejected unless that has happened on every path reaching the read; its
assignment (``s = S{}``) is a member call on uninitialized storage, and a
member-wise store initializes nothing (see
`Writes to Subobjects of Uninitialized Objects`_):

.. code-block:: c++

   struct S { int x; int y; };

   struct T {
     S s [[uninit]];
     int v;
     T() : v(0) {
       v = s.x;                      // error: 's' is not initialized
       std::construct_at(&s, 1, 2);
       v = s.x;                      // OK
     }
     int get() { return s.x; }       // OK: trusted outside the constructor
   };

Outside the contexts whose reads are checked -- another member function, an
object reached through a pointer or reference, a global, a base whose
user-provided constructor initializes it -- reads and stores of an
``[[uninit]]`` member and of its subobjects are trusted (§5.1): ``p->s.x = 1``
and ``return p->a[1];`` are accepted.  Taking its address, binding a reference
to it, calling a member function on it, and destroying it still count the
marker (see `Binding Pointers and References`_), and so does a read or store in
a default member initializer (``int v = s.x;``), which runs before any
constructor body.

A member (each base-class copy of a member separately) or variable counts as
assigned only when every path to the read assigns it (§1.3); for a member,
exception paths are included: a call inside a ``try`` block may transfer
control to the handler before the assignment that follows it (the analysis of a
plain local considers no exception paths; see :ref:`profiles-init-known-gaps`).
A compound assignment (``x += 1``) or an increment or decrement reads the old
value first, so it is diagnosed like a read.  A comma or conditional lvalue,
written whole or below a member access, subscript, indirection, or ``&``
(``(c ? a : b).m``), reads (or assigns) whichever member the chosen arm names;
an assignment through a conditional whose arms name different members assigns
neither for the purpose of a later read.  An access through a pointer to data
member (``a.*pm``, ``p->*pm``) is a member access whose member is not known:
it reads or writes a subobject of ``a`` or of ``*p``, and on the current object
-- or, as the last step, on a tracked local -- a read through it reads every
tracked member while a store through it initializes none.  Inside a
constructor body only that
plain whole-member assignment counts as initialization: passing ``&m`` to a
function (even one whose parameter is marked ``[[ref_to_uninit]]``), binding a
reference to the member, calling a member function, or letting ``this`` escape
does not count as initializing ``m``; suppress the rule where such a flow is
intended.  A ``this``-capturing lambda might run immediately, so member reads in
its body count at the point the lambda is created (and writes there initialize
nothing -- the same strict policy).  For a *local* variable the analyses instead
treat any escape as an assignment (see :ref:`profiles-init-known-gaps`).  A
local copied or moved from a tracked local (``Agg b = a;``,
``Agg b = std::move(a);``) is tracked too: the copy's members start in the
source's per-member state at the copy point; a member of class type is
copy-constructed from the source's, which must then be initialized on every
path, and starts initialized.  A by-value *parameter* of such a
class that declares no default argument is likewise a copy, of the caller's
argument, so its marked members are tracked from an unassigned start: a read
before a local assignment (or an escape of the parameter) is rejected even if
the caller assigned the member first; ``[[profiles::suppress]]`` is the remedy
where the by-value flow is intended.  Members of an object initialized by a
*user-provided* constructor are trusted (§5.1) and not flow-tracked; only the
defining constructor itself is checked, and a *delegating* constructor's body is
not: its target initializes the members first (§5.1).  A *union's* own
constructor is not flow-analyzed at all: its members are mutually exclusive,
so whether the active member is set is deferred (§5.6), matching
``ctor_uninit_member``'s union exemption.


Writes to Subobjects of Uninitialized Objects
---------------------------------------------

Piecemeal delayed initialization of an ``[[uninit]]`` object through its
members or elements cannot be validated statically (§5.4, §5.5), so a store
to a *proper subobject* of an ``[[uninit]]`` entity is rejected (rule
``uninit_write``) unless the entity is initialized on every path reaching the
store (§1.3) -- by ``std::construct_at`` or a ``[[now_init]]`` call; writing
the whole entity is that entity's initialization, stays legal, and initializes
the entity for every path after it (see `Binding Pointers and References`_):

.. code-block:: c++

   struct S { int x; int y; };

   void f() {
     S s [[uninit]];
     s.x = 1;   // error: writing a member of an [[uninit]] object (uninit_write)
     [[uninit]] int a[2];
     a[0] = 1;  // error: writing an element of an [[uninit]] object
     int v [[uninit]];
     v = 7;     // OK: the write initializes the whole entity
   }

   void g(int *p [[ref_to_uninit]], S *ps [[ref_to_uninit]]) {
     *p = 5;    // OK: a write through the marker is the pointee's
                // initialization
     ps->x = 1; // error: writing a member of uninitialized storage reached
                // through a [[ref_to_uninit]] pointer (uninit_write)
   }

A class-type ``[[uninit]]`` member is such an entity wherever its reads are
checked (see `Reads of Uninitialized Objects`_), when its class's default
constructor is not user-provided: ``s.x = 1`` in its constructor before
``std::construct_at(&s)`` is rejected, and the same store in another member
function is trusted.  The scope exit of a constructor-less aggregate local
destroys such a member whose destructor is non-trivial, judged like an
``[[uninit]]`` local's scope exit (`Binding Pointers and References`_).

Subobject writes below the marker are rejected like the named twin: a
write below a member step initializes nothing (§5.4's piecemeal ban; §4.5
makes only the whole scalar write an initialization), whether the storage
is reached through a marked pointer, reference, member, or callee return,
through a recognized allocator's raw result, or through a raw ``new``
expression.  A class pointee's initialization is ``construct_at`` (its
``[[ref_to_uninit]]`` parameter accepts the pointer) or, for a scalar
pointee, the whole ``*p`` store.  For a marked local pointer or reference the
store is accepted once its referent is initialized on every path reaching it
(``construct_at(p, ...)`` or a ``[[now_init]]`` call first).

A compound assignment or increment/decrement of such a subobject also reads
its old value, so the read diagnostic fires alongside this one.  Assigning a
whole *class* object marked ``[[uninit]]`` (``s = S{1, 2};``) is caught too:
the member ``operator=`` binds the uninitialized object as its implicit
object argument (see the next section).


Binding Pointers and References
-------------------------------

A pointer or reference must be bound consistently with its
``[[ref_to_uninit]]`` marking (rule ``ref_to_uninit``, §4.3): a marked
pointer, reference, or function return may only refer to uninitialized
memory, and an unmarked one only to initialized memory.  Whether a source
refers to uninitialized memory is recognized from its form -- the address
(``&x``, ``std::addressof(x)``) or a subobject of an ``[[uninit]]`` entity, the
value or dereference of a marked pointer or reference, pointer and reference
casts of those and the standard identity functions applied to them, a call to a
marked function, a call to a known allocator (``malloc``, ``aligned_alloc``,
``alloca``, raw ``::operator new``, ``std::allocator<T>::allocate``, and
``std::allocator_traits<A>::allocate`` calls return uninitialized memory, so
do the handles ``std::make_unique_for_overwrite``,
``std::make_shared_for_overwrite``, and ``std::allocate_shared_for_overwrite``
return where default-initialization of their object leaves a scalar
indeterminate (see `Smart Pointers, Spans, and Other Handles`_),
``calloc`` zero-initialized memory, and ``realloc`` is unclassified; §4.3 --
trusted recognition keys on Clang's builtin IDs, and where those are absent,
under ``-fno-builtin`` or ``-ffreestanding``, a name-recognized allocator is
*untrusted*: unclassified, so neither binding direction diagnoses -- never
trusted as initialized like an arbitrary callee; the
``operator new``/``operator delete`` families are recognized by form and keep
their recognition everywhere), and a ``new`` expression that default-initializes
a type with indeterminate scalars, ``[[uninit]]`` members included (``new int``,
``new int[n]``; §1.2) -- refined, inside a function body, by the flow state of
tracked storage (below).
A named *function's* decayed pointer value is none of these: its
``[[ref_to_uninit]]`` describes the return value, so binding the function itself
to a function pointer is accepted while a call to it stays a marked source:

.. code-block:: c++

   int i = 7;
   int u [[uninit]];

   int *p1 = &i;                         // OK
   int *p2 = &u;                         // error: needs [[ref_to_uninit]]
   int *p3 [[ref_to_uninit]] = &u;       // OK
   int *p4 [[ref_to_uninit]] = &i;       // error: 'i' is initialized
   int *p5 [[ref_to_uninit]] = new int;  // OK: uninitialized new (§1.2)

   void sink(int *q);
   void fill(int *q [[ref_to_uninit]]);

   void f() {
     sink(p3);  // error: 'sink' expects initialized memory
     fill(p3);  // OK
   }

The check applies wherever a pointer or reference is bound: variable, member,
and aggregate initialization, assignment, call arguments (including defaulted
and variadic ones; a builtin that never evaluates its arguments, such as
``__builtin_object_size``, binds none), the ``va_list`` operand of ``va_arg``
where ``va_list`` is a class type, Objective-C message arguments, a property
assignment's value included (``a.p = &u`` binds the setter's parameter;
a variadic method's extra arguments bind nothing), ``return`` and ``throw``
statements (including returns inside lambdas and blocks -- a lambda's marker is
written on its call operator, in the C++23 attribute position after the
lambda-introducer: ``[] [[ref_to_uninit]] () { ... }``, with a deduced or
trailing return type), lambda captures (by reference and by copy -- a closure
field cannot carry the marker, so a by-copy capture of a marked pointer is
rejected like any unmarked pointer copy; capture by reference, or store through
the marker first, if the flow is intended), and the implicit object argument of
a member call (including a ``__declspec(property)`` access's accessor call and
``co_await``'s awaiter calls) -- so calling a member function on an object
recognized as uninitialized storage is rejected, and so is copying a class
object out of one; a copy out of an ``[[uninit]]`` member reads it, so it is
trusted where its reads are (see `Reads of Uninitialized Objects`_).  A
structured binding names the member or element it decomposes, marker included,
so after ``auto [a, b] = s;`` a use of ``b`` is judged by the marking of the
field it names.  For the copy, the escape is the
paper's own (§7.2): declare the copy constructor's parameter
``[[ref_to_uninit]]``.  Positions that cannot carry the marker -- a variadic
argument, a parameter of a function called through a function pointer or a
pointer to member function, the implicit object parameter, a pointer element of
an array in aggregate initialization -- are checked as unmarked targets;
suppress at the call site if the flow is intended.  A reference to a *const*
pointer, or an rvalue reference to a pointer, is bound by the pointer's value:
``int *const &r = &u`` needs the marker exactly as ``int *r = &u`` does.  A
reference to a *non-const* pointer aliases the pointer object itself and is
neither checked nor tracked.  An assignment whose target is a conditional,
comma, or GNU ``?:`` expression stores to whichever pointer the chosen arm
names: arms that agree on their marking are checked under it, and arms of mixed
marking accept either source; a target that does not directly name a pointer
declaration -- a reference to a pointer, ``*pp``, an array element -- has
unknown marking and accepts either source as well.  A conditional *source* whose
arms disagree -- one initialized, one uninitialized -- is rejected for either
target (P4222R2 §4.9: mixing requires suppression).  The same holds below a
member access, subscript, indirection, or ``&``: ``*(c ? p : q)`` is judged as
``c ? *p : *q``.  A parameter's marker written on any declaration of the
function is inherited by the parameter's later redeclarations, so a header's
marker carries to the source file's definition (the §7.2 header/source split);
the definition keeps its read-through checking and call sites after the
redeclaration still see the marker.  A null pointer source -- ``nullptr``,
``0``, ``{}``, or a local variable initialized to null -- refers to no object,
so it is accepted for marked and unmarked targets alike (§4.3, §8: the marker
means "zero or more uninitialized objects"); a *parameter* with a null default
argument is not a null source (callers may pass any pointer).  A source whose
form the recognizer cannot classify (pointer arithmetic, an integer-to-pointer
cast) is likewise accepted for either target.  A GNU statement expression yields
its last expression and a pointer compound literal its initializer:
``int *q = ({ &u; })`` and ``int *q = (int *){&u}`` bind ``&u``.  ``this``
outside a constructor is initialized, so a marked pointer or reference to an
initialized member of the current object is rejected; inside a constructor body,
mem-initializer, or default member initializer it is unclassified.

The recognizer's answer is refined by *flow-tracked storage*: an
``[[uninit]]`` local, the referent of a ``[[ref_to_uninit]]`` local pointer
or reference (parameters included), an ``[[uninit]]`` scalar member of a
directly named local or of the current object, directly or inside a member
subobject, and, in a member function, the referent of a ``[[ref_to_uninit]]``
pointer member of the current object (``*elem``, ``mem.elem->x``), which the
marker asserts uninitialized when the function is entered -- so the
destructor's ``std::destroy(elem, elem + n)`` is accepted, while a read through
``elem`` is rejected until the same function stores or constructs the
referent; a member call on the object holding the member, a pointer or
reference to that object, or a ``this`` capture leaves the referent unknown.  A
marked pointer's or
reference's referent is, at each point, the storage its latest binding
names: after ``int *p [[ref_to_uninit]] = &u;`` the local ``u``, after
``p = q`` whatever ``q`` refers to there, after ``p = &a.m`` that member.
From the binding on, the marker asserts the referent uninitialized under
every name that reaches it -- after ``int *p [[ref_to_uninit]] = &u;`` an
unmarked ``&u`` is rejected until ``u`` is stored -- and a destroyed
referent stays destroyed.  A source the analysis does not track (an
allocation, pointer arithmetic, a subobject, a decayed array) gives it an
anonymous referent; paths that bind it differently leave the referent
unidentified: nothing is asserted and nothing fires through the pointer.  Each
execution of a tracked local's declaration -- a loop's next iteration, a
backward ``goto`` -- begins a new, uninitialized object.  For tracked storage
the binding is judged where it occurs, by the same local analysis that
checks reads (§1.3): a whole-entity store (``u = 5``, ``*p = 5``, ``r = 5``,
``a.m = 5``, ``this->m = 5``), a ``[[now_init]]`` call, or
``std::construct_at`` initializes the storage under whichever name reaches
it -- ``*p = 5`` initializes ``u``, and ``u = 5`` what ``*p`` reads; handing
out a mutable alias of a marked pointer (``T *&``, ``T **``, a by-reference
capture) gives it an anonymous referent of unknown state; a
``[[now_uninit]]`` call, ``std::destroy_at``, ``free``, ``realloc``,
``operator delete``, or a ``delete`` expression acts on the referent
(below).  A marked target is rejected when the
storage is initialized on every path reaching the binding; an unmarked
target is accepted when the storage is initialized on some path.  A local or
by-value parameter with tracked ``[[uninit]]`` members, bound as a whole to a
marked target (``std::construct_at(&a, 1, 2)``), is judged by those members
-- those of the base it is converted to, if any: it is initialized when every
one is initialized on every path, and uninitialized when none may be; an
unmarked binding of it is judged by its form, and so is an object whose
default-initialization runs code (a constructor or a default member
initializer), which constructed it.  A marked pointer or reference bound to
it refers to no storage the analysis identifies.  Passing
to a ``[[ref_to_uninit]]`` parameter is judged the same way and asserts
nothing after the call.  Element
accesses (``p[i]``, ``a[0]``) are never initialized by an element store, and
a store through a conditional target (``(c ? p : q) = e``) initializes
neither arm for the purpose of a later marked binding.  A lambda's or
block's body is its own function: a store inside it initializes nothing for
the enclosing body, and a by-reference capture of an uninitialized
``[[uninit]]`` variable is the unmarked-reference violation even when the
body assigns it.  Storage outside any function body -- a namespace-scope
object, a default member initializer, a default argument -- has no flow and
is judged by its form alone.  A default argument is judged where it is
declared (once per instantiation of a template), never at a call that uses
it, so a suppression for it belongs on the declaration.  A transparent cast is as transparent to a
store as to a binding: ``(int &)u = 5`` initializes an ``int u`` whole,
``*(int *)p = 5`` the ``int`` referent, and ``(int *&)p = q`` reseats ``p``
(§4.3).  A store or a credit -- ``construct_at``, the ``uninitialized_*``
family, a ``[[now_init]]`` call -- initializes the storage only when it
designates the whole of it: one through another type than the storage's
(same-size signed and unsigned integer types agree, and an array counts by its
element type, though an array type must span the whole array) or through a
conversion to a base class names a subobject,
initializes nothing, and as a store is rejected like ``s.x = 1``
(``*(int *)&s = 1``, ``std::construct_at(static_cast<Base *>(&d))``).
``std::construct_at`` constructs one object, so on an array it constructs the
first element and initializes nothing.
``memset``, ``memcpy``, ``memmove``, and a ``[[now_init]]`` parameter whose
pointee is ``void``, ``char``, ``unsigned char``, or ``std::byte`` claim the
bytes and initialize the whole storage, unless reached through a conversion to
a base class.
The standard identity functions -- ``std::move``, ``std::forward``,
``std::forward_like``, ``std::move_if_noexcept``, and ``std::as_const`` -- are
as transparent as such a cast: ``*std::move(p)`` reads what ``*p`` reads,
``int *q = std::move(p)`` binds what ``int *q = p`` binds, and the call itself
binds nothing; ``std::addressof(x)`` is ``&x``.  An indirection through an
address-of step names the object: ``*&x``, ``(&x)->m``, and
``std::addressof(x)->m`` are ``x`` and ``x.m``.  Whole-object assignment of a
class-type referent never initializes: it is a member ``operator=`` call on
uninitialized storage, rejected as above.

One kind of call *does* count as initialization: §6.2's ``[[now_init]]``
attribute (its placement and spelling track an open committee question)
declares that a function initializes the storage passed to each of its
``[[ref_to_uninit]]`` parameters, and it requires at least one such
parameter.  A call to a ``[[now_init]]`` function initializes the
argument's storage exactly as the equivalent direct store would --
``fill(&u)`` initializes ``u`` whole, ``fill(p)`` the storage ``p`` refers
to there, ``fill(&a.m)`` the member ``m`` of ``a``,
and ``fill(arr)`` an ``[[uninit]]`` array whole, local or member (§6's
``uninitialized_fill`` shape; the element form ``fill(&arr[0])`` initializes
nothing, §5.4) -- with the same reverse-direction consequence (after a
``fill(&u)`` on every path, a second ``fill(&u)`` is rejected: ``u`` no
longer refers to uninitialized memory, which incidentally catches double
``construct_at``).  This is the library annotation P4222R2 §4.4 requests, and
Clang supplies it itself: a call to a ``std::construct_at`` whose first
parameter is of pointer type is checked and credited as if the function were
declared ``[[now_init]]`` with that parameter marked, so the real ``<memory>``
declaration -- included textually or imported from a module or header unit,
whatever options built it -- checks the lifecycle *start* with no user
declaration; so is ``std::allocator_traits<A>::construct``, the allocator-aware
``construct_at``, on its pointer argument -- except that when ``A`` or a base
of it declares its own ``construct`` outside namespace ``std``, a call with no
constructor arguments is credited like ``uninitialized_default_construct``
(below), as the allocator may default-initialize.  The
``std::uninitialized_*`` family (P4222R2 §4.7, §6) is treated the same way on
the parameter that receives
the output range:
``uninitialized_fill``, ``uninitialized_copy``, ``uninitialized_move``, and
``uninitialized_value_construct``, each with its ``_n`` form and its
execution-policy overload, so the range must refer to uninitialized memory -- an
initialized range is rejected, like the paper's
``uninitialized_fill(a1, 10)`` -- and is initialized whole by the call, while
the source range of the copy and move forms is an ordinary unmarked parameter.
The end iterator of the output range (``uninitialized_fill``,
``uninitialized_value_construct``, and ``uninitialized_default_construct``)
bounds the range: it is accepted in any state, marked or not, and the call
neither initializes nor destroys what it points to.
``uninitialized_default_construct`` and its ``_n`` form initialize the range
only where default-initialization of the value type leaves no scalar
indeterminate -- a user-provided default constructor, a default member
initializer on every scalar, ``std::string`` (§2) -- and otherwise require an
uninitialized range that stays uninitialized (``int``, a trivial class).  An
``[[uninit]]`` member of the value type stays uninitialized after the call: a
read of it through the range -- or through a base class view of it -- is
rejected until it is stored, and so is a copy of a whole object while one is
uninitialized, while the objects are alive: constructing them again is
rejected, an ordinary pointer may refer to them, and a marked pointer may
refer to such a member until it is stored.  The
treatment applies where the iterator is a pointer (``arr, arr + n``), and a
decayed ``[[uninit]]`` array member is its whole array: in a constructor,
``std::uninitialized_fill(arr, arr + n, v)`` initializes the member ``arr``
and ``std::destroy(arr, arr + n)`` destroys it.  The ``uninitialized_*``
family also takes a class-type iterator, a handle (see
`Smart Pointers, Spans, and Other Handles`_): it must refer to uninitialized
memory, so ``std::uninitialized_fill(v.begin(), v.end(), x)`` on an
initialized ``std::vector`` is rejected, and the call initializes the storage
the iterator refers to when the analysis tracks it whole
(``std::span<int>(arr).begin()``), but not through an offset iterator
(``s.begin() + 1``); the value type is the iterator's or range's
``element_type`` or ``value_type``, else what its ``operator*`` returns, else
(``std::ranges::subrange``) its iterator's.  The
``std::ranges`` objects of the same names -- ``ranges::construct_at``,
``ranges::destroy_at``, ``ranges::destroy``, ``ranges::destroy_n``, and the
``ranges::uninitialized_*`` family -- are treated the same way where the
iterator is a pointer or the range is an array, and the
``ranges::uninitialized_*`` objects also where the iterator or the range is
of class type (a range by its storage, then by what it refers to):
``std::ranges::uninitialized_fill(arr4, xval)`` initializes an ``[[uninit]]``
array ``arr4`` whole (§5.5), and the sentinel of an output
range, like an end iterator, is accepted in any state.  The §4.4
``now_init()`` identity function is treated the same way: the parameter of a
``std::now_init`` with one pointer parameter is treated as marked, so its
argument must refer to uninitialized memory, its unmarked return is trusted as
initialized (``*std::now_init(p)`` reads the storage ``p`` refers to without
verification -- the paper's "cast in disguise"), and the argument's own storage
keeps its state (§6.1: use the returned pointer, not the original name); only
the attribute legalizes the original *name* after the call.  The treatment is
the call's alone: inside a library's definition the parameters carry only the
markers the library wrote.  The paper's marked definition
``T* now_init(T* p [[ref_to_uninit]]) { return p; }`` violates the binding
rule -- in a system header the `System Headers`_ exemption covers it, under
``-fno-profiles-exempt-system-headers`` the library writes
``[[profiles::suppress(std::init)]]`` on it, and outside a system header it is
diagnosed per instantiation -- while an unmarked definition is judged as
written.

The lifecycle *end* is the mirror attribute ``[[now_uninit]]`` -- the
recording §4.4 notes is missing for ``destroy_at`` ("the object subjected to
``destroy_at()`` should be considered uninitialized, but there is no way of
recording that in the code").  It declares that a function ends the lifetime
of the storage passed to each of its pointer or reference parameters (which
are unmarked; apply the attribute only to functions that destroy *every*
such argument's storage): after the call the storage is uninitialized again
-- re-construction is legal, and binding it to an unmarked pointer or
reference is the unmarked-direction violation -- and destroyed.  The
parameters themselves take storage that is initialized: destruction makes an
object uninitialized (§1), so destroying storage that is still -- or again
-- uninitialized on every path reaching the call is itself an access to raw
memory, rejected as rule ``destroy_uninit`` (§4.4: it is an error to
uninitialize an object twice); storage initialized on some path is
accepted, and so is storage of unknown state.  A storage-release callee
keeps any-state acceptance (``free`` takes storage that may never have been
constructed), a reinitializer (below) is exempt, and so
is any parameter that itself carries ``[[ref_to_uninit]]``: the marker
declares that the parameter accepts uninitialized storage -- the annotation
spelling for an unrecognized release function.  Never accepted is storage
destroyed on some path reaching the call and not stored since: a double
destruction on that path (rule ``double_destroy``).  A destroy on one path only
leaves the storage neither definitely initialized nor definitely destroyed: a
marked binding is not forced, an unmarked binding stays accepted, a second
destroy is a double destroy, and a read through a marker, a read of a
subobject of an ``[[uninit]]`` object, or a subobject write is rejected.  Clang
treats ``std::destroy_at`` this way itself -- a call to a ``std::destroy_at``
whose first parameter is of pointer type destroys that argument's storage, and
so does a call to ``std::destroy`` or ``std::destroy_n`` (P4222R2 §4.4) or
their execution-policy overloads for its first iterator, a pointer or a
class-type iterator (``std::destroy(s.begin(), s.end())`` destroys what a
marked span refers to), and to ``std::allocator_traits<A>::destroy`` for its
pointer argument unless ``A`` declares its own ``destroy`` (as for
``construct``), a call that leaves the storage possibly initialized;
the end iterator is accepted in any state, as for ``uninitialized_fill`` -- so
the construct/destroy/construct cycle is legal, a second destruction is
rejected, a destroy of never-constructed storage is rejected too, and binding
the destroyed storage to an ordinary pointer or reference is rejected as the
unmarked-direction violation.  An
explicit destructor call (``p->~T()``, ``s.~T()``, and a scalar's
pseudo-destructor call) is the same destroy -- ``std::destroy_at`` is specified
as one ([specialized.destroy]) -- and the diagnostics name it a destructor
call.  So is the destructor that ends an ``[[uninit]]`` local's scope when its
type's destructor is non-trivial: such a local must be constructed on some path
before its scope ends (``destroy_uninit``) and must not be destroyed on any
path before (``double_destroy``); the diagnostic is at the scope's end, and a
suppression on the declaration covers it.  Outside its constructors, a member
function that destroys the current object (``std::destroy_at(this)``,
``this->~T()``, ``delete this``) or binds it to a lifecycle parameter ends and
restarts its lifetime: after the destroy, an access to a member is rejected
until ``std::construct_at(this)`` reconstructs it, and ``construct_at(this)``
on the live object is rejected.  A function may carry both
attributes -- a reinitializer that destroys and then reconstructs its
argument's storage -- and models destroy-then-construct: the storage bound to
its ``[[ref_to_uninit]]`` parameters is initialized after the call, and calling
it on already-initialized storage is exactly its purpose, so it is accepted.
Neither attribute can be applied to a constructor; a static member or free
function carries it instead
(``[[now_init]] static Filler make(int *p [[ref_to_uninit]]);``).  Storage
outside any function body is judged by its form: a destroy of a
namespace-scope ``[[uninit]]`` object is ``destroy_uninit``.

The standard storage-release callees are recognized without annotation and
treated like ``[[now_uninit]]`` at the call: ``free``, ``realloc`` (its pointer
argument), ``std::allocator<T>::deallocate`` and
``std::allocator_traits<A>::deallocate`` (their pointer argument), and
replaceable global ``::operator delete`` / ``::operator delete[]`` (sized and
nothrow forms included; class-specific and destroying overloads excluded) accept
a pointer in any state -- an RAII buffer's ``free(p)`` on a
``[[ref_to_uninit]]`` member is legal whether or not the buffer was ever
written -- and end what is known about the storage, so a read through a marked
pointer after ``free(p)`` is diagnosed again.  A release records no *destroyed*
state, though: releasing storage ends no object's lifetime, so both orders
relative to a ``[[now_uninit]]`` call sit outside ``double_destroy``'s scope --
``destroy_at(p); free(p);`` is correct (different operations, correctly
ordered).  The reverse order, ``free(p); destroy_at(p);`` on a *marked* pointer,
is the ``destroy_uninit`` violation: after the release the pointee is
uninitialized again when the destroy takes it.  Through an unmarked pointer the
release changes no classification -- post-release use stays the invalidation
profile's concern -- and only a second ``[[now_uninit]]`` destroy of the same
storage fires ``double_destroy``.  The ``delete`` and ``delete[]`` *expressions*
perform the same release, likewise recording no destroyed state, so
``delete q;`` and ``::operator delete(q);`` agree on everything that follows;
when the deleted type has a non-trivial destructor, the expression first
destroys the operand's referent like an explicit destructor call, so
``delete p;`` of storage never constructed is ``destroy_uninit``.  Like the
allocator side, trusted ``free``/``realloc`` recognition
keys on Clang's builtin IDs.  Where those are absent (``-fno-builtin``,
``-ffreestanding``) a ``free`` or ``realloc`` recognized by name still accepts a
pointer in any state but records no release, so a later read or destroy through
the pointer is not diagnosed there (see :ref:`profiles-init-known-gaps`).

The standard byte-writing callees ``memset``, ``memcpy``, and ``memmove``
are recognized without annotation as initializing their destination: the
destination accepts storage in any state and is initialized whole by the
call, whatever the byte count -- ``memset(arr, 0, sizeof arr)`` initializes
a local ``[[uninit]]`` array, ``memset(p, 0, n)`` the storage a marked
pointer refers to (not its further elements: ``p[1]`` stays a read through
the marker) -- and a second call on the same storage is no double
initialization.  The returned destination is initialized memory.  The
source of ``memcpy`` and ``memmove`` is an ordinary unmarked parameter, so
an uninitialized source is rejected.  Recognition keys on Clang's builtin
IDs, and the fortified ``__builtin___memset_chk``, ``__builtin___memcpy_chk``,
and ``__builtin___memmove_chk`` that ``_FORTIFY_SOURCE`` headers expand to
are recognized the same way; by name (``-fno-builtin``, ``-ffreestanding``)
only the acceptance survives, so a later read of the destination is
diagnosed there, while the ``__builtin_`` spellings keep their IDs
everywhere.

``va_start`` and ``va_copy`` are recognized the same way: their ``va_list``
destination accepts storage in any state and is initialized by the call, so
``va_list ap [[uninit]]; va_start(ap, n); ... va_end(ap);`` is the variadic
idiom (a ``va_list`` with neither an initializer nor the marker is
``uninit_decl``), while ``va_copy``'s source and ``va_end``'s operand are
ordinary unmarked parameters, so an uninitialized ``va_list`` is rejected there.


Smart Pointers, Spans, and Other Handles
----------------------------------------

An object of class type that refers to memory -- a smart pointer, a span, an
iterator -- is a *handle*, and any non-union class may carry
``[[ref_to_uninit]]`` like a pointer: a variable, a parameter, a data member,
or a function's return (§4.6, §9.3).

.. code-block:: c++

   void init(std::span<X> s [[ref_to_uninit]], int i1, int i2);
   void make() { std::unique_ptr<int> p [[ref_to_uninit]]; }

A union, an array of handles, and a structured binding cannot carry the
marker.  A reference to a class keeps its meaning: it refers to the class
object itself.

A pointer or reference that a member function of a handle returns refers to
memory the handle refers to, so for a marked handle it refers to
uninitialized memory: ``s[i]``, ``s.front()``, ``*s.begin()``, ``*p``,
``p.get()``, and ``p->m`` behave as the same forms through a marked pointer
do.  A member function that returns the handle itself -- by reference to its
own class (``++it``), or by value as a handle to the same memory: its own
class, another specialization of its class template, a standard handle of its
kind (a view's iterator), or a class of its value type (``s.subspan(1)``,
``s.begin()``) -- yields a handle in the same state,
and so do ``n + it``, a copy of it, and a local reference to it, while any
other class a member returns by value (a name, a duration) is an ordinary
result.  A call operator's result never refers to its callable object's
memory.  ``s.size()`` uses only the handle object, which is initialized:

.. code-block:: c++

   struct X { int v; X(int x) : v(x) {} };
   struct S { int x; };

   void init(std::span<X> s [[ref_to_uninit]], int i1, int i2) {
     int *q = &s.front().v;           // error: needs [[ref_to_uninit]]
     std::construct_at(&s[2], 10);    // OK
     X y = s[i2];                     // error: reads uninitialized memory
     auto n = s.size();               // OK
   }

   void use(std::unique_ptr<S> p [[ref_to_uninit]]) {
     *p = S{};                        // error: a member call (operator=)
     p->x = 1;                        // error: writes a member
     S *raw [[ref_to_uninit]] = p.get();  // OK
   }

A conditional whose arms are a marked and an unmarked handle is rejected for
either target (§4.9).

A handle is bound like a pointer: a marked handle must refer to
uninitialized memory and an unmarked one to initialized memory, wherever a
handle is initialized -- a variable, a data member, a base subobject, an
aggregate element, a call argument, ``return`` and ``throw``, a ``new``
expression's initializer, and a by-copy lambda capture.  A reference to a
handle cannot carry the marker for what the handle refers to (on a reference
to a class, the marker means the class object itself is uninitialized), so a
handle bound to an unmarked reference -- a parameter, a local, a by-reference
capture -- is judged as an unmarked handle too: ``sinkr(s)`` with
``void sinkr(const std::span<int> &)`` is rejected while a marked ``s`` refers
to uninitialized memory.  A handle's own comparisons and ``+`` and ``-``, and
the source of a copy or assignment (judged where the copy lands), are exempt.
A copy or move of a handle, a conversion
between handles (``std::span<const int> cs = s;``), and a handle a member
function returns refer to what the source refers to.  Raw storage enters a
handle only through the construction of a standard handle -- ``std::span``,
``std::ranges::subrange``, ``std::reverse_iterator``,
``std::move_iterator``, ``std::basic_const_iterator``, the libraries' own
pointer iterators, ``std::unique_ptr``, and ``std::shared_ptr`` -- whose
pointer, array, or range argument decides what the handle refers to (of a
first and a last iterator, the first: the last bounds the range).  That
argument is not judged against the constructor's parameter; the handle is
judged where it lands:

.. code-block:: c++

   void sink(std::span<int> s);

   void f() {
     int arr [[uninit]] [4];
     std::span<int> s(arr);                    // error: needs [[ref_to_uninit]]
     std::span<int> t [[ref_to_uninit]](arr);  // OK
     sink(arr);                                // error: at the argument
     std::unique_ptr<int> p(new int);          // error (§4.7)
     std::unique_ptr<int> q(new int(5));       // OK
   }

The factories ``std::make_unique_for_overwrite``,
``std::make_shared_for_overwrite``, and ``std::allocate_shared_for_overwrite``
return a handle to a default-initialized object, so
``auto p = std::make_unique_for_overwrite<int>();`` needs the marker, while
``std::make_unique_for_overwrite<std::string>()`` does not.  An owning smart
pointer takes its pointer argument, never its deleter or allocator (the
aliasing constructor of ``std::shared_ptr`` takes its pointer argument), and a
null or default-constructed handle refers to nothing, so it suits either
marking.  Storage that is flow-tracked counts at the binding:
after ``memset(arr, 0, sizeof arr)`` an unmarked ``std::span<int> s(arr)`` is
accepted.  Any other class refers to initialized memory unless its
constructor has a parameter its author marked ``[[ref_to_uninit]]``: then
the object refers to what that parameter takes (with
``struct Buf { Buf(int *p [[ref_to_uninit]]); };``, ``Buf b(&u);`` needs the
marker), and the parameter's own binding is judged as usual.  A range-for
over a marked handle binds its elements to uninitialized memory:
``for (int &x [[ref_to_uninit]] : s)`` is accepted, and
``for (int x : s)`` reads uninitialized memory.

Inside a function body, what a marked local handle refers to is flow-tracked
like a marked pointer's referent (see `Binding Pointers and References`_): it
is the storage its initializer names -- another marked handle's referent, or
storage a standard handle was built over whole
(``std::span<int> s [[ref_to_uninit]](arr)`` refers to ``arr``) -- and a
credit through the handle initializes it, so after
``std::uninitialized_fill(s.begin(), s.end(), 0)`` or
``std::construct_at(up.get(), 5)``, ``*s.data()`` and ``*up`` read initialized
memory, and so does an unmarked copy of the handle; a standard handle built
over a marked pointer (``std::span<int>(p, 4)``) reads what ``p`` refers to.
The referent is one object: ``*up``, ``up->m``, ``*it``, ``up.get()``, and
``s.data()`` name it whole, also through a prefix (``s.first(2).data()``),
while ``s[i]``, ``s.front()``, ``s.back()``, any other accessor with arguments,
and an access through a handle past the referent's start
(``s.subspan(1).data()``) name an element, judged by its form as
``p[i]`` through a marked pointer is: a read of it is rejected whatever the
referent's state, and a store to it initializes nothing.  Incrementing or
advancing an iterator keeps its referent but reaches part of it, as a prefix
does, so a fill through either initializes the referent on no path; a reset
or an assignment gives the handle a new referent, a move or ``release()``
hands it over, and
any other non-const member call -- but one returning a pointer or reference
into the referent -- or a non-const reference or pointer to the handle leaves
it unknown, except a standard library function's other than ``std::swap``,
``std::exchange``, and ``std::iter_swap``.  A destructor call on a handle other
than a marked ``std::unique_ptr`` (below) ends the handle object, not its
referent.

A ``std::uninitialized_*`` call, ``std::destroy`` and ``std::destroy_n``, and
their ``std::ranges`` objects take a class-type iterator or range, and a
``[[now_uninit]]`` function's handle parameter is destroyed like its pointer
parameters.  A marked ``std::unique_ptr`` owns its referent: its destructor at
the end of its scope (for a by-value parameter, of the function), its
``reset``, its reassignment, and an explicit destructor call destroy the
referent, so the owned object must have been constructed when its type has a
non-trivial destructor (``destroy_uninit``) and must not have been destroyed
already (``double_destroy``); a ``std::shared_ptr``'s destruction is not
judged.  A null ``std::unique_ptr`` -- constructed empty or from ``nullptr``,
reset, assigned ``nullptr``, released, or moved from -- owns nothing, and its
destruction destroys nothing (after ``if (c) u.reset();`` its destruction is
no ``double_destroy``), while a move or ``release()`` hands what it
owned to the destination.  An owning smart pointer with static or thread
storage duration cannot carry the marker: its destructor runs at program or
thread exit, where nothing is judged.

Assigning a handle (``t = s``) binds it like an initialization, judged
against the assigned-to handle's marking, and so does an owning smart
pointer's ``reset(p)``, whose pointer is judged there: ``u.reset(new int)``
needs a marked ``u``, while ``u.reset(new int(5))`` needs an unmarked one.
Assigning ``nullptr`` and ``reset()`` refer to nothing.  An assignment called
by name (``t.operator=(s)``) is one too, and a copy-and-swap ``operator=``
taking the handle by value is judged once, at the assignment.  A handle
assigned or reset through a reference or a pointer, or as an element
(``r = s`` with ``std::span<int> &r``, ``p->reset(new int)``, ``v[0] = s``),
cannot carry the marker, so it is judged as an unmarked handle.


Constructors
------------

A user-provided constructor must initialize every non-static data member
through its member-initializer list or a default member initializer unless
the member is marked ``[[uninit]]`` (rule ``ctor_uninit_member``, §5.1); an
assignment in the constructor body does not count (reads of an ``[[uninit]]``
member before such an assignment are flow-checked, as above).  Direct
non-virtual base subobjects must be initialized the same way -- a base cannot
carry ``[[uninit]]``, so it must always be initialized, by a written base
initializer or by the base's own user-provided default constructor:

.. code-block:: c++

   struct X {
     int a;
     int b = 0;
     int c [[uninit]];
     X(int v) : a(v) {}  // OK: 'a' is written, 'b' has a default member
   };                    // initializer, 'c' is acknowledged

   struct Y {
     int m;
     Y() {     // error: 'm' is not initialized (ctor_uninit_member)
       m = 1;  //   (a body assignment does not count)
     }
   };

A member whose type's default-initialization leaves unacknowledged scalars
indeterminate (a nested aggregate) is flagged the same way, and the members
of an anonymous struct are checked exactly like direct members (a written
initializer for one is an indirect member-initializer).  An anonymous
*union* member instead needs one active member: a written leaf initializer
or a leaf default member initializer satisfies it -- default member
initializers on the leaves of an anonymous-record variant activate that
variant, which satisfies the union when they initialize it completely --
and an anonymous union of only unnamed bit-fields or only ``std::byte``
members has nothing to initialize.  A delegating constructor is exempt --
its target initializes the members -- and so is a union's own constructor,
whose members are mutually exclusive (§5.6).

An *inherited* constructor (``using B::B;``) initializes only the base it
is inherited from ([class.inhctor.init]); the inheriting class's own
members and its other bases get default member initializers or
default-initialization -- for every inherited signature alike.  So a class
that inherits constructors is checked once, at the ``using``-declaration:
every member must have a default member initializer or an ``[[uninit]]``
marker (rule ``ctor_uninit_member``, the same obligation as for a written
constructor), and every direct non-virtual base other than the nominated
one must not be left indeterminate.  A ``using``-declaration whose every
inherited constructor is deleted imposes no obligation:

.. code-block:: c++

   struct B { B(int); };
   struct D : B {
     using B::B;  // error: inherited constructor does not initialize 'm'
     int m;       //   (give 'm' a default member initializer or [[uninit]])
   };

A default constructor explicitly defaulted *after* its first declaration
(``struct S { int d; S(); }; S::S() = default;``) is user-provided
([class.default.ctor]), so the class is trusted at every use exactly like
one with a written constructor body -- and the constructor itself is
checked at the ``= default`` definition, where it writes no
member-initializers: every member must have a default member initializer
or an ``[[uninit]]`` marker, and every base an initializing default
constructor of its own.  The in-class form (``S() = default;`` in the
class body) takes a different route: it is not user-provided, so the class
is not trusted and each indeterminate use draws ``uninit_decl`` at the
variable instead.  A defaulted *copy* or *move* constructor initializes
every member from its source and is never flagged, and no rule judges the
synthesized body of any defaulted function -- a copy or move constructor or
assignment operator, or a defaulted comparison: only the binding of its
source object is judged, where the function is used.  When such a defaulted
definition is trivial -- it initializes nothing at all -- ``[[uninit]]``
on a variable of the type is accepted once the ``= default`` definition
has been parsed (see :ref:`profiles-init-known-gaps` for the ordering caveat).


Global and Static Variables
---------------------------

Variables with static or thread storage duration are zero-initialized, so
they are never uninitialized: ``[[uninit]]`` on one is rejected -- by
``static_marker`` when nothing else initializes the object, and by
``uninit_with_initializer`` when a written initializer or a non-no-op
default-initialization already contradicts the marker (exactly one of the
pair fires).  ``static_marker`` fires at the declaration that writes the
marker, defining or not: an ``extern`` declaration and an in-class static
data member declaration are rejected themselves, and redeclarations that
merely inherit the marker are not diagnosed again.  Non-local variables with static storage duration must
additionally be initialized at compile time (rule ``static_runtime_init``,
§3), because cross-translation-unit initialization order can otherwise
produce a read of a not-yet-initialized object:

.. code-block:: c++

   int seed();

   int g1 = 42;                  // OK: constant-initialized
   int g2 = seed();              // error: runtime initializer (static_runtime_init)
   thread_local int t = seed();  // OK: thread storage duration

   int &counter() {
     static int c = seed();  // OK: a function-local static is initialized
     return c;               // on first use (§3)
   }


Templates
---------

In a template, the declaration rules, the constructor rules, and the
flow-based rules fire per instantiation; ``uninit_read`` (through a
marker), ``uninit_write``, and the binding rule fire at the definition
when their operands are not flow-tracked and do not depend on the
template's parameters, and again at each instantiation that rebuilds the
expression.  A binding, read, write, or destroy of flow-tracked storage is
judged at instantiation only, whatever its operands depend on; a
never-instantiated template's are not diagnosed.


Limitations
-----------

The implemented slice rejects some correct code that it cannot prove safe
(``[[profiles::suppress]]`` is the escape hatch there) and accepts some
flows it does not check; both lists are in :ref:`profiles-init-known-gaps`.


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
