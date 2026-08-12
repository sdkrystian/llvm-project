// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized %s
// The LEADING_ERROR runs add a leading unrelated error so every later function
// is analyzed through the post-error path; the same profile diagnostics must
// still fire there.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::cfg_hooks)]];

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

// The row's uninitialized-read rule fires under test::cfg_hooks.
void test_uninit_read() {
  int x; // expected-note {{variable 'x' is declared here}}
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::cfg_hooks'}}
  (void)y;
}

// The VarExempt hook exempts a variable named with an "exempt" prefix from
// the uninitialized-read rule; neither the self-init arm nor the use arm
// fires for it.
void test_var_exempt() {
  int exempt_x;
  int y = exempt_x;
  (void)y;
}

void test_var_exempt_self_init() {
  int exempt_x = exempt_x;
  (void)&exempt_x;
}

// The ExtraPass diagnoses every lambda-expression CFG element; the
// ConfigureCFG hook's always-add is what gives a lambda buried in an
// initializer an element of its own.
void test_extra_pass_lambda() {
  auto l = [] {}; // expected-error {{test profile fired on a lambda expression under profile 'test::cfg_hooks'}}
  (void)l;
}

// The ExtraPass gates each check site through shouldEmitProfileViolation: a
// suppression of its "lambda" rule covers the lambda but leaves the row's
// uninitialized-read rule live.
void test_extra_pass_rule_suppressed() {
  int x; // expected-note {{variable 'x' is declared here}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::cfg_hooks, rule: "lambda")]] auto l = [] {};
  (void)l;
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::cfg_hooks'}}
  (void)y;
}

// A whole-profile suppression covers the row's rule and the ExtraPass's rule
// alike.
void test_whole_profile_suppressed() {
  int x;
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::cfg_hooks)]] {
    int y = x;
    auto l = [] {};
    (void)y;
    (void)l;
  }
}
