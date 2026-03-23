// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// std::strict expands to type+bounds+lifetime
[[profiles::enforce(std::strict)]];

void test_strict_type() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

  long long ll = 100;
  int i = static_cast<int>(ll); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
}

void test_strict_bounds() {
  int x = 0;
  int *p = &x;
  p++; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

void test_strict_lifetime() {
  int *p = new int(42);
  delete p; // expected-error {{'delete' is not allowed when profile std::lifetime is enforced}}
}

// std::arithmetic is NOT part of std::strict
void test_strict_no_arithmetic() {
  int s = 42;
  unsigned u = s;
}

// Suppress std::strict suppresses type+bounds+lifetime
void test_suppress_strict() {
  [[profiles::suppress(std::strict)]] {
    int x;
    int *p = new int(42);
    delete p;
    int *q = &x;
    q++;
  }

  int y; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}
