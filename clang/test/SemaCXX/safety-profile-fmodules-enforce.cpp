// An enforcement written in source maps the profile's rule diagnostics to
// errors, which the import of an implicitly built module must not mistake for
// a -Werror option the module build lacked. The module's own code takes the
// importer's initial state, so the enforcement does not reach it.

// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++23 -fmodules -fimplicit-module-maps -fmodules-cache-path=%t.cache -I %t %t/main.cpp

//--- module.modulemap
module foo { header "foo.h" export * }

//--- foo.h
inline int *foo_cast(long l) { return reinterpret_cast<int *>(l); }

//--- main.cpp
[[profiles::enforce(test::type_cast)]];
#include "foo.h"
int *main_cast(long l) { return reinterpret_cast<int *>(l); } // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
