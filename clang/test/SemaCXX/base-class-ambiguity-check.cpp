// RUN: %clang_cc1 -fsyntax-only -verify %s

template <typename T> class Foo {
  struct Base : T {};

  // Test that this code no longer causes a crash in Sema.
  struct Derived : Base, T {};
};


template <typename T> struct Foo2 {
  struct Base1; // expected-note{{forward declaration of 'Foo2::Base1'}}
  struct Base2; // expected-note{{forward declaration of 'Foo2::Base2'}}
  // Should not crash on an incomplete-type and dependent base specifier.
  struct Derived : Base1, Base2 {}; // expected-error2 {{base class has incomplete type}}
};

Foo2<int>::Derived a;
