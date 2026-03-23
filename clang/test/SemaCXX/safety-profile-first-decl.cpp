// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

int x = 0; // non-profile declaration first

[[profiles::enforce(std::type)]]; // expected-error {{profile attribute must appear on the lexically first declaration in the translation unit}}
[[profiles::apply(std::bounds)]]; // expected-error {{profile attribute must appear on the lexically first declaration in the translation unit}}
