// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// ==========================================================================
// enforce/apply only allowed on empty declarations
// ==========================================================================

[[profiles::enforce(std::type)]] void f1(); // expected-error {{'profiles::enforce' attribute only applies to empty declarations}}
[[profiles::enforce(std::type)]] int g1; // expected-error {{'profiles::enforce' attribute only applies to empty declarations}}

[[profiles::apply(std::arithmetic)]] void f2(); // expected-error {{'profiles::apply' attribute only applies to empty declarations}}
[[profiles::apply(std::arithmetic)]] int g2; // expected-error {{'profiles::apply' attribute only applies to empty declarations}}

// OK: enforce/apply on empty declarations
[[profiles::enforce(std::type)]];
[[profiles::enforce(std::arithmetic)]];

// ==========================================================================
// Unknown profile name
// ==========================================================================

[[profiles::enforce(std::nonexistent)]]; // expected-error {{unknown safety profile 'nonexistent'}}
[[profiles::apply(std::nonexistent)]]; // expected-error {{unknown safety profile 'nonexistent'}}

// ==========================================================================
// Enforce + apply conflict on the same profile
// ==========================================================================

[[profiles::apply(std::type)]]; // expected-error {{cannot both enforce and apply profile 'type'}}

// No conflict for different profiles:
[[profiles::apply(std::lifetime)]];

// ==========================================================================
// Enforce via attribute fires diagnostics
// ==========================================================================

void test_enforce_type() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

  long long ll = 100;
  int i = static_cast<int>(ll); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
}

void test_enforce_arithmetic() {
  int s = 42;
  unsigned u = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
}

// ==========================================================================
// Suppress scoping: restores after compound scope exits
// ==========================================================================

void test_suppress_scoping() {
  int s = 42;
  unsigned u = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}

  {
    [[profiles::suppress(std::arithmetic)]];
    unsigned v = s;
  }

  unsigned w = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
}

void test_suppress_type_scoping() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

  {
    [[profiles::suppress(std::type)]];
    int y;
  }

  int z; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

// ==========================================================================
// Suppress only one profile, other remains active
// ==========================================================================

void test_suppress_one_profile() {
  {
    [[profiles::suppress(std::arithmetic)]];
    int s = 42;
    unsigned u = s;

    int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  }

  {
    [[profiles::suppress(std::type)]];
    int x;

    int s = 42;
    unsigned u = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
  }
}

// ==========================================================================
// Unevaluated contexts are suppressed
// ==========================================================================

void test_unevaluated() {
  int x = 0;
  (void)sizeof(reinterpret_cast<long*>(&x));
  using T = decltype(reinterpret_cast<long*>(&x));
}
