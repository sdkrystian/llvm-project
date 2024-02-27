// RUN: %clang_cc1 -fsyntax-only -verify %s

namespace N0 {

  struct A {
    using AA = A;

    int y;
  };

  template<typename T>
  struct B : A {
    using BB = B;

    // These should all be diagnosed prior to instantiation
    void f(B* a, B& b) {
      this->x; // expected-error{{no member named}}
      this->A::x; // expected-error{{no member named}}
      this->B::x; // expected-error{{no member named}}
      this->A::AA::x; // expected-error{{no member named}}
      this->B::AA::x; // expected-error{{no member named}}
      this->B::BB::x; // expected-error{{no member named}}
      this->AA::x; // expected-error{{no member named}}
      this->BB::x; // expected-error{{no member named}}
      this->B<T>::x; // expected-error{{no member named}}
      this->B<T>::A::x; // expected-error{{no member named}}
      this->B<T>::AA::x; // expected-error{{no member named}}
      this->B<T>::BB::x; // expected-error{{no member named}}
      this->B<T>::A::AA::x; // expected-error{{no member named}}

      a->x; // expected-error{{no member named}}
      a->A::x; // expected-error{{no member named}}
      a->B::x; // expected-error{{no member named}}
      a->A::AA::x; // expected-error{{no member named}}
      a->B::AA::x; // expected-error{{no member named}}
      a->B::BB::x; // expected-error{{no member named}}
      a->AA::x; // expected-error{{no member named}}
      a->BB::x; // expected-error{{no member named}}
      a->B<T>::x; // expected-error{{no member named}}
      a->B<T>::A::x; // expected-error{{no member named}}
      a->B<T>::AA::x; // expected-error{{no member named}}
      a->B<T>::BB::x; // expected-error{{no member named}}
      a->B<T>::A::AA::x; // expected-error{{no member named}}


      (*this).x; // expected-error{{no member named}}
      (*this).A::x; // expected-error{{no member named}}
      (*this).B::x; // expected-error{{no member named}}
      (*this).A::AA::x; // expected-error{{no member named}}
      (*this).B::AA::x; // expected-error{{no member named}}
      (*this).B::BB::x; // expected-error{{no member named}}
      (*this).AA::x; // expected-error{{no member named}}
      (*this).BB::x; // expected-error{{no member named}}
      (*this).B<T>::x; // expected-error{{no member named}}
      (*this).B<T>::A::x; // expected-error{{no member named}}
      (*this).B<T>::AA::x; // expected-error{{no member named}}
      (*this).B<T>::BB::x; // expected-error{{no member named}}
      (*this).B<T>::A::AA::x; // expected-error{{no member named}}

      b.x; // expected-error{{no member named}}
      b.A::x; // expected-error{{no member named}}
      b.B::x; // expected-error{{no member named}}
      b.A::AA::x; // expected-error{{no member named}}
      b.B::AA::x; // expected-error{{no member named}}
      b.B::BB::x; // expected-error{{no member named}}
      b.AA::x; // expected-error{{no member named}}
      b.BB::x; // expected-error{{no member named}}
      b.B<T>::x; // expected-error{{no member named}}
      b.B<T>::A::x; // expected-error{{no member named}}
      b.B<T>::AA::x; // expected-error{{no member named}}
      b.B<T>::BB::x; // expected-error{{no member named}}
      b.B<T>::A::AA::x; // expected-error{{no member named}}

      A::x; // expected-error{{no member named}}
      B::x; // expected-error{{no member named}}
      AA::x; // expected-error{{no member named}}
      BB::x; // expected-error{{no member named}}
      A::AA::x; // expected-error{{no member named}}
      B::A::x; // expected-error{{no member named}}
      B::BB::x; // expected-error{{no member named}}
      B::AA::x; // expected-error{{no member named}}
      B<T>::x; // expected-error{{no member named}}
      B<T>::A::x; // expected-error{{no member named}}
      B<T>::AA::x; // expected-error{{no member named}}
      B<T>::BB::x; // expected-error{{no member named}}
    }
  };

} // namespace N0

namespace N1 {

  struct A {
    using AA = A;

    int y;
  };

  template<typename T>
  struct B : T, A {
    using BB = B;

    void f(B* a, B& b) {
      a->x;
      a->A::x; // expected-error{{no member named}}
      a->B::x;
      a->A::AA::x; // expected-error{{no member named}}
      a->B::AA::x; // expected-error{{no member named}}
      a->B::BB::x;
      a->AA::x; // expected-error{{no member named}}
      a->BB::x;
      a->B<T>::x;
      a->B<T>::A::x; // expected-error{{no member named}}
      a->B<T>::AA::x; // expected-error{{no member named}}
      a->B<T>::BB::x;
      a->B<T>::A::AA::x; // expected-error{{no member named}}

      b.x;
      b.A::x; // expected-error{{no member named}}
      b.B::x;
      b.A::AA::x; // expected-error{{no member named}}
      b.B::AA::x; // expected-error{{no member named}}
      b.B::BB::x;
      b.AA::x; // expected-error{{no member named}}
      b.BB::x;
      b.B<T>::x;
      b.B<T>::A::x; // expected-error{{no member named}}
      b.B<T>::AA::x; // expected-error{{no member named}}
      b.B<T>::BB::x;
      b.B<T>::A::AA::x; // expected-error{{no member named}}

      A::x; // expected-error{{no member named}}
      B::x;
      AA::x; // expected-error{{no member named}}
      BB::x;
      A::AA::x; // expected-error{{no member named}}
      B::A::x; // expected-error{{no member named}}
      B::BB::x;
      B::AA::x; // expected-error{{no member named}}
      B<T>::x;
      B<T>::A::x; // expected-error{{no member named}}
      B<T>::AA::x; // expected-error{{no member named}}
      B<T>::BB::x;
    }
  };

} // namespace N1
