// RUN: %clang_cc1 -fsyntax-only -verify -std=c23 %s
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c23 %s

// The profiles framework is C++-only: in C the attributes are unknown
// attributes whose argument clauses are skipped as balanced tokens, with or
// without -fprofiles.

// expected-warning@+1 {{unknown attribute 'profiles::enforce' ignored}}
[[profiles::enforce(std::init)]];

// expected-warning@+1 {{unknown attribute 'profiles::suppress' ignored}}
[[profiles::suppress(std::init, justification: "reviewed")]] int x;

// expected-warning@+1 {{unknown attribute 'profiles::require' ignored}}
[[profiles::require(vendor::checks(fortify: 3))]] int f(void);
