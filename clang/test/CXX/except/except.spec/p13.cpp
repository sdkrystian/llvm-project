// RUN: %clang_cc1 -fexceptions -fcxx-exceptions -fsyntax-only -verify %s

struct A {
  static constexpr bool x = true;
};

template<typename T, typename U>
void f(T, U) noexcept(T::x);

template<typename T, typename U>
void f(T, U*) noexcept(T::x); // expected-note {{explicitly specialized declaration is here}}

template<typename T, typename U>
void f(T, U**) noexcept(T::y); // expected-note 2{{explicitly specialized declaration is here}}
                               // expected-error@-1 2{{no member named 'y' in 'A'}}

template<typename T, typename U>
void f(T, U***) noexcept(T::x); // expected-note {{explicitly specialized declaration is here}}


template<>
void f(A, int*) noexcept;

template<>
void f(A, int*); // expected-error {{exception specification in explicit specialization does not match the primary template}}

template<>
void f(A, int**) noexcept; // expected-error {{exception specification in explicit specialization does not match the primary template}}
                           // expected-note@-1 {{in instantiation of exception specification for 'f' requested here}}

template<>
void f(A, int**); // expected-error {{exception specification in explicit specialization does not match the primary template}}
                  // expected-note@-1 {{in instantiation of exception specification for 'f' requested here}}

template<>
void f(A, int***) noexcept;

template<>
void f(A, int***); // expected-error {{exception specification in explicit specialization does not match the primary template}}
