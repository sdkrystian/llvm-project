// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c++23 %t/local.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c++23 %t/referent.cpp
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

// construct_at initializes u whole: a second construct_at is rejected by
// the reverse-direction rule.
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

// Calling through an explicit specialization initializes the same storage.
void explicit_specialization() {
  int u [[uninit]];
  std::construct_at<int>(&u, 5);
  int v = u; // OK
  (void)v;
}

//--- referent.cpp
// A marked local pointer or reference refers to the storage its latest
// binding names (ProfilesFramework.rst, "Binding Pointers and References"),
// so the lifecycle calls act on that storage under either name.
[[profiles::enforce(std::init)]];

namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
} // namespace std

struct Payload { int y; };

// construct_at through a marked pointer initializes the local it refers to.
void construct_through_pointer() {
  Payload x [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  std::construct_at(p, 5);
  int z = x.y; // OK: p refers to x
  (void)z;
}

// The same through a marked reference.
void construct_through_reference() {
  Payload x [[uninit]];
  Payload &r [[ref_to_uninit]] = x;
  std::construct_at(&r, 5);
  int z = x.y; // OK: r refers to x
  (void)z;
}

// construct_at on the local initializes what a marked pointer to it reads.
void construct_local_read_through_pointer() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  std::construct_at(&u, 5);
  int v = *p; // OK: u is initialized
  (void)v;
}

// A second construct_at under the other name is a double construction.
void double_construct_across_names() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  std::construct_at(p, 5);
  std::construct_at(&u, 6); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A second destroy_at under the other name is a double destruction.
void double_destroy_across_names() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  std::construct_at(&u, 5);
  std::destroy_at(p);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// destroy_at through the pointer destroys the local: never-constructed
// storage is rejected, and a subobject read after a destroy reads destroyed
// storage.
void destroy_through_pointer() {
  Payload x [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  std::destroy_at(p); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  std::construct_at(&x, 1);
  std::destroy_at(p);
  int z = x.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)z;
}

// A marked pointer copied from another refers to the same storage.
void pointer_chain() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  int *q [[ref_to_uninit]] = p;
  std::construct_at(q, 5);
  int v = *p;                    // OK: q and p refer to u
  int *r [[ref_to_uninit]] = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)v; (void)r;
}

// Reseating the pointer to a second local credits that local from then on.
void reseat_to_second_local() {
  Payload x [[uninit]], y [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  std::construct_at(p, 1);
  p = &y;
  std::construct_at(p, 2);
  int a = x.y; // OK
  int b = y.y; // OK
  (void)a; (void)b;
}

// A conditional initializer with tracked arms leaves the referent
// unidentified: construct_at through the pointer credits neither local, and
// nothing fires through the pointer.
void conditional_initializer(bool c) {
  int u [[uninit]], v [[uninit]];
  int *p [[ref_to_uninit]] = c ? &u : &v;
  std::construct_at(p, 5);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int r = *p;  // OK: unknown referent
  (void)q; (void)r;
}

// A pointer to a subobject has an anonymous referent: construct_at through
// it does not initialize the enclosing object.
void subobject_pointer() {
  Payload x [[uninit]];
  int *p [[ref_to_uninit]] = &x.y;
  std::construct_at(p, 5);
  int z = x.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)z;
}

// The wording follows the spelling of the read, not the storage it reaches.
void wording_by_spelling() {
  Payload x [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  int a = p->y; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int b = x.y;  // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)a; (void)b;
}

// A marked binding asserts the local it names uninitialized, whatever a
// conditional construct_at left it.
void construct_conditional_then_bind(bool c) {
  Payload x [[uninit]];
  if (c)
    std::construct_at(&x, 1);
  Payload *p [[ref_to_uninit]] = &x; // OK: constructed on one path only
  int z = x.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  std::construct_at(p, 2);
  int z2 = x.y; // OK
  (void)z; (void)z2;
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
  int v = u; // OK: initialized
  std::destroy_at(&u);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
  (void)v;
}
