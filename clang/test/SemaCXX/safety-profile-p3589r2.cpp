// Tests for P3589R2 "C++ Profiles: The Framework" conformance.
//
// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 -fsafety-profile-enforce=type -fsafety-profile-enforce=bounds %s

// =====================================================================
// P3589R2 [decl.attr.suppress]: suppress with justification: argument
// =====================================================================

void test_suppress_justification() {
  long long ll = 100;
  [[profiles::suppress(std::type, justification: "known safe narrowing")]]
  int i = static_cast<int>(ll);

  int j = static_cast<int>(ll); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
}

// =====================================================================
// P3589R2 [decl.attr.suppress] para 4: suppress with rule: argument
//   only suppresses that specific rule, not the whole profile.
// =====================================================================

void test_suppress_rule() {
  long long ll = 100;

  [[profiles::suppress(std::type, rule: "expr.static.cast")]] {
    int i = static_cast<int>(ll);
    int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  }
}

void test_suppress_rule_reinterpret() {
  int x = 0;
  int *p = &x;

  [[profiles::suppress(std::type, rule: "expr.reinterpret.cast")]] {
    long *q = reinterpret_cast<long *>(p);
    int y; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  }
}

void test_suppress_rule_const_cast() {
  const int x = 42;
  const int *cp = &x;

  [[profiles::suppress(std::type, rule: "expr.const.cast")]] {
    int *mp = const_cast<int *>(cp);
    int y; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  }
}

void test_suppress_rule_uninit() {
  [[profiles::suppress(std::type, rule: "basic.life")]] {
    int x;
    int *p = &x;
    long *q = reinterpret_cast<long *>(p); // expected-error {{'reinterpret_cast' is not allowed when profile std::type is enforced}}
  }
}

// =====================================================================
// P3589R2: suppress with both rule: and justification:
// =====================================================================

void test_suppress_rule_and_justification() {
  int x = 0;
  int *p = &x;

  [[profiles::suppress(std::type,
                       justification: "needed for FFI interop",
                       rule: "expr.reinterpret.cast")]] {
    long *q = reinterpret_cast<long *>(p);
    int y; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  }
}

// =====================================================================
// P3589R2: suppress with rule: for bounds profile
// =====================================================================

void test_suppress_bounds_rule() {
  int x = 0;
  int *p = &x;

  [[profiles::suppress(std::bounds, rule: "expr.add")]] {
    int *q = p + 1;
    int arr[4] = {};
    int *r = arr; // expected-error {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
  }
}

// =====================================================================
// P3589R2: suppress with rule: scoping — does not persist
// =====================================================================

void test_suppress_rule_scoping() {
  int x = 0;
  int *p = &x;

  [[profiles::suppress(std::type, rule: "expr.reinterpret.cast")]]
  (void)reinterpret_cast<long *>(p);

  (void)reinterpret_cast<long *>(p); // expected-error {{'reinterpret_cast' is not allowed when profile std::type is enforced}}
}

// =====================================================================
// P3589R2: whole-profile suppress still works as before
// =====================================================================

void test_suppress_whole_profile() {
  [[profiles::suppress(std::type)]] {
    int x;
    long long ll = 100;
    int i = static_cast<int>(ll);
    int *p = &i;
    long *q = reinterpret_cast<long *>(p);
  }
  int y; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}
