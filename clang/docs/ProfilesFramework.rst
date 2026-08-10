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
violation.

Profile names are open-ended: standard (``std::``-prefixed),
implementation-defined, and third-party profiles are all requested with the
same syntax, and enforcing a profile the implementation does not know is not
an error -- it simply has no rules to enforce.  Clang currently implements
two real profiles: an initial slice of the proposed ``std::init``
initialization profile (see `The std::init Profile`_), whose rules are
checked at compile time, and an initial slice of the proposed
``std::core_ub`` undefined-behavior profile (see `The std::core_ub
Profile`_), whose rules are checked at run time.  The feature is
experimental: attribute spellings, rule names, and diagnostics may
change.


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

   [[profiles::enforce(std::init)]];
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
   [[profiles::enforce(std::init)]];  // error: does not precede 'x'


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

A suppression covers exactly the tokens of the declaration or statement it
appertains to -- nothing more.  The declarator-id position works too: on a
function definition it covers the whole definition, mem-initializers and
body included, ending with it --

.. code-block:: c++

   void legacy_code [[profiles::suppress(std::init)]] () { /* exempt */ }

-- for free functions, inline members, and out-of-line definitions alike
(the attribute must be on the *definition*; one written on a previous
declaration does not carry over).  For a variable declaration the covered
tokens include the
initializer, so violations inside the initializer are silenced; but the
variable is *not* marked as exempt at later uses, which appear in other
declarations or statements and are checked normally:

.. code-block:: c++

   [[profiles::suppress(std::init)]] int x;  // OK: uninit_decl suppressed
   int y = x;  // error: 'x' is read before initialization

To exempt an object from a profile's checks everywhere it is used, use the
profile's own per-object marker instead (for ``std::init``, ``[[uninit]]``).


Profiles and Modules
====================

A module interface advertises the profiles it enforces through
``[[profiles::enforce]]`` on its module-declaration, and importers can insist
on that advertisement with
``[[profiles::require(profile-designator-list)]]``, which may appear only on
a module-import-declaration.  Every listed profile must be advertised; each
one that is not is diagnosed individually:

.. code-block:: c++

   // M.cppm
   export module M [[profiles::enforce(std::init, vendor::hardened(fortify: 3))]];

   // user.cpp
   import M [[profiles::require(std::init)]];  // OK: M enforces std::init
   import M [[profiles::require(std::init, vendor::hardened(fortify: 3))]];  // OK
   import N [[profiles::require(std::init)]];  // error unless N does too

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
  ``[[ref_to_uninit]]`` attribute, and reads through it are rejected.
- Non-local objects must be initialized at compile time (§3).
- Implicit initialization counts: a default constructor or the
  zero-initialization of statics initializes an object, and a class with a
  user-provided constructor is trusted to initialize its members (§5.1).

``[[uninit]]`` and ``[[ref_to_uninit]]`` are ordinary C++11 attributes,
recognized regardless of ``-fprofiles`` (see the :doc:`AttributeReference`
entries for their placement rules); the rules below carry weight only while
``std::init`` is enforced.  Reads and writes of ``std::byte`` objects are
exempt from all of them (§4.5).

**Marker placement.**  The marker family -- ``[[uninit]]``,
``[[ref_to_uninit]]``, ``[[now_init]]``, and ``[[now_uninit]]`` --
appertains to *declarations*, in either of the two standard positions: the
prefix decl-specifier position (``[[now_uninit]] void destroy_at(T* p);``)
or the declarator-id position (``int u [[uninit]];``,
``void destroy_at [[now_uninit]] (T* p);``).  The *type* positions some of
the paper's examples use -- after a ``*`` or ``&``, after an array bound,
or after a parameter list -- are rejected: neither
``int arr[10] [[uninit]];`` nor ``void destroy_at(T* p) [[now_uninit]];``
compiles.  The markers' exact placement and spelling track an open
committee question (P4222R2 §4.7).

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
``double_destroy``          A ``[[now_uninit]]`` call on storage already
                            destroyed.
``destroy_uninit``          A ``[[now_uninit]]`` call on storage that is
                            (still or again) uninitialized.
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
unions is rejected exactly like a single one.  A no-op initialization -- the
trivial default-initialization of a scalar or aggregate that leaves a scalar
subobject indeterminate -- is consistent with the marker; any other
synthesized initialization contradicts it and is rejected (§5.3): a member or
base with a user-provided default constructor, a default member initializer,
a virtual table pointer, or a value-initialization (``= P()``), all of which
initialize something.  ``uninit_with_initializer`` reports the two cases
differently: a *written* initializer contradicts the marker outright, while a
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
constructor-less aggregate locals; reads through ``[[ref_to_uninit]]``
pointers and references and reads of subobjects of ``[[uninit]]`` objects are
rejected outright -- unless a prior whole-entity store credits the storage
as initialized (see `Binding Pointers and References`_):

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

