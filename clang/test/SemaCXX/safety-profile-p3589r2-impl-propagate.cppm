// Test for P3589R2 [decl.attr.enforce] para 4: enforcement dominion extends
// from module interface to implementation units.
//
// RUN: rm -rf %t
// RUN: split-file %s %t
//
// Build interface with type enforced:
// RUN: %clang_cc1 -std=c++20 -emit-module-interface -fsafety-profile-enforce=type -o %t/M.pcm %t/interface.cppm
//
// Build implementation — should inherit type enforcement from interface:
// RUN: %clang_cc1 -std=c++20 -fmodule-file=M=%t/M.pcm -fsyntax-only -verify %t/impl.cpp

//--- interface.cppm
export module M;
export int add(int a, int b) { return a + b; }

//--- impl.cpp
module M;
void f() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}
