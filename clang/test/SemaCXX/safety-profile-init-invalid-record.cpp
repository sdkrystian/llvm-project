// RUN: %clang_cc1 -fsyntax-only -verify=expected,common -fprofiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles,common -std=c++23 %s

// A local of an invalid self-containing recovery record reaches the
// analysis-based passes with the record errors already emitted (the
// post-error rerun analyzes valid bodies regardless); the tracked-variable
// classification (recordIsNotEmpty) must terminate on the cyclic field
// graph instead of recursing forever. The record errors fire in both runs;
// neither run adds any profile diagnostic for these locals.

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

struct SelfC { // common-note {{definition of 'SelfC' is not complete until the closing '}'}}
  SelfC x; // common-error {{field has incomplete type 'SelfC'}}
};

struct A; // common-note {{forward declaration of 'A'}}
struct B {
  A a; // common-error {{field has incomplete type 'A'}}
};
struct A {
  B b;
};

// An array of the record itself does not recurse (getAsRecordDecl does not
// look through array types); kept as termination coverage.
struct SelfArr { // common-note {{definition of 'SelfArr' is not complete until the closing '}'}}
  SelfArr x[2]; // common-error {{field has incomplete type 'SelfArr'}}
};

void use() {
  SelfC s;
  A a;
  SelfArr r;
  (void)s;
  (void)a;
  (void)r;
}
