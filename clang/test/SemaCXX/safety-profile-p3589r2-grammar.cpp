// Tests for P3589R2 profile-name grammar extensions.
//
// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// Unknown multi-level profile names for enforce
[[profiles::enforce(acme::type)]]; // expected-error {{unknown safety profile 'acme::type'}}
[[profiles::enforce(std::lib::hardened)]]; // expected-error {{unknown safety profile 'std::lib::hardened'}}

// Unknown profile name for suppress (on a statement, not a declaration)
void f() {
  [[profiles::suppress(acme::foo)]] {  // expected-error {{unknown safety profile 'acme::foo'}}
    int x = 0;
  }
}
