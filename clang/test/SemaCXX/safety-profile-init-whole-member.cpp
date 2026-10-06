// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wno-uninitialized -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// The LEADING_ERROR run adds a leading unrelated error so every later function
// is analyzed through the post-error path.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wno-uninitialized -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// -Wunreachable-code makes the main path build a fully linearized CFG.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wno-uninitialized -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp

// std::init: an [[uninit]] member of a class type is flow-tracked as a whole
// object wherever a scalar [[uninit]] member is.

//--- sys/std_init.h
namespace std {
using size_t = decltype(sizeof 0);
template <class T, class... A> constexpr T *construct_at(T *p, A &&...args);
template <class T> constexpr void destroy_at(T *p);
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);
template <class It> constexpr void destroy(It first, It last);
} // namespace std
extern "C" void *memset(void *, int, std::size_t);

//--- test.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <std_init.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

struct S {
  int x;
  int y;
  void f();
};

// In a constructor body the member is uninitialized until initialized whole
struct Before {
  S s [[uninit]];
  int r;
  Before() : r(0) {
    r = s.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
    s.y = 1; // expected-error {{writing a member of an '[[uninit]]' object does not initialize it under profile 'std::init'; initialize the whole object}}
    S t = s; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    s = S{}; // expected-error {{calling member function 'operator=' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
    (void)t;
  }
};

// construct_at initializes the member
struct After {
  S s [[uninit]];
  int r;
  After() : r(0) {
    std::construct_at(&s, 1, 2);
    r = s.x; // OK
    s.y = 1; // OK
    S t = s; // OK
    s = S{}; // OK
    (void)t;
  }
};

// A construct on one path does not initialize the member at the join
struct OnePath {
  S s [[uninit]];
  int r;
  OnePath(bool c) : r(0) {
    if (c)
      std::construct_at(&s, 1, 2);
    r = s.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  }
};