A member or variable counts as assigned only when every path to the read
assigns it (§1.3), and a compound assignment (``x += 1``) or an increment or
decrement reads the old value first, so it is diagnosed like a read.  Inside
a constructor body only that plain whole-member assignment earns credit:
passing ``&m`` to a function (even one whose parameter is marked
``[[ref_to_uninit]]``), binding a reference to the member, calling a member
function, or letting ``this`` escape does not count as initializing ``m`` --
the paper rejects complex constructor code (§5.1) and reserves
callee-initialization for ``now_init()`` (§6.2); suppress the rule where
such a flow is intended.  A ``this``-capturing lambda might run immediately,
so member reads in its body count at the point the lambda is created (and
writes there earn no credit -- the same strict policy).  For a *local*
variable the analyses instead treat any escape as an assignment (see
`Limitations`_).  A local copied or moved from a tracked local is tracked
too: the copy's members inherit the source's per-member state at the copy
point -- a copy does not inherit initialization (§5.2), it inherits
whatever state the source has.  A by-value *parameter* of such a class is
likewise a copy, of the caller's argument, so its marked members are
tracked from an unassigned start: a read before a local assignment (or an
escape of the parameter) is rejected even if the caller assigned the member
first.  That is the call-boundary twin of the constructor-body strictness
-- the paper hands uninitialized-capable storage across calls through
marked pointers and references (§4.3), not by-value slots -- and
``[[profiles::suppress]]`` is the remedy where the by-value flow is
intended.  Members of an object initialized by a *user-provided*
constructor are trusted (§5.1) and not flow-tracked; only the defining
constructor itself is checked.  A *union's* own constructor is not
flow-analyzed at all: its members are mutually exclusive, so whether the
active member is set is deferred (§5.6), matching ``ctor_uninit_member``'s
union exemption.


Writes to Subobjects of Uninitialized Objects
---------------------------------------------

Piecemeal delayed initialization of an ``[[uninit]]`` object through its
members or elements cannot be validated statically (§5.4, §5.5), so a store
to a *proper subobject* of an ``[[uninit]]`` entity is rejected (rule
``uninit_write``); writing the whole entity is that entity's initialization,
stays legal, and credits the entity as initialized for everything after it
in parse order (see `Binding Pointers and References`_):

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

