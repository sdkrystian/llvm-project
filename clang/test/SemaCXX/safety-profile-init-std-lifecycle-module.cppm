// The std::init roles of the standard library's lifecycle functions are
// looked up at the call, so a module or header unit that provides them reads
// the same whether or not it was built with -fprofiles.

// RUN: rm -rf %t
// RUN: mkdir -p %t
// RUN: split-file %s %t

// RUN: %clang_cc1 -std=c++23 -emit-module-interface %t/lib.cppm -o %t/lib.pcm
// RUN: %clang_cc1 -std=c++23 -fprofiles -emit-module-interface %t/lib.cppm -o %t/lib-profiles.pcm
// RUN: %clang_cc1 -std=c++23 -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -fsyntax-only %t/use.cpp -fmodule-file=lib=%t/lib.pcm -verify
// RUN: %clang_cc1 -std=c++23 -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -fsyntax-only %t/use.cpp -fmodule-file=lib=%t/lib-profiles.pcm -verify
// RUN: %clang_cc1 -std=c++23 -emit-header-unit -xc++-user-header %t/std.h -o %t/std.pcm
// RUN: %clang_cc1 -std=c++23 -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -Wno-experimental-header-units -fsyntax-only %t/use-hu.cpp -fmodule-file=%t/std.pcm -verify

//--- std.h
namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);
} // namespace std

//--- lib.cppm
module;
#include "std.h"
export module lib;
export namespace std {
using std::construct_at;
using std::destroy_at;
using std::uninitialized_fill;
} // namespace std

//--- use.cpp
import lib;

// construct_at initializes its argument's storage
int construct() {
  int u [[uninit]];
  std::construct_at(&u, 1);
  return u;
}

// A second construct_at is a double construction
void construct_twice() {
  int u [[uninit]];
  std::construct_at(&u, 1);
  std::construct_at(&u, 2); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A second destroy_at is a double destruction
void destroy_twice() {
  int u [[uninit]];
  std::construct_at(&u, 1);
  std::destroy_at(&u);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// uninitialized_fill initializes the range
int fill() {
  int a [[uninit]] [2];
  std::uninitialized_fill(a, a + 2, 0);
  return a[0];
}

//--- use-hu.cpp
import "std.h";

// construct_at initializes its argument's storage
int construct() {
  int u [[uninit]];
  std::construct_at(&u, 1);
  return u;
}

// A second construct_at is a double construction
void construct_twice() {
  int u [[uninit]];
  std::construct_at(&u, 1);
  std::construct_at(&u, 2); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