// A second construct binds initialized storage to a marked parameter
struct Twice {
  S s [[uninit]];
  Twice() {
    std::construct_at(&s, 1, 2);
    std::construct_at(&s, 3, 4); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// A written initializer, a [[now_init]] call, and memset initialize the member
[[now_init]] void fill(S *p [[ref_to_uninit]]);
struct ByInitializer {
  S s [[uninit]];
  int r;
  ByInitializer() : s{1, 2}, r(s.x) {} // OK
};
struct ByNowInit {
  S s [[uninit]];
  int r;
  ByNowInit() : r(0) {
    fill(&s);
    r = s.x; // OK
  }
};
struct ByMemset {
  S s [[uninit]];
  int r;
  ByMemset() : r(0) {
    memset(&s, 0, sizeof s);
    r = s.x; // OK
  }
};

// Destroying the member leaves it uninitialized
struct DestroyRead {
  S s [[uninit]];
  int r;
  DestroyRead() : r(0) {
    std::construct_at(&s, 1, 2);
    std::destroy_at(&s);
    r = s.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  }
};
struct DestroyTwice {
  S s [[uninit]];
  DestroyTwice() {
    std::construct_at(&s, 1, 2);
    std::destroy_at(&s);
    std::destroy_at(&s); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
  }
};

// A call inside a try block may transfer to the handler before the construct
S make();
struct TryConstruct {
  S s [[uninit]];
  int r;
  TryConstruct() : r(0) {
    try {
      std::construct_at(&s, make());
    } catch (...) {
    }
    r = s.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  }
};

// A this-capturing lambda reads the member where the lambda is created
struct ThisLambda {
  S s [[uninit]]; // expected-note {{member 's' declared here}}
  int r;
  ThisLambda() : r(0) {
    auto l = [this] { return s.x; }; // expected-error {{member 's' is read before initialization under profile 'std::init'}}
    std::construct_at(&s, 1, 2);
    auto m = [this] { return s.x; }; // OK
    r = l() + m();
  }
};

// A *this capture copies every member where the lambda is created
struct StarThisLambda {
  S s [[uninit]]; // expected-note {{member 's' declared here}}
  int r;
  StarThisLambda() : r(0) {
    auto l = [*this] { return s.x; }; // expected-error {{member 's' is read before initialization under profile 'std::init'}}
    r = l();
  }
};

// Outside a constructor, reads and stores below the member are trusted, while
// its address, a reference to it, a member call on it, and its destruction are
// judged from an uninitialized start
struct Methods {
  S s [[uninit]];
  Methods();
  int read() { return s.x; } // OK
  void store() { s.y = 1; }  // OK
  S *address() { return &s; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  S &reference() { return s; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  void call() { s.f(); } // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  void destroy() { std::destroy_at(&s); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
};

// An aggregate local's member is tracked the same way
struct Holder {
  S inner [[uninit]];
  int k = 0;
};
int local_read_before() {
  Holder h;
  return h.inner.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

int local_construct_then_read() {
  Holder h;
  std::construct_at(&h.inner, 1, 2);
  return h.inner.x; // OK
}

// An escape of the local trusts later accesses below the member
void take_holder(Holder *);
int local_escape() {
  Holder h;
  take_holder(&h);
  return h.inner.x; // OK
}

// The scope exit of an aggregate local destroys a member whose destructor is non-trivial
struct DtorMember {
  int x;
  ~DtorMember();
};
struct DtorHolder {
  DtorMember m [[uninit]];
  int k = 0;
};
void local_scope_exit_never_constructed() {
  DtorHolder h; // expected-note {{'h' is destroyed at the end of its scope}}
} // expected-error {{uninitialized storage is destroyed by its scope-exit destructor under profile 'std::init'}}
void local_scope_exit_destroyed() {
  DtorHolder h; // expected-note {{'h' is destroyed at the end of its scope}}
  std::construct_at(&h.m);
  std::destroy_at(&h.m);
} // expected-error {{storage already destroyed by its scope-exit destructor is destroyed again under profile 'std::init'}}
void local_scope_exit_constructed() {
  DtorHolder h;
  std::construct_at(&h.m); // OK
}

// The scope exit of an escaped local destroys a member of unknown state
void take_dtor_holder(DtorHolder *);
void local_scope_exit_escaped() {
  DtorHolder h;
  take_dtor_holder(&h); // OK
}

// The scope exit of a local that escapes after its member's destroy destroys a
// member of unknown state
void local_scope_exit_escaped_after_destroy() {
  DtorHolder h;
  std::construct_at(&h.m);
  std::destroy_at(&h.m);
  take_dtor_holder(&h); // OK
}

// A member whose class has a converting constructor is tracked as a whole object
struct Converting {
  int x;
  int y;
  Converting() = default;
  Converting(int a, int b) : x(a), y(b) {}
};
struct ConvertingHolder {
  Converting c [[uninit]];
  int k = 0;
};
int local_converting_read() {
  ConvertingHolder h;
  return h.c.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A copy carries the member's state
int local_copy_constructed() {
  Holder h;
  std::construct_at(&h.inner, 1, 2);
  Holder c = h;
  return c.inner.x; // OK
}

int local_copy_unconstructed() {
  Holder h;
  Holder c = h;
  return c.inner.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A by-value parameter's member starts uninitialized
int parameter_read(Holder p) {
  return p.inner.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// An [[uninit]] array member is tracked as a whole object too
struct ArrayFill {
  S arr [[uninit]] [2];
  int r;
  ArrayFill() : r(0) {
    std::uninitialized_fill(arr, arr + 2, S{});
    r = arr[0].x; // OK
  }
};

// An element store does not initialize the array
struct ArrayStoreBefore {
  int a [[uninit]] [4];
  ArrayStoreBefore() {
    a[0] = 1; // expected-error {{writing an element of an '[[uninit]]' object does not initialize it under profile 'std::init'; initialize the whole object}}
    std::uninitialized_fill(a, a + 4, 0);
  }
};

// memset initializes the whole array
struct ArrayMemset {
  int a [[uninit]] [4];
  int r;
  ArrayMemset() : r(0) {
    memset(a, 0, sizeof a);
    r = a[3]; // OK
  }
};

// std::destroy leaves the array uninitialized
struct ArrayDestroy {
  S arr [[uninit]] [2];
  int r;
  ArrayDestroy() : r(0) {
    std::uninitialized_fill(arr, arr + 2, S{});
    std::destroy(arr, arr + 2);
    r = arr[1].y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  }
};

// Constructing one element initializes nothing
struct ArrayElementConstruct {
  S arr [[uninit]] [2];
  int r;
  ArrayElementConstruct() : r(0) {
    std::construct_at(&arr[0], 1, 2);
    r = arr[0].x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  }
};

// Outside a constructor, element reads and stores are trusted
struct ArrayMethods {
  int a [[uninit]] [4];
  ArrayMethods();
  int read() { return a[1]; } // OK
  void store() { a[2] = 1; }  // OK
};
