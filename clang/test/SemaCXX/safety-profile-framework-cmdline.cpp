// RUN: %clang_cc1 -fsyntax-only -verify=expected,mismatch -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=expected,mismatch -fprofiles -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=expected,mismatch -fprofiles-test-profiles -fprofiles-enforce=test::type_cast,test::type_cast -fprofiles-enforce=test::type_cast -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-test,mismatch -fprofiles-enforce=test::type_cast -std=c++23 %s

// -fprofiles-enforce= implies -fprofiles and enforces its profiles on the
// whole translation unit; an explicit -fprofiles or a repeated name changes
// nothing. The test:: gate still applies: without -fprofiles-test-profiles no
// test:: rule fires, while the enforcement itself is recorded.

// Repeating a command-line enforcement in source has no effect; a different
// designator for the same profile is the usual mismatch, with the note naming
// the option.
[[profiles::enforce(test::type_cast)]];
[[profiles::enforce(test::type_cast(fortify: 3))]]; // mismatch-error {{repeated enforcement of profile 'test::type_cast' with different designator}} mismatch-note {{profile 'test::type_cast' is enforced by '-fprofiles-enforce='}}
// A source enforcement of another profile coexists with the option's.
[[profiles::enforce(test::other)]];

void test_violation() {
  int *p = reinterpret_cast<int*>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

// Suppression applies as for a source enforcement.
[[profiles::suppress(test::type_cast)]]
void test_suppress_decl() {
  int *p = reinterpret_cast<int*>(0);
}

void test_suppress_stmt() {
  [[profiles::suppress(test::type_cast)]] int *p = reinterpret_cast<int*>(0);
}