Subobject writes below the marker are rejected like the named twin: a
write below a member step initializes nothing (§5.4's piecemeal ban; §4.5
makes only the whole scalar write an initialization), whether the storage
is reached through a marked pointer, reference, member, or callee return,
through a recognized allocator's raw result, or through a raw ``new``
expression.  A class pointee's initialization is ``construct_at`` (its
``[[ref_to_uninit]]`` parameter accepts the pointer) or, for a scalar
pointee, the whole ``*p`` store.

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
refers to uninitialized memory is recognized from its form -- the address or
a subobject of an ``[[uninit]]`` entity, the value or dereference of a
marked pointer or reference, pointer and reference casts of those, a call to
a marked function, a call to a known allocator (``malloc``,
``aligned_alloc``, ``alloca``, and raw ``::operator new`` calls return
uninitialized memory, ``calloc`` zero-initialized memory, and ``realloc`` is
unclassified; §4.3 -- trusted recognition keys on Clang's builtin IDs, and
where those are absent, under ``-fno-builtin`` or ``-ffreestanding``, a
name-recognized allocator is *untrusted*: unclassified, so neither binding
direction diagnoses -- never trusted as initialized like an arbitrary
callee; the ``operator new``/``operator delete`` families are recognized by
form and keep their recognition everywhere), and a ``new``
expression that default-initializes a type with indeterminate scalars
(``new int``, ``new int[n]``; §1.2) -- refined by one parse-order fact,
whole-entity stores (below).  A named *function's* decayed pointer value is
none of these: its ``[[ref_to_uninit]]`` describes the return value, so
binding the function itself to a function pointer is accepted while a call
to it stays a marked source:

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
and variadic ones), ``return`` and ``throw`` statements (including returns
inside lambdas and blocks -- a lambda's marker is written on its call
operator, in the C++23 attribute position after the lambda-introducer:
``[] [[ref_to_uninit]] () -> int* { ... }``), lambda captures (by reference
and by copy -- a closure field cannot carry the marker, so a by-copy capture
of a marked pointer is rejected like any unmarked pointer copy; capture by
reference, or store through the marker first, if the flow is intended), and
the implicit object argument of a member call -- so calling a member function
on an object recognized as uninitialized storage is rejected, and so is
copying a class object out of one.  For the copy, the escape is the paper's
own (§7.2): declare the copy constructor's parameter ``[[ref_to_uninit]]``.
Positions that cannot carry the marker -- a variadic argument, a parameter of
a function called through a function pointer, the implicit object parameter,
a pointer element of an array in aggregate initialization
-- are checked as unmarked targets; suppress at the call site if the flow is
intended.  A parameter's marker written on any declaration of the function is
inherited by the parameter's later redeclarations, so a header's marker
carries to the source file's definition (the §7.2 header/source split); the
definition keeps its read-through checking and call sites after the
redeclaration still see the marker.  A null pointer source -- ``nullptr``, ``0``, ``{}``, or a local
variable initialized to null -- refers to no object, so it is accepted for
marked and unmarked targets alike (§4.3, §8: the marker means "zero or more
uninitialized objects"); a *parameter* with a null default argument is not a
null source (callers may pass any pointer).  A source whose form the recognizer cannot classify
(pointer arithmetic, an integer-to-pointer cast) is likewise accepted for
either target.

The parse-order refinement: a *whole-entity store* credits its target as
initialized (§4.2, §4.5).  After ``u = 5;`` the ``[[uninit]]`` variable
``u`` counts as initialized, and after ``*p = 5;`` the marked pointer's
pointee does, for every later ``*p`` access -- until ``p`` is reseated
(``p = q``, ``p += n``, ``p++``), which withdraws the credit; a store
through a marked *reference* credits its referent permanently.  A store
through a transparent cast credits (and reseats) like its uncast form --
``(int &)u = 5`` credits ``u`` whole, ``*(int *)p = 5`` the pointee,
``(int *&)p = q`` reseats ``p`` -- exactly the transparency the
recognizers already grant casts (§4.3), narrowing included: ``(char &)u =
'x'`` credits ``u`` whole.  Handing out a *mutable alias* of the marked
pointer object -- binding ``p`` to a ``T*&``, or ``&p`` to a ``T**``,
whether as a call argument, an initializer, or an assignment source --
lets the holder reseat it, so the escape withdraws the credit's firing
strength while the suppressing credit survives, like a conditional
``[[now_uninit]]`` destroy; a const alias (``T* const &``) cannot reseat
and withdraws nothing.  (A ``void*`` escape of ``&p``, or an alias source
wrapped in a ternary or comma, is not recognized -- missed withdrawals,
toward missed diagnostics only.)  The two
directions consult the credit differently, because they use it
differently.  Credit *suppresses* the unmarked-target diagnostic in plain
parse order: ``int *q = &u;`` after any earlier store -- even one under a
condition -- is accepted, since the storage may well be initialized (the
untaken path is a missed diagnostic, see `Limitations`_).  Credit *fires*
the marked-target diagnostic only when the store is unconditional in the
entity's own function: after a top-level ``u = 5;`` a later
``int *r [[ref_to_uninit]] = &u;`` is rejected -- a definitely initialized
entity requires an unmarked target (§4.2) -- while a store under an
``if``, a loop, a ``switch``, a ``try``, ``&&``/``||``/``?:``, or inside a
lambda or block body may not have executed on the path that reaches the
binding, so the marked binding stays legal -- the mechanism errs toward
missed diagnostics (see `Limitations`_).  (The
conditionality test is syntactic and errs the same way: a store in a
condition itself, in a ``do`` body, or in the taken branch of
``if constexpr`` conservatively counts as conditional, and a store after a
``goto`` -- which could jump over it without introducing any scope -- or
after a ``switch`` (the shared branch flag) earns the suppressing credit
only.)  Element accesses
(``p[i]``) are never credited in either direction (§5.4's random-access
ban applies even to ``p[0]``), element stores earn no credit, and address
escapes (passing ``&u`` to a ``[[ref_to_uninit]]`` parameter) never count
-- the paper reserves callee-initialization for ``now_init()`` (§6.2).
Class-typed whole-object assignment never credits either: it is a member
``operator=`` call on uninitialized storage, rejected as above.

Whole-member stores are credited too, keyed per base object: after
``a.m = 5;`` on a directly named local, or ``this->m = 5;`` on the current
object (keyed to the enclosing function body, so no other function -- nor a
lambda body, which is its own function -- shares it; instantiations of one
template or generic lambda share their pattern's single body, and its
credit), binding ``a.m`` or
``&a.m`` through the *same* base is accepted, and the reverse direction
applies as for locals.  The boundaries: one member level only (``x.agg.m =
5;`` is itself the piecemeal-initialization error and earns nothing), only
directly named non-reference locals or the current object (a member reached
through a pointer, reference, or any other object -- including a copy, per
§5.2 -- stays strict), and never member *pointee* stores (``*w.p = 5;``),
whose aliasing is per-value: a copy of the object shares the pointee.  The
credit is purely parse-order, with no dominance or flow analysis: a store
under a condition still credits -- and so suppresses -- everything after
it, so a binding on the untaken path is a missed diagnostic (see
`Limitations`_); only the requires-uninitialized direction insists on an
unconditional store, exactly as for locals.

One kind of call *does* count as initialization: §6.2's ``[[now_init]]``
attribute (its placement and spelling track an open committee question)
declares that a function initializes the storage passed to each of its
``[[ref_to_uninit]]`` parameters, and it requires at least one such
parameter.  After a call to a ``[[now_init]]`` function, the argument's
storage earns exactly the credit the equivalent direct store would --
``fill(&u)`` credits ``u`` whole, ``fill(p)`` credits the marked pointer's
pointee (until ``p`` is reseated), ``fill(&a.m)`` credits the ``(a, m)``
pair, and ``fill(arr)`` credits a local ``[[uninit]]`` array whole (§6's
``uninitialized_fill`` shape; the element form ``fill(&arr[0])`` earns
nothing, §5.4) -- with the same boundaries and the same reverse-direction
consequence
(after an *unconditional* ``fill(&u)`` in the entity's own function, a
second ``fill(&u)`` is rejected: ``u`` no longer refers to uninitialized
memory, which incidentally catches double ``construct_at``).  This is R2
§4.5's requested library annotation: declare ``construct_at`` with
``[[now_init]]`` and a ``[[ref_to_uninit]]`` first parameter and the
lifecycle *start* is checked.  The §4.4 ``now_init()`` identity function
needs no compiler support at all -- declared as ``template<class T> T*
now_init(T* p [[ref_to_uninit]]);``, its unmarked return is already trusted
as initialized -- but only the attribute legalizes the original *name* after
the call.

The lifecycle *end* is the mirror attribute ``[[now_uninit]]`` -- the
recording §4.4 notes is missing for ``destroy_at`` ("the object subjected to
``destroy_at()`` should be considered uninitialized, but there is no way of
recording that in the code").  It declares that a function ends the lifetime
of the storage passed to each of its pointer or reference parameters (which
are unmarked; apply the attribute only to functions that destroy *every*
such argument's storage), and a call to one *withdraws* exactly the credit
the equivalent ``[[now_init]]`` call would have recorded.  A *conditional*
call -- under an ``if``, a loop, or any of the constructs listed above --
may or may not have destroyed the storage, so it withdraws only the
credit's firing strength (a following marked binding is no longer forced,
while an unmarked binding stays accepted) and records no destroyed state.
The parameters themselves take storage that is initialized, or at least
credited by an earlier store: destruction makes an object uninitialized
(§1), so destroying storage that is still -- or again -- uninitialized is
itself an access to raw memory, rejected as rule ``destroy_uninit``
(§4.4: it is an error to uninitialize an object twice).  The rejection
fires only on affirmatively uninitialized storage (a conditional store
suppresses, and unknown storage is accepted), a storage-release callee
keeps any-state acceptance (``free`` takes storage that may never have
been constructed; see `Limitations`_), a reinitializer (below) is
exempt, and so is any parameter that itself carries
``[[ref_to_uninit]]``: the marker declares that the parameter accepts
uninitialized storage -- the annotation spelling for an unrecognized
release function.  Never accepted is storage a
``[[now_uninit]]`` call already destroyed.  That is a double destruction
(rule ``double_destroy``), definite by construction: only an unconditional
same-function destroy records the destroyed state, and any store or
``[[now_init]]`` call retires it.  Declare ``destroy_at`` as
``template<class T> [[now_uninit]] void destroy_at(T* p);`` and the
construct/destroy/construct cycle
is legal, a second destruction is rejected, a destroy of never-constructed
storage is rejected too, and binding the destroyed
storage to an ordinary pointer or reference is rejected as the
unmarked-direction violation.  A function may
carry both attributes -- a reinitializer that destroys and then
reconstructs its argument's storage -- and models destroy-then-construct:
the storage bound to its ``[[ref_to_uninit]]`` parameters is initialized
after the call, and calling it on already-initialized storage is exactly
its purpose, so it is accepted.

The standard storage-release callees are recognized without annotation and
treated like ``[[now_uninit]]`` at the call: ``free``, ``realloc`` (its
pointer argument), and replaceable global ``::operator delete`` /
``::operator delete[]`` (sized and nothrow forms included; class-specific
and destroying overloads excluded) accept a pointer in any state -- an
RAII buffer's ``free(p)`` on a ``[[ref_to_uninit]]`` member is legal
whether or not the buffer was ever written -- and withdraw the storage's
credit, so a read through a marked pointer after ``free(p)`` is diagnosed
again.  A release records no *destroyed* state, though: releasing storage
ends no object's lifetime, so both orders relative to a ``[[now_uninit]]``
call sit outside ``double_destroy``'s scope -- ``destroy_at(p); free(p);``
is correct (different operations, correctly ordered).  The reverse order,
``free(p); destroy_at(p);`` on a *marked* pointer, is the
``destroy_uninit`` violation: the release withdrew the pointee's credit,
so the storage is uninitialized again when the destroy takes it.  Through
an unmarked pointer the release changes no classification -- post-release
use stays the invalidation profile's concern -- and only a second
``[[now_uninit]]`` destroy of the same storage fires ``double_destroy``.  The ``delete`` and ``delete[]`` *expressions*
perform the same credit withdrawal, likewise recording no destroyed state
-- with no diagnostic on the operand, matching their historical silence --
so ``delete q;`` and ``::operator delete(q);`` agree on everything that
follows.  Like the allocator side, trusted ``free``/``realloc`` recognition
keys on Clang's builtin IDs.  Where those are absent (``-fno-builtin``,
``-ffreestanding``) the binding acceptance survives by name -- accepting a
pointer never diagnoses, and a declared ``free`` should not reject its
argument just because the ID is gone -- but the credit withdrawal does not:
it is a diagnostic's firing basis and needs the trusted recognition, so a
post-release read stays accepted there (a missed diagnostic, never a false
positive).


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
every member from its source and is never flagged.  When such a defaulted
definition is trivial -- it initializes nothing at all -- ``[[uninit]]``
on a variable of the type is accepted once the ``= default`` definition
has been parsed (see `Limitations`_ for the ordering caveat).


Global and Static Variables
---------------------------

Variables with static or thread storage duration are zero-initialized, so
they are never uninitialized: ``[[uninit]]`` on one is rejected -- by
``static_marker`` when nothing else initializes the object, and by
``uninit_with_initializer`` when a written initializer or a non-no-op
default-initialization already contradicts the marker (exactly one of the
pair fires).  Non-local variables with static storage duration must
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


Limitations
-----------

The implemented slice is conservative in both directions.  The first group
below lists *deliberate strictnesses*: places where the analysis rejects
what it cannot prove safe, even though the code may be correct --
``[[profiles::suppress]]`` is the escape hatch there.  The second group
lists *missed diagnostics*: accepted flows the analysis does not see;
those entries never cause a rejection.

**Deliberate strictnesses (may reject correct code):**

- Inside a constructor body, only a plain assignment to an ``[[uninit]]``
  member (including through a transparent reference cast such as
  ``(int &)m = 1`` -- the same casts the parse-order credit sees through)
  or a call to a ``[[now_init]]`` function (§6.2) counts as its
  initialization -- the latter for the current-object storage bound to the
  callee's ``[[ref_to_uninit]]`` parameters (``&m``, ``m``, or ``this``
  itself, which credits every tracked member), as a genuine dataflow fact,
  so a ``[[now_init]]`` call on one branch still does not satisfy a read at
  the join (§1.2).  A ``[[now_uninit]]`` call is the credit's destruction
  twin: a real kill bit for the same storage shapes, so a destroy on one
  branch already spoils a read at the join, even though the destroy may
  not have run.  Taking the member's address, binding a reference to it,
  calling a member function, letting ``this`` escape, or passing ``&m`` to a
  ``[[ref_to_uninit]]`` parameter of an *ordinary* function earns no credit,
  so a later read of the member is rejected: the paper rejects complex
  constructor code (§5.1) and reserves callee-initialization for
  ``now_init`` (§6.2); the remedy for an intended flow is ``[[now_init]]``
  on the callee or ``[[profiles::suppress]]``.  For *locals*, by contrast,
  the local-aggregate pass and the plain-local analysis conservatively treat
  any escape of the variable as an assignment (member accesses -- including
  through anonymous aggregates -- are not escapes) -- there the omission is
  a missed diagnostic, never a false positive.  That escape-crediting is an
  interim leniency relative to the paper (which credits only ``now_init``);
  tightening it to ``[[now_init]]`` callees alone is future work.
