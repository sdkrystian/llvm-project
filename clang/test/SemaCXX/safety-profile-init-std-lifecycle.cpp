// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c++23 %t/local.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -isystem %t/sys -std=c++23 %t/system.cpp
//
// The lifecycle attributes are injected only under -fprofiles (enforcement
// not required), and appear on both the pattern and its instantiations.
// RUN: %clang_cc1 -ast-dump -fprofiles -std=c++23 %t/dump.cpp | FileCheck %s --check-prefix=INJECT
// RUN: %clang_cc1 -ast-dump -std=c++23 %t/dump.cpp | FileCheck %s --check-prefix=PLAIN

// Clang attaches the std::init lifecycle markers to std::construct_at
// (RefToUninit on the first parameter + NowInit) and std::destroy_at
// (NowUninit) itself, keyed on a pointer-typed first parameter, so the real
// library functions are usable under enforcement.

// INJECT: FunctionDecl {{.*}} construct_at 'T *(T *, A &&...)'
// INJECT: RefToUninitAttr {{.*}} Implicit
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} construct_at 'int *(int *, int &&)' implicit_instantiation
// INJECT: RefToUninitAttr {{.*}} Implicit
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} destroy_at 'void (T *)'
// INJECT: NowUninitAttr {{.*}} Implicit

// PLAIN-NOT: RefToUninitAttr
// PLAIN-NOT: NowInitAttr
// PLAIN-NOT: NowUninitAttr

//--- dump.cpp
namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
} // namespace std

void instantiate(int *p) {
  std::construct_at(p, 5);
  std::destroy_at(p);
}

//--- local.cpp
[[profiles::enforce(std::init)]];

namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
// A non-pointer first parameter is outside the form key: not annotated.
template <class T> void construct_at(T &r, int);
} // namespace std

// A global-namespace construct_at is not annotated either.
template <class T, class... A> T *construct_at(T *p, A &&...args);

// The paper's central lifecycle idiom compiles clean.
void lifecycle() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  int v = u; // OK: the callee initialized u
  std::destroy_at(&u);
  (void)v;
}

// construct_at credits u whole: a second construct_at is rejected by the
// reverse-direction rule.
void double_construct() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  std::construct_at(&u, 6); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// destroy_at before any store destroys uninitialized storage.
void destroy_uninitialized() {
  int u [[uninit]];
  std::destroy_at(&u); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

// A second destroy_at is a double destruction.
void double_destroy() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  std::destroy_at(&u);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// Binding the destroyed storage is the unmarked-direction violation.
void destroy_then_bind() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  std::destroy_at(&u);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q;
}

// The global-namespace overload earns nothing: its first parameter is
// unmarked.
void global_ns_not_annotated() {
  int u [[uninit]];
  construct_at(&u, 5); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// The non-pointer-first-parameter std overload is likewise not annotated.
void non_pointer_p0_not_annotated() {
  int u [[uninit]];
  std::construct_at(u, 5); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Calling through an explicit specialization credits the same storage.
void explicit_specialization() {
  int u [[uninit]];
  std::construct_at<int>(&u, 5);
  int v = u; // OK
  (void)v;
}

//--- sys/init_mem.h
namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
} // namespace std

//--- system.cpp
// Declarations in a system header are annotated the same way, and the checks
// still fire: the violation is located at the user's call site, outside the
// system-header exemption.
[[profiles::enforce(std::init)]];
#include <init_mem.h>

void user() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  int v = u; // OK: credited
  std::destroy_at(&u);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
  (void)v;
}
