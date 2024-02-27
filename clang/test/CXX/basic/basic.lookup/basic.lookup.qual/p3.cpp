// RUN: %clang_cc1 -fsyntax-only -verify %s

template<typename T>
struct A {
  using AA = A;

  void f() {
    this->x; // expected-error{{}}
    (*this).x; // expected-error{{}}
    this->A::x; // expected-error{{}}
    (*this).A::x; // expected-error{{}}
    this->A<T>::x; // expected-error{{}}
    (*this).A<T>::x; // expected-error{{}}
    this->AA::x; // expected-error{{}}
    (*this).AA::x; // expected-error{{}}
    this->A<T>::AA::x; // expected-error{{}}
    (*this).A<T>::AA::x; // expected-error{{}}
  }
};
