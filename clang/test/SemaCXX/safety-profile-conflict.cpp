// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

[[profiles::enforce(std::type)]];
[[profiles::enforce(std::bounds)]];
[[profiles::apply(std::arithmetic)]];

// Conflict: type is already enforced
[[profiles::apply(std::type)]]; // expected-error {{cannot both enforce and apply profile 'std::type'}}
