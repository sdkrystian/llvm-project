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
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/obligation.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/obligation.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/obligation.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/obligation.cpp

// std::init: a [[must_init]] parameter is a [[ref_to_uninit]] parameter whose
// storage the call initializes, and which the function's body initializes on
// every path that returns (ProfilesFramework.rst, "Binding Pointers and
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
void callee(X* p [[must_init]]) { } // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                    // expected-note {{declared here}}
void callee_some(X* p [[must_init]], bool c) { if (c) p->v = 1; } // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}} \
                                                                  // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                                  // expected-note {{declared here}}
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
[[now_init]] void bad_now_init(int* p [[ref_to_uninit]]) { } // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                            // expected-note {{declared here}}

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
void read_first(int *p [[must_init]]) { use(*p); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}} \
                                                   // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                   // expected-note {{declared here}}

// A member store does not initialize a class-type referent
void member_store(Agg *p [[must_init]]) { p->a = 1; } // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}} \
                                                      // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                      // expected-note {{declared here}}

//--- obligation.cpp
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
void plain(int *p [[ref_to_uninit]]);
void unmarked(int *p);
[[noreturn]] void stop();

// Initializing on every path that returns keeps the obligation
void every_path(int *p [[must_init]], bool c) {
  if (c)
    *p = 1;
  else
    std::construct_at(p, 2);
}
void early_return(Obj *p [[must_init]], bool c) {
  if (c) {
    std::construct_at(p, 1);
    return;
  }
  std::construct_at(p, 2);
}

// Initializing on one path only breaks it
void one_path(int *p [[must_init]], bool c) { // expected-note {{declared here}}
  if (c)
    *p = 1;
} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}}

// Each obligated parameter is judged
void two(int *a [[must_init]], int *b [[must_init]]) { // expected-note {{declared here}}
  *a = 1;
} // expected-error {{storage referred to by parameter 'b' is not initialized on every path that returns under profile 'std::init'}}

// A reference parameter is judged by the object it binds
void reference(int &r [[must_init]], bool c) { // expected-note {{declared here}}
  if (c)
    r = 1;
} // expected-error {{storage referred to by parameter 'r' is not initialized on every path that returns under profile 'std::init'}}

// A handle parameter is judged by the range it refers to
void span_filled(std::span<int> s [[must_init]]) {
  std::ranges::uninitialized_fill(s, 0);
}
void span_unfilled(std::span<int> s [[must_init]]) {} // expected-error {{storage referred to by parameter 's' is not initialized on every path that returns under profile 'std::init'}} \
                                                      // expected-note {{declared here}}

// A byte writer initializes a void pointer's referent
void bytes(void *p [[must_init]], std::size_t n) { memset(p, 0, n); }

// Each marked parameter of a [[now_init]] function is obligated
[[now_init]] void now_init_marked(int *a [[ref_to_uninit]], int *b, int *c [[ref_to_uninit]]) { // expected-note {{declared here}}
  *a = 1;
} // expected-error {{storage referred to by parameter 'c' is not initialized on every path that returns under profile 'std::init'}}

// An unnamed parameter can never be initialized
void unnamed([[must_init]] int *) {} // expected-error {{storage referred to by an unnamed parameter is not initialized on every path that returns under profile 'std::init'}} \
                                     // expected-note {{declared here}}

// The storage the parameter refers to at entry is judged, not where it points later
void store_then_reseat(int *p [[must_init]]) {
  *p = 1;
  p = nullptr;
}
void stores_advance(int *p [[must_init]], int n) { // expected-note {{declared here}}
  for (int i = 0; i < n; ++i)
    *p++ = 0;
} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}}
void reseat_then_store(int *p [[must_init]], int *q [[ref_to_uninit]]) { // expected-note {{declared here}}
  p = q;
  *p = 1;
} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}}

// A marked alias of the parameter initializes the same storage
void through_alias(int *p [[must_init]]) {
  int *q [[ref_to_uninit]] = p;
  *q = 1;
}

// Passing the parameter on initializes only through a [[must_init]] callee
void passed_to_must_init(int *p [[must_init]]) { fill_int(p); }
void passed_to_marked(int *p [[must_init]]) { plain(p); } // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                          // expected-note {{declared here}}
void passed_to_unmarked(int *p [[must_init]]) { unmarked(p); } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}} \
                                                               // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                               // expected-note {{declared here}}

// A destroy leaves the storage uninitialized again
void constructed_then_destroyed(Obj *p [[must_init]]) { // expected-note {{declared here}}
  std::construct_at(p, 1);
  std::destroy_at(p);
} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}}

// An element store initializes nothing
void element_loop(int *p [[must_init]], int n) { // expected-note {{declared here}}
  for (int i = 0; i < n; ++i)
    p[i] = 0;
} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}}

// A null-checked parameter is not initialized on the null path
void null_checked(int *p [[must_init]]) { // expected-note {{declared here}}
  if (!p)
    return;
  *p = 1;
} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}}

// A mutable alias of the parameter hides what initializes its storage
void by_reference_capture(int *p [[must_init]]) { // expected-note {{declared here}}
  auto l = [&] { *p = 1; };
  l();
} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}}

// A path that throws or does not return is not judged
void throws(int *p [[must_init]], bool c) {
  if (c)
    throw 1;
  *p = 1;
}
void always_throws(int *p [[must_init]]) { throw 1; }
void no_return(int *p [[must_init]]) { stop(); }
void endless(int *p [[must_init]]) {
  while (true) {
  }
}

// A function template is judged per specialization
template <class T> void template_unfilled(T *p [[must_init]]) {} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                                 // expected-note {{declared here}}
template void template_unfilled<int>(int *); // expected-note {{in instantiation of function template specialization 'template_unfilled<int>' requested here}}

// A lambda's parameter is obligated
auto lambda_unfilled = [](int *p [[must_init]]) {}; // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                                                    // expected-note {{declared here}}

// A definition inherits the obligation of an earlier declaration
struct Filler {
  void fill(int *p [[must_init]]);
  virtual void fill_virtual(int *p [[must_init]]);
};
void Filler::fill(int *p) {} // expected-error {{storage referred to by parameter 'p' is not initialized on every path that returns under profile 'std::init'}} \
                             // expected-note {{declared here}}

// An override that does not repeat the marker is not obligated
struct DerivedFiller : Filler {
  void fill_virtual(int *p) override {}
};

// A marker added after the definition does not obligate it
void marked_late(int *p) {}
void marked_late(int *p [[must_init]]);

// The rule can be suppressed
[[profiles::suppress(std::init, rule: "must_init")]] void suppressed(int *p [[must_init]]) {} // no-profiles-warning {{'profiles::suppress' attribute ignored}}

// A loop that fills the same storage twice constructs over it
void fill_agg(Agg *p [[must_init]]);
void loop_reuse(int n) {
  Agg g [[uninit]];
  for (int i = 0; i < n; ++i) {
    fill_agg(&g); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
    use(g.a);
  }
}
void loop_fresh(int n) {
  for (int i = 0; i < n; ++i) {
    Agg g [[uninit]];
    fill_agg(&g);
    use(g.a);
  }
}
