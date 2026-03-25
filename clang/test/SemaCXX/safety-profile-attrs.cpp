// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// Profile enforcement must come first in the TU.
[[profiles::enforce(std::type)]];
[[profiles::enforce(std::arithmetic)]];
[[profiles::enforce(std::lifetime)]];
[[profiles::enforce(std::bounds)]];

// enforce/apply only allowed on empty declarations
[[profiles::enforce(std::type)]] void f1(); // expected-error {{'profiles::enforce' attribute only applies to empty declarations}}
[[profiles::enforce(std::type)]] int g1; // expected-error {{'profiles::enforce' attribute only applies to empty declarations}}

[[profiles::apply(std::arithmetic)]] void f2(); // expected-error {{'profiles::apply' attribute only applies to empty declarations}}
[[profiles::apply(std::arithmetic)]] int g2; // expected-error {{'profiles::apply' attribute only applies to empty declarations}}

// Unknown profile name
[[profiles::enforce(std::nonexistent)]]; // expected-error {{unknown safety profile 'nonexistent'}}
[[profiles::apply(std::nonexistent)]]; // expected-error {{unknown safety profile 'nonexistent'}}

// First-decl check fires (and preempts the conflict check) because
// the non-empty declarations above are non-profile declarations.
[[profiles::apply(std::type)]]; // expected-error {{profile attribute must appear on the lexically first declaration in the translation unit}}

void test_enforce_type() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

  long long ll = 100;
  int i = static_cast<int>(ll); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
}

void test_enforce_arithmetic() {
  int s = 42;
  unsigned u = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
}

void test_enforce_lifetime() {
  int *p = new int(42);
  delete p; // expected-error {{'delete' is not allowed when profile std::lifetime is enforced}}

  // Pointer dereference is profile-checked (runtime), not profile-rejected.
  int x = 0;
  int *q = &x;
  int v = *q;
}

void test_enforce_bounds() {
  int x = 0;
  int *p = &x;
  p++; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  int arr[4] = {};
  int *q = arr; // expected-error {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
}

// Suppress scoping: null-statement form persists for rest of block
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

void test_suppress_lifetime_scoping() {
  int *p = new int(42);
  delete p; // expected-error {{'delete' is not allowed when profile std::lifetime is enforced}}

  {
    [[profiles::suppress(std::lifetime)]];
    int *q = new int(1);
    delete q;
  }

  delete p; // expected-error {{'delete' is not allowed when profile std::lifetime is enforced}}
}

void test_suppress_bounds_scoping() {
  int x = 0;
  int *p = &x;
  p++; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  {
    [[profiles::suppress(std::bounds)]];
    p++;
  }

  p++; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

// Suppress on a single statement (not null-statement form)
void test_suppress_single_statement() {
  long long ll = 100;
  int i = static_cast<int>(ll); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}

  [[profiles::suppress(std::type)]]
  int j = static_cast<int>(ll);

  int k = static_cast<int>(ll); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
}

// Suppress on a compound statement
void test_suppress_compound_statement() {
  int s = 42;
  unsigned u = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}

  [[profiles::suppress(std::arithmetic)]] {
    unsigned v = s;
  }

  unsigned w = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
}

// Suppress only one profile, others remain active
void test_suppress_one_profile() {
  {
    [[profiles::suppress(std::arithmetic)]];
    int s = 42;
    unsigned u = s;

    int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

    int *p = new int(1);
    delete p; // expected-error {{'delete' is not allowed when profile std::lifetime is enforced}}

    int *q = &s;
    q++; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  }

  {
    [[profiles::suppress(std::bounds)]];
    int x = 0;
    int *p = &x;
    p++;

    int s = 42;
    unsigned u = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}

    int y; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

    delete p; // expected-error {{'delete' is not allowed when profile std::lifetime is enforced}}
  }
}

// Unevaluated contexts are suppressed
void test_unevaluated() {
  int x = 0;
  (void)sizeof(reinterpret_cast<long*>(&x));
  using T = decltype(reinterpret_cast<long*>(&x));
}
