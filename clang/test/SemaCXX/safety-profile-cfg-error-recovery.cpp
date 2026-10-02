// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++23 -w %s

[[profiles::enforce(test::cfg_hooks)]];

// A body whose own scope reported an error gets no rider diagnostics.
void own_error() {
  int x;
  int y = x;
  (void)y;
  auto l = [] {};
  (void)l;
  switch (0) 0 = 0:; // expected-error {{expression is not assignable}} expected-error {{expected 'case' keyword before expression}}
}

// A case value that contains errors skips the body.
void case_value_error() {
  switch (0) { case undeclared1(): break; } // expected-error {{use of undeclared identifier 'undeclared1'}}
}

// A case value naming a constant whose initializer failed skips the body.
constexpr int k = undeclared2(); // expected-error {{use of undeclared identifier 'undeclared2'}}
void case_value_from_failed_constant() {
  int x;
  switch (0) { case k: break; }
  int y = x;
  (void)y;
}

// An array new-expression whose size cannot be determined skips the body.
void array_new_without_size() {
  char *p = new char[](); // expected-error {{cannot determine allocated array size from initializer}}
  int (*q)[3] = new int[][3](); // expected-error {{cannot determine allocated array size from initializer}}
  (void)p;
  (void)q;
}

// An instantiation of a pattern with an error is skipped.
template <class T> void instantiated_case_error() {
  T x;
  T y = x;
  (void)y;
  switch (0) { case undeclared3(): break; } // expected-error {{use of undeclared identifier 'undeclared3'}}
}
template void instantiated_case_error<int>();

// A lambda body naming a failed constant skips the enclosing body too.
void lambda_case_value_from_failed_constant() {
  auto l = [] { switch (0) { case k: break; } };
  (void)l;
}

// A failed default member initializer skips the constructor using it.
struct FailedDefaultMemberInit {
  int m = undeclared4(); // expected-error {{use of undeclared identifier 'undeclared4'}}
  FailedDefaultMemberInit() {
    int x;
    int y = x;
    (void)y;
  }
};

// A constructor is checked over a sizeless array new member initializer.
struct DefaultMemberInitArrayNew {
  char *p = new char[](); // expected-error {{cannot determine allocated array size from initializer}}
  DefaultMemberInitArrayNew() {
    int x; // expected-note {{variable 'x' is declared here}}
    int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::cfg_hooks'}}
    (void)y;
  }
};

// A body after the errors above is checked.
void after_errors() {
  int x; // expected-note {{variable 'x' is declared here}}
  int y = x; // expected-error {{variable 'x' is read before initialization under profile 'test::cfg_hooks'}}
  (void)y;
  auto l = [] {}; // expected-error {{test profile fired on a lambda expression under profile 'test::cfg_hooks'}}
  (void)l;
}
