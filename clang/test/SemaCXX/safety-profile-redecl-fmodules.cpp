// A module-map module (a Clang header module, -fmodules) is textual
// inclusion wearing an AST file: in the standard's model its declarations
// belong to this TU, so it has no profile dominion of its own and the
// redeclaration compatibility check (P3589R2 [decl.attr.enforce]p5) must not
// compare against its (never-populated) exported designator set.

// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c++23 -fmodules -fimplicit-module-maps -fmodules-cache-path=%t.cache -I %t %t/main.cpp

//--- module.modulemap
module foo { header "foo.h" export * }

//--- foo.h
void f(int);

//--- main.cpp
// expected-no-diagnostics
[[profiles::enforce(std::init)]];
#include "foo.h"
void f(int);
