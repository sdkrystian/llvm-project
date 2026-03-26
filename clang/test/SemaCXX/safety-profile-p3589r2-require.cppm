// Tests for P3589R2 [decl.attr.require]: profiles::require on import.
//
// RUN: rm -rf %t
// RUN: split-file %s %t
//
// Build module interface with profiles enforced:
// RUN: %clang_cc1 -std=c++20 -emit-module-interface -fsafety-profile-enforce=type -o %t/MyMod.pcm %t/MyMod.cppm
//
// Importing TU: require satisfied (type is enforced)
// RUN: %clang_cc1 -std=c++20 -fmodule-file=MyMod=%t/MyMod.pcm -fsyntax-only -verify=ok %t/ok.cpp
//
// Importing TU: require NOT satisfied (bounds not enforced by MyMod)
// RUN: %clang_cc1 -std=c++20 -fmodule-file=MyMod=%t/MyMod.pcm -fsyntax-only -verify=fail %t/fail.cpp

//--- MyMod.cppm
export module MyMod;
export int add(int a, int b) { return a + b; }

//--- ok.cpp
// ok-no-diagnostics
import MyMod [[profiles::require(std::type)]];
void f() {}

//--- fail.cpp
import MyMod [[profiles::require(std::bounds)]]; // fail-error {{imported module does not enforce required profile 'std::bounds'}}
void g() {}
