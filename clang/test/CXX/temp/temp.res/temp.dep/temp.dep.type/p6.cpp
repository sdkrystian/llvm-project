// RUN: %clang_cc1 -fsyntax-only -verify %s

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-value"

namespace N0 {

  struct A {
    int x; // expected-note 3{{candidate found by name lookup is 'N0::A::x'}}
  };

  struct B {
    char x; // expected-note 3{{candidate found by name lookup is 'N0::B::x'}}
  };

  template<typename T>
  struct C : A, T {
    void not_instantiated() {
      x;
      A::x;
      B::x;
      C::x;
      T::x;

      this->x;
      this->A::x;
      this->B::x;
      this->C::x;
      this->T::x;
    }

    void instantiated() {
      x;
      A::x;
      B::x;
      C::x; // expected-error{{the result of lookup for 'x' in the template definition context differs from the result during instantiation}}
      T::x;

      this->x; // expected-error{{the result of lookup for 'x' in the template definition context differs from the result during instantiation}}
      this->A::x;
      this->B::x;
      this->C::x; // expected-error{{the result of lookup for 'x' in the template definition context differs from the result during instantiation}}
      this->T::x;
    }
  };

  template void C<B>::instantiated(); // expected-note{{in instantiation of member function 'N0::C<N0::B>::instantiated' requested here}}

} // namespace N0

namespace N1 {

  struct A {
    int x;
  };

  struct B : virtual A {
  };

  struct C : virtual A {
    char x; // expected-note 3{{candidate found by name lookup is 'N1::C::x'}}
    bool y; // expected-note 4{{'C::y' declared here}}
  };

  template<typename T>
  struct D : B, T {
    void not_instantiated() {
      x;
      D::x;
      C::x;
      B::x;
      A::x;

      this->x;
      this->D::x;
      this->C::x;
      this->B::x;
      this->A::x;

      y; // expected-error{{use of undeclared identifier 'y'}}
      D::y;
      C::y;
      B::y; // expected-error{{no member named 'y' in 'N1::B'}}
      A::y; // expected-error{{no member named 'y' in 'N1::A'}}

      this->y;
      this->D::y;
      this->C::y;
      this->B::y; // expected-error{{no member named 'y' in 'N1::B'}}
      this->A::y; // expected-error{{no member named 'y' in 'N1::A'}}
    }

    void instantiated() {
      x;
      D::x; // expected-error{{the result of lookup for 'x' in the template definition context differs from the result during instantiation}}
      C::x;
      B::x;
      A::x;

      this->x; // expected-error{{the result of lookup for 'x' in the template definition context differs from the result during instantiation}}
      this->D::x; // expected-error{{the result of lookup for 'x' in the template definition context differs from the result during instantiation}}
      this->C::x;
      this->B::x;
      this->A::x;

      y; // expected-error{{use of undeclared identifier 'y'}}
      D::y;
      C::y;
      B::y; // expected-error{{no member named 'y' in 'N1::B'}}
      A::y; // expected-error{{no member named 'y' in 'N1::A'}}

      this->y;
      this->D::y;
      this->C::y;
      this->B::y; // expected-error{{no member named 'y' in 'N1::B'}}
      this->A::y; // expected-error{{no member named 'y' in 'N1::A'}}
    }
  };

  template void D<C>::instantiated(); // expected-note{{in instantiation of member function 'N1::D<N1::C>::instantiated' requested here}}

} // namespace N1

#pragma clang diagnostic pop
