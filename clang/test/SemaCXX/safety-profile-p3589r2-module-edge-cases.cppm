// Edge-case tests for P3589R2 module-related profile enforcement.
//
// RUN: rm -rf %t
// RUN: split-file %s %t
//
// Enforce on module-declaration (inline attribute, no CLI flag):
// RUN: %clang_cc1 -std=c++20 -emit-module-interface -o %t/InlineEnforce.pcm %t/InlineEnforce.cppm
//
// Unknown profile on module-declaration:
// RUN: not %clang_cc1 -std=c++20 -fsyntax-only %t/UnknownProfile.cppm 2>&1 | FileCheck --check-prefix=UNKNOWN %t/UnknownProfile.cppm
//
// Duplicate/conflict on module-declaration:
// RUN: not %clang_cc1 -std=c++20 -fsyntax-only %t/DuplicateConflict.cppm 2>&1 | FileCheck --check-prefix=CONFLICT %t/DuplicateConflict.cppm
//
// Global module fragment with enforce:
// RUN: %clang_cc1 -std=c++20 -emit-module-interface -o %t/GMF.pcm %t/GMF.cppm
//
// Require satisfied for inline-enforced module:
// RUN: %clang_cc1 -std=c++20 -fmodule-file=InlineEnforce=%t/InlineEnforce.pcm -fsyntax-only -verify=ok %t/RequireOK.cpp
//
// Require NOT satisfied for inline-enforced module:
// RUN: %clang_cc1 -std=c++20 -fmodule-file=InlineEnforce=%t/InlineEnforce.pcm -fsyntax-only -verify=fail %t/RequireFail.cpp

//--- InlineEnforce.cppm
export module InlineEnforce [[profiles::enforce(std::type)]];
export int add(int a, int b) { return a + b; }

//--- UnknownProfile.cppm
export module UnknownProfile [[profiles::enforce(acme::unknown)]];
// UNKNOWN: error: unknown safety profile 'acme::unknown'

//--- DuplicateConflict.cppm
export module DuplicateConflict [[profiles::enforce(std::type), profiles::enforce(type)]];
// CONFLICT: error: conflicting profile enforcement attributes for 'type'

//--- GMF.cppm
module;
[[profiles::enforce(std::type)]];
export module GMF;
export int value() {
  int x = 42;
  return x;
}

//--- RequireOK.cpp
// ok-no-diagnostics
import InlineEnforce [[profiles::require(std::type)]];
void f() {}

//--- RequireFail.cpp
import InlineEnforce [[profiles::require(std::bounds)]]; // fail-error {{imported module does not enforce required profile 'std::bounds'}}
void g() {}