- A by-value *parameter* of a tracked class is a copy of the caller's
  argument, so its marked members are tracked from an unassigned start: a
  read before a local assignment (or an escape of the parameter) is
  rejected even if the caller assigned the member first (see `Reads of
  Uninitialized Objects`_) -- unless the parameter declares a default
  argument: a defaulted call initializes the parameter object directly
  (guaranteed elision -- no copy), so such parameters are not tracked.  A
  *const* by-value parameter cannot be assigned locally at all, so no code
  change can satisfy the analysis -- suppression is the only remedy there.
- A ``this``-capturing lambda might run immediately, so member reads in
  its body count at the point the lambda is created, and writes there earn
  no credit (see `Reads of Uninitialized Objects`_): a lambda that is only
  ever run later, after the members are assigned, is rejected the same
  way.  A ``*this`` capture copy-constructs the whole object at the
  lambda's creation, reading every member right there: it is rejected
  before every tracked member is assigned -- unless the class has a
  *user-provided* copy constructor, which is opaque and trusted (§5.1) --
  and its body accesses go to the copy, not the original.
- The flow-based read passes promote every use a path *may* reach
  uninitialized to the profile error, path-insensitively: with two
  correlated conditions (``if (c) x = 1; ... if (c) use(x);``) the read is
  rejected although no executable path reads uninitialized memory --
  the shape ``-Wuninitialized`` reports as only "may be uninitialized"
  is a hard error under the profile.  Destroys over-flag the same way:
  with correlated conditions (``if (c) destroy_at(&m); ... if (!c)
  use(m);``) the member read is rejected although no executable path
  reads destroyed memory.
