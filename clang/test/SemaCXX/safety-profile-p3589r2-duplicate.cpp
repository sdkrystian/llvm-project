// Tests for P3589R2 [decl.attr.enforce] para 3: duplicate enforcement.
//
// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// Identical repetition is valid
[[profiles::enforce(std::type)]];
[[profiles::enforce(std::type)]];

// Different name for same profile is a conflict
[[profiles::enforce(type)]]; // expected-error {{conflicting profile enforcement attributes for 'type'}}
