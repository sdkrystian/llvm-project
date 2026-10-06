// A class whose definition two modules merge has one member table: a member
// expression naming either module's field matches it.

// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fmodules -fimplicit-module-maps -fmodules-cache-path=%t/cache -I %t %t/main.cpp

//--- module.modulemap
module A { header "a.h" export * }
module B { header "b.h" export * }

//--- agg.h
#pragma once
struct Agg {
  int m [[uninit]];
  int k = 0;
};

//--- a.h
#include "agg.h"
inline int fa() { Agg a; a.m = 1; return a.m; }

//--- b.h
#include "agg.h"
inline int fb() { Agg b; b.m = 1; return b.m; }

//--- main.cpp
[[profiles::enforce(std::init)]];
#include "a.h"
#include "b.h"

// A read of a merged class's tracked member before its assignment is rejected
int read_before_assignment() {
  Agg x;
  return x.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
              // expected-note@agg.h:3 {{member 'm' declared here}}
}

// A read of a merged class's tracked member after its assignment is accepted
int read_after_assignment() {
  Agg x;
  x.m = 1;
  return x.m;
}