- Destroying through a ``[[ref_to_uninit]]`` pointer that was never stored
  through is rejected by ``destroy_uninit``: the marker asserts an
  uninitialized pointee at entry.  If a helper filled the pointee first,
  mark the helper ``[[now_init]]``, store through the marker before the
  destroy, or suppress.
- Passing ``&u`` to a ``[[ref_to_uninit]]`` parameter of an *ordinary*
  function earns no credit (see the constructor-body bullet above), so a
  later ``destroy_at(&u)`` is rejected by ``destroy_uninit`` even if the
  callee filled the storage -- the same strictness, and the same remedies,
  as the unmarked-direction binding after such a call: ``[[now_init]]`` on
  the callee or suppression.
- Member-wise initialization of a class pointee through
  ``[[ref_to_uninit]]`` is rejected even for trivially-copyable pointees
  (``ptr->x = 5``): below a member step only whole-object ``construct_at``
  could initialize, which is unmodeled.  The remedies are ``construct_at``,
  a whole scalar write for a scalar pointee, or suppression.  Two corners
  are suppress-only: a marked pointer *member*'s pointee is never
  credited, and an element access (``ptr[i].x``) skips the store-credit
  consult, so no prior fill legalizes either.

**Missed diagnostics (never rejections):**

- ``construct_at``/``destroy_at`` flow is modeled through the annotations:
  a ``[[now_init]]``-annotated ``construct_at`` declaration checks the
  lifecycle start (including double construction, via the reverse-direction
  binding rule) and a ``[[now_uninit]]``-annotated ``destroy_at`` the end
  (double destruction, use-after-destroy where the destroyed storage is
  *bound*, and -- in both member dataflow passes -- a member read after the
  destroy, which kills the member's assigned bit.  A direct named read of a
  plain ``[[uninit]]`` *local* after a destroy stays with the flow-based
  local-variable analysis, which treats the call as an escape in every
  mode, so it is a missed diagnostic).
