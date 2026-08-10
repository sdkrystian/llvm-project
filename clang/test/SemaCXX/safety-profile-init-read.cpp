// The DEMOTE run additionally enforces test::uninit_read to exercise profile
// table ordering, which would otherwise change the std::init-only diagnostics.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify=demote -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized -DDEMOTE %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized %s
// The LEADING_ERROR runs add a leading unrelated error so every later function
// is analyzed through the post-error path; the same profile diagnostics must
// still fire there.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s
// RUN: %clang_cc1 -fsyntax-only -verify=demote -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized -DDEMOTE -DLEADING_ERROR %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::other)]];
#ifdef DEMOTE
[[profiles::enforce(test::uninit_read)]];
#endif

namespace std { enum class byte : unsigned char {}; class type_info; }

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// demote-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-3 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

// The always-compiled suppress tests suppress both std::init and
// test::uninit_read so the function-under-test demonstrates std::init behavior
// in isolation under either run.
// no-profiles-warning@+2 {{'profiles::suppress' attribute ignored}}
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init)]] [[profiles::suppress(test::uninit_read)]]
void test_suppress_decl() {
  int x [[uninit]];
  int y = x;
  (void)y;
}

void test_suppress_stmt_inner() {
  int x [[uninit]];
  // no-profiles-warning@+2 {{'profiles::suppress' attribute ignored}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] [[profiles::suppress(test::uninit_read)]] {
    int y = x;
    (void)y;
  }
}

void test_suppress_var_init() {
  int x [[uninit]];
  // no-profiles-warning@+2 {{'profiles::suppress' attribute ignored}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] [[profiles::suppress(test::uninit_read)]]
  int y = x;
  (void)y;
}

void test_suppress_rule_targeted() {
  int x [[uninit]];
  // no-profiles-warning@+2 {{'profiles::suppress' attribute ignored}}
  // no-profiles-warning@+2 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_read")]]
  [[profiles::suppress(test::uninit_read)]] {
    int y = x;
    (void)y;
  }
}

void test_marker_then_write_then_read() {
  int x [[uninit]];
  x = 7;
  int y = x;
  (void)y;
}

// An unevaluated mention of the variable is not a read.
void test_unevaluated_contexts() {
  int x [[uninit]];
  (void)sizeof(x);
  (void)noexcept(x + 1);
  (void)__builtin_constant_p(x);
  (void)typeid(x);
  bool r = requires { x + 1; };
  (void)r;
  [[assume(x == 1)]];
}

void test_param(int p) {
  int y = p;
  (void)y;
}

#ifndef DEMOTE
void test_marker_does_not_excuse_read() {
  int x [[uninit]]; // expected-note {{variable 'x' is declared here}}
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'std::init'}}
  (void)y;
}

// An unevaluated mention earns no assignment credit either: the following
// real read still fires.
void test_unevaluated_no_credit() {
  int x [[uninit]]; // expected-note {{variable 'x' is declared here}}
  (void)__builtin_constant_p(x = 1);
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'std::init'}}
  (void)y;
}

void test_suppress_stmt_outer() {
  int x [[uninit]]; // expected-note {{variable 'x' is declared here}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] {
    int y = x;
    (void)y;
  }
  int z = x; // expected-error {{variable 'x' is read before initialization under profile 'std::init'}}
  (void)z;
}

template <typename T>
T template_uninit() {
  T x [[uninit]]; // expected-note {{variable 'x' is declared here}}
  return x; // expected-error {{variable 'x' is read before initialization under profile 'std::init'}}
}
void instantiate_template_uninit() {
  template_uninit<int>(); // expected-note {{in instantiation of function template specialization 'template_uninit<int>' requested here}}
}

void test_selective_suppress() {
  int x [[uninit]]; // expected-note {{variable 'x' is declared here}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::other)]] {
    int y = x; // expected-error {{variable 'x' is read before initialization under profile 'std::init'}}
    (void)y;
  }
}

void test_decl_suppress_does_not_extend() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] int x [[uninit]]; // expected-note {{variable 'x' is declared here}}
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'std::init'}}
  (void)y;
}

// std::byte may be read while uninitialized (paper section 4), so std::init
// does not diagnose a read of an uninitialized std::byte.
void test_byte_read_exempt() {
  std::byte b [[uninit]];
  std::byte c = b;
  (void)c;
}

// A self-init reads the uninitialized variable in its own initializer and is
// reported at the root cause, even with no later use.
void test_self_init() {
  int x = x; // expected-error {{variable 'x' is read before initialization under profile 'std::init'}} \
             // expected-note {{variable 'x' is declared here}}
  (void)&x;
}

void test_self_init_suppressed() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] int x = x; // OK: suppressed
  (void)&x;
}

// A std::byte self-init stays exempt (paper section 4).
void test_self_init_byte_exempt() {
  std::byte b = b;
  (void)&b;
}

void take_const_ref(const int &);
void take_const_ptr(const int *);

// A const-reference or const-pointer use of an uninitialized variable is a
// binding (ref_to_uninit territory, diagnosed at the binding site), not a
// read: uninit_read must not fire on it.
void test_const_ref_use_is_not_a_read() {
  int x [[uninit]];
  take_const_ref(x); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

void test_const_ptr_use_is_not_a_read() {
  int x [[uninit]];
  take_const_ptr(&x); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A read of a subobject of an [[uninit]] local -- an aggregate's member or an
// array's element -- is the read-through check's (this CFG pass does not track
// member accesses of record locals or arrays at all, and subobject-wise
// delayed initialization is banned, paper sections 5.4/5.5). Full coverage
// lives in safety-profile-init-ref-to-uninit.cpp.
struct Agg { int m; };
void test_member_read_of_uninit_aggregate() {
  Agg s [[uninit]];
  int y = s.m; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}

void test_element_read_of_uninit_array() {
  [[uninit]] int a[2];
  int y = a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}

// A non-field member is not a subobject of the object: a static data member
// is zero-initialized static storage, and an enumerator has no storage at
// all. Reading either through an [[uninit]] object is fine.
struct WithStatics {
  static int sm;
  enum { E = 1 };
  int x;
};
void test_static_member_read_of_uninit_object() {
  WithStatics s [[uninit]];
  int a = s.sm; // OK: static storage, not a subobject
  int b = s.E;  // OK: an enumerator has no storage
  (void)a; (void)b;
}
#endif

#ifdef DEMOTE
// With both test::uninit_read and std::init enforced, table order makes
// test::uninit_read fire first; suppressing it at the use site lets the
// std::init diagnostic surface.
void test_demote_test_profile() {
  int x [[uninit]]; // demote-note {{variable 'x' is declared here}}
  [[profiles::suppress(test::uninit_read)]] {
    int y = x; // demote-error {{variable 'x' is read before initialization under profile 'std::init'}}
    (void)y;
  }
}

// The std::byte exemption is std::init-only: test::uninit_read still diagnoses
// a read of an uninitialized std::byte.
void test_byte_not_exempt_under_test_profile() {
  std::byte b [[uninit]]; // demote-note {{variable 'b' is declared here}}
  std::byte c = b; // demote-error {{variable 'b' is read before initialization under profile 'test::uninit_read'}}
  (void)c;
}
#endif
