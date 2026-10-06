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
extern "C" void *malloc(std::size_t);
extern "C" void free(void *);

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

// A copy of a local copy-constructs its class-type member: the source member
// must be initialized on every path, and the copy's member is alive
void local_copy_destroyed_member() {
  DtorHolder h;
  std::construct_at(&h.m);
  std::destroy_at(&h.m);
  DtorHolder h2 = h; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&h.m);
}
void local_copy_constructed_member() {
  DtorHolder h;
  std::construct_at(&h.m);
  DtorHolder h2 = h; // OK
  std::construct_at(&h2.m); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
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

// A copy copy-constructs the member from the source's, which must be
// initialized on every path
int local_copy_constructed() {
  Holder h;
  std::construct_at(&h.inner, 1, 2);
  Holder c = h;
  return c.inner.x; // OK
}

int local_copy_unconstructed() {
  Holder h;
  Holder c = h; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  return c.inner.x; // OK
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

// Where no analysis tracks the member, reads and stores below it are trusted
struct Box {
  S s [[uninit]];
  int a [[uninit]] [4];
};
int through_pointer(Box *p) {
  p->s.x = 1;              // OK
  p->a[1] = 2;             // OK
  return p->s.y + p->a[1]; // OK
}

struct Global {
  S agg [[uninit]];
};
Global g;
int global_read() { return g.agg.x; } // OK

// A base with a user-provided constructor initializes its members
struct UserBase {
  S s [[uninit]];
  UserBase();
};
struct DerivedFromUser : UserBase {
  int r;
  DerivedFromUser() : r(s.x) {} // OK
};

// A default member initializer counts the marker of the member it reads
struct NsdmiRead {
  S s [[uninit]];
  int a [[uninit]] [4];
  int v = s.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  int w = a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  NsdmiRead() {}
};

// Taking the address, binding a pointer, and destroying still count the marker
void untracked_pins(Box *p) {
  int *px = &p->s.x;      // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  S *ps = &p->s;          // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::destroy_at(&p->s); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  (void)px;
  (void)ps;
}

// Storage reached through a marked pointer member or an allocator still counts
struct HoldsMarked {
  Global *q [[ref_to_uninit]];
  int read() { return q->agg.x; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
};
void allocated() {
  ((Global *)malloc(sizeof(Global)))->agg.x = 1; // expected-error {{writing a member of an '[[uninit]]' object does not initialize it under profile 'std::init'; initialize the whole object}}
}

// A copy of the member reads it, and is trusted where a read of it is
void take(S);
S copy_through_pointer(Box *h, S &t) {
  S c = h->s; // OK
  t = h->s;   // OK
  take(h->s); // OK
  return h->s; // OK
}

struct CopiesInMethod {
  S s [[uninit]];
  CopiesInMethod();
  S get() {
    S t = s; // OK
    return t;
  }
};

// A copy in a default member initializer counts the marker
struct NsdmiCopy {
  S s [[uninit]];
  S c = s; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  NsdmiCopy() {}
};

// A copy through a pointer to data member in a constructor reads every tracked member
struct CopiesThroughMemberPointer {
  S s [[uninit]];
  S t;
  CopiesThroughMemberPointer(S CopiesThroughMemberPointer::*pm) : t{1, 2} {
    S c = this->*pm; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    (void)c;
  }
};

// A reference binding still counts the marker, and a copy of an [[uninit]] local is judged by its flow
void copy_pins(Box *h) {
  const S &r = h->s; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  S u [[uninit]];
  S t = u; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)r;
  (void)t;
}

// Outside a constructor, destroying the current object ends its lifetime
struct Lifetime {
  int x = 0;
  int y [[uninit]];
  Lifetime();
  void reinitialize() {
    std::destroy_at(this);
    std::construct_at(this); // OK
  }
  int read_after_destroy() {
    std::destroy_at(this);
    return x; // expected-error {{read of a subobject of uninitialized storage accesses uninitialized memory under profile 'std::init'}}
  }
  int read_after_destructor_call() {
    this->~Lifetime();
    return x; // expected-error {{read of a subobject of uninitialized storage accesses uninitialized memory under profile 'std::init'}}
  }
  void store_after_destroy() {
    std::destroy_at(this);
    x = 1; // expected-error {{writing a member of uninitialized storage does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
  }
  void destroy_twice() {
    this->~Lifetime();
    this->~Lifetime(); // expected-error {{storage already destroyed by a destructor call is destroyed again under profile 'std::init'}}
  }
  void construct_live() {
    std::construct_at(this); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
  int read_live() {
    std::construct_at(&y, 1);
    return x + y; // OK
  }
};

// A raw-bytes credit through a conversion to a base reconstructs only the base
struct LifetimeBase {
  int b = 0;
};
[[now_init]] void fill_raw(void *p [[ref_to_uninit]]);
struct LifetimeDerived : LifetimeBase {
  int d = 0;
  LifetimeDerived();
  int reconstruct_base() {
    std::destroy_at(this);
    fill_raw(static_cast<LifetimeBase *>(this));
    return d; // expected-error {{read of a subobject of uninitialized storage accesses uninitialized memory under profile 'std::init'}}
  }
};

// A marked binding of the live current object is judged without a lifecycle call
struct ReturnsThis {
  int x = 0;
  ReturnsThis *self [[ref_to_uninit]] () {
    return this; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// The P4222R2 §4.6 Vector: a marked pointer member's referent is flow-tracked in member functions
struct VectorMemory {
  S *elem [[ref_to_uninit]];
  int n = 0;
  explicit VectorMemory(int c) : elem((S *)malloc(c * sizeof(S))) {}
  ~VectorMemory() { free(elem); }
};
struct Vector {
  VectorMemory mem;
  explicit Vector(int c) : mem(c) {}
  void push_back(const S &x) {
    std::construct_at(mem.elem + mem.n, x);
    ++mem.n;
  }
  ~Vector() { std::destroy(mem.elem, mem.elem + mem.n); } // OK
  void construct_then_destroy() {
    std::construct_at(mem.elem, 1, 2);
    std::destroy_at(mem.elem); // OK
  }
  void destroy_twice() {
    std::destroy_at(mem.elem);
    std::destroy_at(mem.elem); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
  }
  S &front() { return *mem.elem; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

// A const member function may destroy the referent
struct VectorConstPop {
  VectorMemory mem;
  explicit VectorConstPop(int c) : mem(c) {}
  void pop_all() const { std::destroy(mem.elem, mem.elem + mem.n); }
  void after_pop() {
    std::construct_at(mem.elem, 1, 2);
    pop_all();
    std::construct_at(mem.elem, 3, 4); // OK
  }
};

// A this capture or an alias of the current object may reseat the referent
struct AliasedElem {
  S *e [[ref_to_uninit]];
  AliasedElem();
  void through_capture() {
    std::construct_at(e);
    [this] { e = (S *)malloc(sizeof(S)); }();
    std::construct_at(e); // OK
  }
  void through_alias() {
    std::construct_at(e);
    AliasedElem *self = this;
    self->e = (S *)malloc(sizeof(S));
    std::construct_at(e); // OK
  }
  void through_marked_alias() {
    S *p [[ref_to_uninit]] = e;
    std::destroy_at(p); // OK
  }
};

// A default member initializer sets the marked pointer member
struct DefaultInitElem {
  S *e [[ref_to_uninit]] = (S *)malloc(sizeof(S));
  DefaultInitElem() { std::destroy_at(e); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
};

// A braced initializer of a member subobject sets the marked pointer member inside it
struct ElemHolder {
  S *e [[ref_to_uninit]];
};
struct ContainerInitElem {
  ElemHolder h;
  ContainerInitElem() : h{(S *)malloc(sizeof(S))} { std::destroy_at(h.e); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
};

template <class T> struct TVectorMemory {
  T *elem [[ref_to_uninit]];
  int n = 0;
  explicit TVectorMemory(int c) : elem((T *)malloc(c * sizeof(T))) {}
  ~TVectorMemory() { free(elem); }
};
template <class T> struct TVector {
  TVectorMemory<T> mem;
  explicit TVector(int c) : mem(c) {}
  ~TVector() { std::destroy(mem.elem, mem.elem + mem.n); } // OK
  void construct_then_destroy() {
    std::construct_at(mem.elem);
    std::destroy_at(mem.elem); // OK
  }
  void destroy_twice() {
    std::destroy_at(mem.elem);
    std::destroy_at(mem.elem); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
  }
};
template struct TVector<S>; // expected-note {{in instantiation of member function 'TVector<S>::destroy_twice' requested here}}
