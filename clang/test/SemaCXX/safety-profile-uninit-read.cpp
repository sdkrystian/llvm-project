// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized %s
// The LEADING_ERROR runs add a leading unrelated error so every later function
// is analyzed through the post-error path; the same profile diagnostics must
// still fire there.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::uninit_read)]];
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::other)]];

namespace std { class type_info; }

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(test::uninit_read)]]
void test_suppress_decl() {
  int x;
  int y = x;
  (void)y;
}

void test_suppress_stmt_inner() {
  int x;
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] {
    int y = x;
    (void)y;
  }
}

void test_suppress_var_init() {
  int x;
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] int y = x;
  (void)y;
}

// A declarator-id suppression on the definition covers the body for the
// CFG-routed rules (Decl-based suppression, not the parse-time stack).
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
void test_suppress_declarator_id [[profiles::suppress(test::uninit_read)]] () {
  int x;
  int y = x;
  (void)y;
}

void test_discarded_branch() {
  int x;
  if constexpr (false) {
    int y = x;
    (void)y;
  }
}

void test_unevaluated() {
  int x;
  using T = decltype(x);
  (void)sizeof(x);
  (void)static_cast<T>(0);
  (void)noexcept(x + 1);
  (void)__builtin_constant_p(x);
  (void)typeid(x);
  bool r = requires { x + 1; };
  (void)r;
  [[assume(x == 1)]];
}

// An unevaluated mention earns no assignment credit either: the following
// real read still fires.
void test_unevaluated_no_credit() {
  int x; // expected-note {{variable 'x' is declared here}}
  (void)__builtin_constant_p(x = 1);
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
  (void)y;
}

void test_param(int p) {
  int y = p;
  (void)y;
}

int g_static;
void test_global() {
  int y = g_static;
  (void)y;
}

// A self-init reads the uninitialized variable in its own initializer; the
// profile reports it at the root cause even when there is no later use.
void test_self_init_no_use() {
  int x = x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}} \
             // expected-note {{variable 'x' is declared here}}
  (void)&x;
}

void test_suppress_self_init() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] int x = x;
  (void)&x;
}

void test_violation() {
  int x; // expected-note {{variable 'x' is declared here}}
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
  (void)y;
}

void test_suppress_stmt_outer() {
  int x; // expected-note {{variable 'x' is declared here}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] {
    int y = x;
    (void)y;
  }
  int z = x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
  (void)z;
}

template <typename T>
T template_uninit() {
  T x; // expected-note {{variable 'x' is declared here}}
  return x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
}
void instantiate_template_uninit() {
  template_uninit<int>(); // expected-note {{in instantiation of function template specialization 'template_uninit<int>' requested here}}
}

void test_self_init_with_use() {
  int x = x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}} \
             // expected-note {{variable 'x' is declared here}}
  int y = x;
  (void)y;
}

void test_selective_suppress() {
  int x; // expected-note {{variable 'x' is declared here}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::other)]] {
    int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
    (void)y;
  }
}

// Suppress on a declaration is token-based: it covers the initializer of that
// declaration but not later uses that live in different declarations.
void test_decl_suppress_does_not_extend() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] int x; // expected-note {{variable 'x' is declared here}}
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
  (void)y;
}

void take_const_ref(const int &);
void take_const_ptr(const int *);

// A const-reference or const-pointer use of an uninitialized variable is a
// binding, not a read: the profile must not report it as one.
void test_const_ref_use_not_diagnosed() {
  int x;
  take_const_ref(x);
  take_const_ptr(&x);
}
