// RUN: %clang_cc1 -fsyntax-only -verify=enforced -std=c++20 -fsafety-profile-enforce=arithmetic %s
// RUN: %clang_cc1 -fsyntax-only -verify=applied -std=c++20 -fsafety-profile-apply=arithmetic -Wsafety-profile-arithmetic %s

// --- Rule 7.1: Narrowing conversions ---

void test_narrowing() {
  long long ll = 1LL;
  double d = 1.0;

  int a{ll};
  // enforced-error@-1 {{non-constant-expression cannot be narrowed from type 'long long' to 'int'}}
  // enforced-note@-2 {{insert an explicit cast}}
  // enforced-error@-3 {{narrowing conversion from 'long long' to 'int' is not allowed when profile std::arithmetic is enforced}}
  // applied-error@-4 {{non-constant-expression cannot be narrowed from type 'long long' to 'int'}}
  // applied-note@-5 {{insert an explicit cast}}
  // applied-warning@-6 {{narrowing conversion from 'long long' to 'int' is discouraged by profile std::arithmetic}}

  int b{d};
  // enforced-error@-1 {{type 'double' cannot be narrowed to 'int'}}
  // enforced-note@-2 {{insert an explicit cast}}
  // enforced-error@-3 {{narrowing conversion from 'double' to 'int' is not allowed when profile std::arithmetic is enforced}}
  // applied-error@-4 {{type 'double' cannot be narrowed to 'int'}}
  // applied-note@-5 {{insert an explicit cast}}
  // applied-warning@-6 {{narrowing conversion from 'double' to 'int' is discouraged by profile std::arithmetic}}

  int c{10000000000LL};
  // enforced-error@-1 {{constant expression evaluates to 10000000000 which cannot be narrowed to type 'int'}}
  // enforced-note@-2 {{insert an explicit cast}}
  // enforced-error@-3 {{narrowing conversion from 'long long' to 'int' is not allowed when profile std::arithmetic is enforced}}
  // enforced-warning@-4 {{implicit conversion from 'long long' to 'int' changes value from 10000000000 to 1410065408}}
  // applied-error@-5 {{constant expression evaluates to 10000000000 which cannot be narrowed to type 'int'}}
  // applied-note@-6 {{insert an explicit cast}}
  // applied-warning@-7 {{narrowing conversion from 'long long' to 'int' is discouraged by profile std::arithmetic}}
  // applied-warning@-8 {{implicit conversion from 'long long' to 'int' changes value from 10000000000 to 1410065408}}
}

// --- Rule 7.2: Signedness conversions ---

void test_signedness() {
  int s = 42;
  unsigned u = 42u;

  unsigned x = s;
  // enforced-error@-1 {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
  // applied-warning@-2 {{implicit conversion changes signedness from 'int' to 'unsigned int'; discouraged by profile std::arithmetic}}

  int y = u;
  // enforced-error@-1 {{implicit conversion changes signedness from 'unsigned int' to 'int' when profile std::arithmetic is enforced}}
  // applied-warning@-2 {{implicit conversion changes signedness from 'unsigned int' to 'int'; discouraged by profile std::arithmetic}}
}

// --- Rule 7.3: Arithmetic overflow ---

void test_overflow() {
  int a = __INT_MAX__ + 1;
  // enforced-warning@-1 {{overflow in expression; result is -2'147'483'648 with type 'int'}}
  // enforced-error@-2 {{arithmetic overflow is not allowed when profile std::arithmetic is enforced}}
  // applied-warning@-3 {{overflow in expression; result is -2'147'483'648 with type 'int'}}
  // applied-warning@-4 {{arithmetic overflow is discouraged by profile std::arithmetic}}

  int b = -(-__INT_MAX__ - 1);
  // enforced-warning@-1 {{overflow in expression; result is -2'147'483'648 with type 'int'}}
  // enforced-error@-2 {{arithmetic overflow is not allowed when profile std::arithmetic is enforced}}
  // applied-warning@-3 {{overflow in expression; result is -2'147'483'648 with type 'int'}}
  // applied-warning@-4 {{arithmetic overflow is discouraged by profile std::arithmetic}}
}
