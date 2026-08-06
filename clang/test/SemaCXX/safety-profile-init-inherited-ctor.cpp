// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 %s

// std::init / ctor_uninit_member for inheriting constructors
// ([class.inhctor.init]): an inherited constructor initializes only the
// nominated base; the inheriting class's own members and its other bases get
// NSDMI-or-default-initialization -- invariantly, for every inherited
// signature. Checked once per class at finalization, at the
// using-declaration.

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

struct B { B(int); };

struct D : B {
  using B::B; // expected-error {{constructor inherited from 'B' does not initialize member 'm' under profile 'std::init'}}
  int m;      // expected-note {{member 'm' declared here}}
};
void use_d() { D d(1); (void)d; }

// A default member initializer or an [[uninit]] marker satisfies the
// obligation, exactly as for a written constructor.
struct DNsdmi : B {
  using B::B;
  int m = 0;
};
struct DMarked : B {
  using B::B;
  int m [[uninit]];
};

// Anonymous-union member flavors: an uninitialized anonymous union fires the
// union flavor; a leaf NSDMI activates that variant and satisfies it.
struct DAnonUnion : B {
  using B::B;               // expected-error {{constructor inherited from 'B' does not initialize any member of the anonymous union under profile 'std::init'}}
  union { int a; float f; }; // expected-note {{anonymous union declared here}}
};
struct DAnonUnionNsdmi : B {
  using B::B;
  union { int a = 0; float f; };
};

// A second (non-nominated) direct base whose default-initialization is
// indeterminate is left that way by the inherited constructor.
struct Indet { int x; };
struct DBase : B, Indet { // expected-note {{base class 'Indet' declared here}}
  using B::B;             // expected-error {{constructor inherited from 'B' does not initialize base class 'Indet' under profile 'std::init'}}
};

// Two using-declarations: members are diagnosed once (anchored at the
// lexically first introducer); bases are diagnosed per introducer (each
// inherited constructor initializes only its own nominated base).
struct B2 { B2(double); };
struct DTwo : B, B2, Indet { // expected-note 2 {{base class 'Indet' declared here}}
  using B::B;   // expected-error {{constructor inherited from 'B' does not initialize member 'm' under profile 'std::init'}} \
                // expected-error {{constructor inherited from 'B' does not initialize base class 'Indet' under profile 'std::init'}}
  using B2::B2; // expected-error {{constructor inherited from 'B2' does not initialize base class 'Indet' under profile 'std::init'}}
  int m;        // expected-note {{member 'm' declared here}}
};

// A using-declaration whose every inherited constructor is deleted inherits
// nothing callable and imposes no obligation (the base's implicit copy/move
// constructors never act as inherited constructors).
struct BDeleted { BDeleted(int) = delete; };
struct DDeleted : BDeleted {
  using BDeleted::BDeleted;
  int m;
};

// A class template defers on the (dependent) pattern; class finalization
// re-fires the check on each instantiation.
template <typename T>
struct DTemplate : B {
  using B::B; // expected-error {{constructor inherited from 'B' does not initialize member 'm' under profile 'std::init'}}
  T m;        // expected-note {{member 'm' declared here}}
};
template struct DTemplate<int>; // expected-note {{in instantiation of template class 'DTemplate<int>' requested here}}

// Class-level suppression covers the using-declaration.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
struct [[profiles::suppress(std::init)]] DSuppressed : B {
  using B::B;
  int m;
};