- The raw storage-release callees (``free``, ``realloc``, replaceable
  global ``operator delete``) accept storage that was never constructed:
  that is ``free``'s contract, not a gap -- only a ``[[now_uninit]]``
  destroy proper is rejected on uninitialized storage (rule
  ``destroy_uninit``).  A destroy of *unknown* storage stays accepted (the
  rule fires only on an affirmative classification), and so does a destroy
  after a merely *conditional* store: the ``Maybe`` credit suppresses -- a
  missed diagnostic relative to the paper's "for acceptance all
  alternatives must provide the desired solution" ("Guarantees",
  p4222r2.md:1982-1985), the same conservatism the parse-order credit uses
  elsewhere.  The asymmetry with the member dataflow is deliberate: the
  dataflow joins pessimistically (a destroy on any considered-executed
  branch clears the assigned bit, "Static analysis", p4222r2.md:306-312),
  while parse-order credit is optimistic (a conditional store suppresses);
  the first governs reads, the second call-site acceptance, and both err
  away from false positives.  A reinitializer's exemption from ``destroy_uninit`` is
  call-wide, so its *unmarked* destroy-only pointer parameters -- whose
  storage the paper requires live -- are exempt too; a parameter that
  itself carries ``[[ref_to_uninit]]`` is exempt per parameter (the
  release-workaround spelling below).
