// RUN: %clang_cc1 -fsyntax-only -Wno-unused-value -std=c++20 -verify %s

namespace N0 {

  struct A {
    int x; // expected-note 3{{candidate found by name lookup is 'N0::A::x'}}

    void f(int); // expected-note 3{{candidate found by name lookup is 'N0::A::f'}}
    void f(char); // expected-note 3{{candidate found by name lookup is 'N0::A::f'}}

    using X = int; // expected-note {{candidate found by name lookup is 'N0::A::X'}}
  };

  struct B {
    char x; // expected-note 3{{candidate found by name lookup is 'N0::B::x'}}

    void f(int); // expected-note 3{{candidate found by name lookup is 'N0::B::f'}}
    void f(char); // expected-note 3{{candidate found by name lookup is 'N0::B::f'}}

    using X = char; // expected-note {{candidate found by name lookup is 'N0::B::X'}}
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

      f(0);
      A::f(0);
      B::f(0);
      C::f(0);
      T::f(0);

      this->f(0);
      this->A::f(0);
      this->B::f(0);
      this->C::f(0);
      this->T::f(0);

      static_cast<X *>(nullptr);
      static_cast<A::X *>(nullptr);
      static_cast<B::X *>(nullptr);
      static_cast<C::X *>(nullptr);
      static_cast<T::X *>(nullptr);
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

      f(0);
      A::f(0);
      B::f(0);
      C::f(0); // expected-error{{the result of lookup for 'f' in the template definition context differs from the result during instantiation}}
      T::f(0);

      this->f(0); // expected-error{{the result of lookup for 'f' in the template definition context differs from the result during instantiation}}
      this->A::f(0);
      this->B::f(0);
      this->C::f(0); // expected-error{{the result of lookup for 'f' in the template definition context differs from the result during instantiation}}
      this->T::f(0);

      static_cast<X *>(nullptr);
      static_cast<A::X *>(nullptr);
      static_cast<B::X *>(nullptr);
      static_cast<C::X *>(nullptr); // expected-error{{the result of lookup for 'X' in the template definition context differs from the result during instantiation}}
      static_cast<T::X *>(nullptr);
    }
  };

  template void C<B>::instantiated(); // expected-note{{in instantiation of member function 'N0::C<N0::B>::instantiated' requested here}}

} // namespace N0

namespace N1 {

  struct A {
    int x;

    void f(int);
    void f(char);

    using X = int;
  };

  struct B : virtual A {
    char x; // expected-note 3{{candidate found by name lookup is 'N1::B::x'}}
    bool y; // expected-note 2{{'B::y' declared here}}

    void f(int); // expected-note 3{{candidate found by name lookup is 'N1::B::f'}}
    void f(char); // expected-note 3{{candidate found by name lookup is 'N1::B::f'}}

    void g(int); // expected-note 2{{'B::g' declared here}}
    void g(char);

    using X = char; // expected-note{{candidate found by name lookup is 'N1::B::X'}}
    using Y = bool; // expected-note 4{{'B::Y' declared here}}
  };

  template<typename T>
  struct C : virtual A, T {
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

      y; // expected-error{{use of undeclared identifier 'y'}}
      A::y; // expected-error{{no member named 'y' in 'N1::A'}}
      B::y;
      C::y;
      T::y;

      this->y;
      this->A::y; // expected-error{{no member named 'y' in 'N1::A'}}
      this->B::y;
      this->C::y;
      this->T::y;

      f(0);
      A::f(0);
      B::f(0);
      C::f(0);
      T::f(0);

      this->f(0);
      this->A::f(0);
      this->B::f(0);
      this->C::f(0);
      this->T::f(0);

      g(0); // expected-error{{use of undeclared identifier 'g'}}
      A::g(0); // expected-error{{no member named 'g' in 'N1::A'}}
      B::g(0);
      C::g(0);
      T::g(0);

      this->g(0);
      this->A::g(0); // expected-error{{no member named 'g' in 'N1::A'}}
      this->B::g(0);
      this->C::g(0);
      this->T::g(0);

      static_cast<X *>(nullptr);
      static_cast<A::X *>(nullptr);
      static_cast<B::X *>(nullptr);
      static_cast<C::X *>(nullptr);
      static_cast<T::X *>(nullptr);

      static_cast<Y *>(nullptr); // expected-error{{unknown type name 'Y'}}
      static_cast<A::Y *>(nullptr); // expected-error{{no type named 'Y' in 'N1::A'}}
      static_cast<B::Y *>(nullptr);
      static_cast<C::Y *>(nullptr);
      static_cast<T::Y *>(nullptr);
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

      y; // expected-error{{use of undeclared identifier 'y'}}
      A::y; // expected-error{{no member named 'y' in 'N1::A'}}
      B::y;
      C::y;
      T::y;

      this->y;
      this->A::y; // expected-error{{no member named 'y' in 'N1::A'}}
      this->B::y;
      this->C::y;
      this->T::y;

      f(0);
      A::f(0);
      B::f(0);
      C::f(0); // expected-error{{the result of lookup for 'f' in the template definition context differs from the result during instantiation}}
      T::f(0);

      this->f(0); // expected-error{{the result of lookup for 'f' in the template definition context differs from the result during instantiation}}
      this->A::f(0);
      this->B::f(0);
      this->C::f(0); // expected-error{{the result of lookup for 'f' in the template definition context differs from the result during instantiation}}
      this->T::f(0);

      g(0); // expected-error{{use of undeclared identifier 'g'}}
      A::g(0); // expected-error{{no member named 'g' in 'N1::A'}}
      B::g(0);
      C::g(0);
      T::g(0);

      this->g(0);
      this->A::g(0); // expected-error{{no member named 'g' in 'N1::A'}}
      this->B::g(0);
      this->C::g(0);
      this->T::g(0);

      static_cast<X *>(nullptr);
      static_cast<A::X *>(nullptr);
      static_cast<B::X *>(nullptr);
      static_cast<C::X *>(nullptr); // expected-error{{the result of lookup for 'X' in the template definition context differs from the result during instantiation}}
      static_cast<T::X *>(nullptr);

      static_cast<Y *>(nullptr); // expected-error{{unknown type name 'Y'}}
      static_cast<A::Y *>(nullptr); // expected-error{{no type named 'Y' in 'N1::A'}}
      static_cast<B::Y *>(nullptr);
      static_cast<C::Y *>(nullptr);
      static_cast<T::Y *>(nullptr);
    }
  };

  template void C<B>::instantiated(); // expected-note{{in instantiation of member function 'N1::C<N1::B>::instantiated' requested here}}

} // namespace N1
