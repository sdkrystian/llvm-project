// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/p15.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/p15.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/p15.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/p15.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/callers.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/callers.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/callers.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/callers.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/callees.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/callees.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/callees.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/callees.cpp

// std::init: a [[must_init]] parameter is a [[ref_to_uninit]] parameter whose
// storage the call initializes (ProfilesFramework.rst, "Binding Pointers and
// References").

//--- sys/must_init.h
// The shapes of libstdc++'s <memory>, <cstring>, and <span> the tests use.
namespace std {
using size_t = decltype(sizeof 0);
template <class T, class... A> constexpr T *construct_at(T *p, A &&...args);
template <class T> constexpr void destroy_at(T *p);
template <class It, class Size, class T>
It uninitialized_fill_n(It first, Size n, const T &x);
template <class T> struct span {
  T *data() const;
  size_t size() const;
  T *begin() const;
  T *end() const;
};
namespace ranges {
struct __uninitialized_fill_fn {
  template <class R, class T> R &&operator()(R &&r, const T &x) const;
};
inline constexpr __uninitialized_fill_fn uninitialized_fill{};
} // namespace ranges
} // namespace std
extern "C" void *memset(void *, int, std::size_t);

//--- p15.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

// The [[must_init]] example of P4222R2 §6.2
struct X { int v; };
void f(X* p [[must_init]]);
void t1()
{
    X x [[uninit]];
    f(&x);
    X z = x;
    (void)z;
}
void t2()
{
    X y = { 7 };
    f(&y); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
void callee(X* p [[must_init]]) { }
void callee_some(X* p [[must_init]], bool c) { if (c) p->v = 1; } // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
void mixed(int* a [[must_init]], int* b [[ref_to_uninit]]);
int t3()
{
    int a [[uninit]];
    int b [[uninit]];
    mixed(&a, &b);
    return a + b;
}
[[now_init]] void fill2(int* a [[ref_to_uninit]], int* b [[ref_to_uninit]]);
int t4()
{
    int a [[uninit]];
    int b [[uninit]];
    fill2(&a, &b);
    return a + b;
}
[[now_init]] void bad_now_init(int* p [[ref_to_uninit]]) { }

//--- callers.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <must_init.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

struct Agg { int a; int b; };
struct Obj { Obj(); ~Obj(); int v; };
void fill_int(int *p [[must_init]]);
void fill_ref(int &r [[must_init]]);
void fill_agg(Agg *p [[must_init]]);
void fill_obj(Obj *p [[must_init]]);
void fill_bytes(void *p [[must_init]]);
void fill_array(int n, int *p [[must_init]]);
void fill_span(std::span<int> s [[must_init]]);
template <class T> void fill_any(T *p [[must_init]]);
void fill_redecl(int *p [[must_init]]);
void fill_redecl(int *p);
struct Filler {
  void fill(int *p [[must_init]]);
  static void make(Agg *p [[must_init]]);
};

// A call initializes a scalar the argument points to
int scalar() {
  int a [[uninit]];
  fill_int(&a);
  return a;
}

// A call initializes the object a reference parameter binds
int reference() {
  int a [[uninit]];
  fill_ref(a);
  return a;
}

// A call initializes an aggregate whole
int aggregate() {
  Agg g [[uninit]];
  fill_agg(&g);
  return g.a + g.b;
}

// A call constructs a class object, which its scope exit then destroys
void constructs(Obj *p [[ref_to_uninit]]) {
  fill_obj(p);
  std::destroy_at(p);
}

// A void pointer parameter claims the bytes of any storage
int raw_bytes() {
  Agg g [[uninit]];
  fill_bytes(&g);
  return g.b;
}

// A call initializes an array the argument decays from whole
int whole_array() {
  int a [[uninit]] [4];
  fill_array(4, a);
  return a[1];
}

// A call through a marked pointer initializes its referent
int through_marker(int *q [[ref_to_uninit]]) {
  fill_int(q);
  return *q;
}

// A function template's specialization credits its argument
int template_callee() {
  Agg g [[uninit]];
  fill_any(&g);
  return g.a;
}

// A lambda's parameter credits its argument
int lambda_callee() {
  auto l = [](int *p [[must_init]]) { *p = 1; };
  int a [[uninit]];
  l(&a);
  return a;
}

// A member function and a static member function credit their arguments
int member_callee(Filler &f) {
  int a [[uninit]];
  f.fill(&a);
  Agg g [[uninit]];
  Filler::make(&g);
  return a + g.b;
}

// A marker on an earlier declaration credits a call of a later one
int redeclared() {
  int a [[uninit]];
  fill_redecl(&a);
  return a;
}

// An argument that is initialized is rejected
void initialized_argument() {
  int a = 0;
  fill_int(&a); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A second call may construct over the object the first one constructed
void called_twice() {
  Agg g [[uninit]];
  fill_agg(&g);
  fill_agg(&g); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A call on some path only initializes nothing on the other
int some_path(bool c) {
  Agg g [[uninit]];
  if (c)
    fill_agg(&g);
  return g.a; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

//--- callees.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <must_init.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

struct Agg { int a; int b; };
struct Obj { Obj(int); ~Obj(); int v; };
void use(int);
void fill_int(int *p [[must_init]]);

// std::construct_at initializes the parameter's storage
void by_construct(Obj *p [[must_init]]) { std::construct_at(p, 1); }

// memset initializes the parameter's storage
void by_memset(Agg *p [[must_init]]) { memset(p, 0, sizeof *p); }

// std::uninitialized_fill_n initializes the parameter's storage
void by_fill(int *p [[must_init]], int n) { std::uninitialized_fill_n(p, n, 0); }

// A range fill initializes a span parameter's storage
void by_span(std::span<int> s [[must_init]]) { std::ranges::uninitialized_fill(s, 0); }

// A store initializes the parameter's storage
void by_store(int *p [[must_init]]) { *p = 1; }
void by_reference(int &r [[must_init]]) { r = 1; }

// A [[must_init]] callee initializes the parameter's storage
void by_forward(int *p [[must_init]]) { fill_int(p); }

// A function template and a lambda judge their bodies the same way
template <class T> void by_template(T *p [[must_init]]) { std::construct_at(p); }
template void by_template<int>(int *);
auto by_lambda = [](int *p [[must_init]]) { *p = 2; };

// The parameter's storage is uninitialized at entry
void read_first(int *p [[must_init]]) { use(*p); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}

// A member store does not initialize a class-type referent
void member_store(Agg *p [[must_init]]) { p->a = 1; } // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