- ``__builtin_operator_delete`` binds its operand with no parameter
  declaration in sight, so the storage-release relaxation cannot recognize
  it: passing ``[[ref_to_uninit]]`` storage keeps the unmarked-direction
  error.  ``_aligned_free`` and ``reallocarray`` carry no Clang builtin ID
  and are likewise unrecognized; declaring such a function
  ``[[now_uninit]]``, with its pointer parameter marked
  ``[[ref_to_uninit]]``, is the workaround -- the parameter marker keeps
  ``destroy_uninit`` from rejecting the release of a buffer that was
  never written (a release function's contract, unlike a destroy's).
- An element write through the marker (``p[3] = 0``) is accepted and never
  credited -- a gap against the paper's random-access ban ("Static
  analysis", p4222r2.md:314-316; "Guarantees", p4222r2.md:1987-1989); no
  read is legalized by it.  The asymmetry with ``[[uninit]] int a[2];
  a[0] = 1;`` -- rejected under the same paper rule -- follows the paper's
  own span example ("Lifetimes", p4222r2.md:947-961), which accepts the
  constant-index element initialization and flags only reliance on the
  partial state.  The strict alternative -- clearing the marker trust in
  the subscript arm too -- would remove the asymmetry at the cost of
  turning every typed raw-buffer fill loop through a marked pointer into a
  hard error, with range algorithms, ``construct_at``, or suppression as
  the only remedies (``std::byte`` buffers excepted, p4222r2.md:929-931
  and :1166-1167).
- A ``new`` expression whose result is not bound to anything (``new int;``)
  is not checked.
- A call through a function pointer cannot see parameter markers on the
  pointed-to function, and a member call through a pointer-to-member bypasses
  the object-argument check.
- Members of anonymous structs and unions, and arrays of aggregates, are not
  flow-tracked.
- ``[[uninit]]`` members of locals copied from untracked sources (a call
  result, a member, an element) are not flow-tracked: a copy does not
  inherit initialization (§5.2) -- it copies indeterminate bits -- but the
  source's per-member state is unknown, so reads of such members are
  trusted (a missed diagnostic, never a false positive).  Copies of tracked
  locals and by-value parameters *are* tracked (see `Reads of Uninitialized
  Objects`_).
- A by-value parameter with a default argument is not tracked (see the
  strictness bullet above), so an *explicit* call passing an uninitialized
  copy into such a parameter is not detected -- reads of its marked members
  in the callee are trusted.
- An *alias* of ``this`` in a constructor body -- a stored
  ``T *self = this;``, an init-capture ``[p = this]``, ``&*this`` -- is
  invisible to the constructor-body pass: reads through the alias are not
  attributed to the members, and writes through it earn no credit (that
  half is a strictness, never a false positive).
- Virtual base subobjects are not checked by ``ctor_uninit_member`` (they are
  initialized by the most-derived class).
- A read of a tracked member inside another member's default initializer is
  not detected.
- ``[[uninit]]`` on a type whose default constructor is explicitly defaulted
  *after* its first declaration is accepted only once the ``= default``
  definition has been parsed: a marker written between the class definition
  and the constructor's still sees a declared-but-undefined constructor and
  is rejected as running a constructor.
- Whole-entity store credit is parse-order only: a store under a condition
  (or inside a lambda body) credits every later use in parse order, so a
  read or binding on a path that skips the store is a missed diagnostic.
  ``[[now_init]]`` call credit outside constructor bodies is parse-order in
  the same way (inside them it is a real dataflow fact, as above).  The
  requires-uninitialized direction is stricter about what it *fires* on:
  only a store (or ``[[now_init]]`` call) unconditionally executed in the
  entity's own function forces a later binding onto an unmarked target, so
  a conditional store leaves both binding directions legal -- more missed
  diagnostics, still no false positive.  The conditionality test is
  syntactic and conservative in the same direction: a store in an
  ``if``/``while``/``for`` condition or a ``for`` init-statement, in a
  ``do`` body, or in the taken branch of ``if constexpr`` counts as
  conditional, and once a function has branched -- ``goto``, indirect or
  ``asm goto``, or a ``switch``, which shares the tracking flag -- every
  later store counts as conditional too (a goto earlier in the body could
  skip the store without introducing any scope; the switch inclusion is a
  further over-approximation).  ``[[now_uninit]]`` withdrawal mirrors the
  split: a
  conditional destroy revokes only the firing strength and leaves the
  suppressing credit in place.  The requires-uninitialized direction also
  consults credit at definition time only -- an instantiation re-walk does
  not rewind parse-order state, so re-checked statements must not trip over
  credit they themselves recorded -- which makes a reverse-direction
  violation established only by credit in fully dependent code a missed
  diagnostic as well.
- In a template, a violation in non-dependent code is diagnosed at definition
  time and may be repeated at instantiation.


Runtime-Checked Rules
=====================

P3589R2 anticipates profiles with *dynamic semantics*: a profile "may have
an effect on the runtime behavior of a program", such as enabling runtime
instrumentation like bound checking (§1.1, §2.2.2).  The framework supports
such rules: enforcing the profile makes the compiler emit a runtime check
into the generated code, and a failed check *traps* -- deterministically
stopping the program (typically ``SIGILL``) before the guarded operation
executes.  ``std::core_ub`` (see `The std::core_ub Profile`_) is the real
profile built from such rules; the in-tree pilot rule (``test::arith`` /
``zero_divide``, which traps on integer division or remainder by a runtime
zero) additionally exists to exercise the machinery in the test suite and
is inert outside it.

Behavior of a runtime check:

- **Trap-only.**  Enforcement is declared in source, so the driver cannot
  know at link time that a support runtime would be needed: there is no
  handler library, no runtime error message, and no way to continue past a
  failed check.  When debug info is enabled (and under the default
  ``-fsanitize-debug-trap-reasons=detailed``), the trap's debug location
  carries a message naming the violated profile, which debuggers display.
  At ``-O0`` every check site gets its own trap instruction with an exact
  source location; optimized builds merge a function's profile trap blocks,
  like UBSan's trap mode.
- **Suppression applies identically.**  ``[[profiles::suppress]]`` on the
  statement, the declaration, or an enclosing declaration removes the
  runtime check, with the same token-dominion rule as compile-time
  diagnostics: a suppression around a use site does not silence checks in a
  default member initializer or default argument emitted there -- those
  belong to the member's or parameter's construct, so suppress on the
  member or parameter instead.  Two known over-checking gaps (a check that
  suppression fails to remove, never a missing check): suppression from an
  enclosing *statement* does not reach the member functions of a local
  class defined inside it, nor the body of an Objective-C block (a lambda
  body is covered).
- **System-header exemption.**  As for compile-time rules, code originating
  in a system header gets no checks by default;
  ``-fno-profiles-exempt-system-headers`` restores them.
- **Sanitizer independence.**  The checks are emitted regardless of
  sanitizer configuration.  Enabling a sanitizer that guards the same
  operation (e.g. ``-fsanitize=integer-divide-by-zero`` alongside the pilot
  rule) instruments the operation twice -- redundant, never wrong -- and no
  sanitizer facility (ignorelists, ``__attribute__((no_sanitize))``,
  ``-fsanitize-skip-hot-cutoff``) can disable a profile's check, which is a
  language guarantee rather than opt-in instrumentation.
- **Per-TU enforcement.**  A check is emitted exactly when the translation
  unit *emitting the code* enforces the profile.  An inline function
  defined in a textual header is therefore compiled with checks in
  enforcing TUs and without them elsewhere, and the linker keeps one copy
  arbitrarily -- the situation long accepted for mixing sanitized and
  unsanitized TUs.  Code imported from a named module currently follows the
  same rule: it is checked only if the *importing* TU enforces the profile,
  even when the module interface itself did.

The zero-divisor checks (``std::core_ub``'s and the pilot's) deliberately
mirror UBSan's blind spots: GCC vector-extension integer division,
``_Complex int`` division, and (in C only) compound assignment of a scalar
by a ``_Complex`` operand are not checked.


The ``std::core_ub`` Profile
============================

``std::core_ub`` is an initial slice of the proposed profile from Vinnie
Falco's "A Profile for Runtime-Checkable Core-Language Undefined Behavior:
std::core_ub" (P4317; the case identifiers below, in braces, are from its
Appendix A).  Its guarantee: **a checkable core-language operation whose
precondition is violated traps rather than proceeding into undefined
behavior**.  Unlike ``std::init``, it acts at run time: every rule is a
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

Clang also ships five ``test::`` profiles (``test::type_cast``,
``test::uninit_read``, ``test::class_final``, ``test::ctor_final``, and
``test::arith``) that exist only to exercise the framework in the test
suite.  They are inert without an additional ``-cc1``-only flag; see
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
