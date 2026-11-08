// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -fblocks -isystem %t/sys %t/members.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -fblocks -isystem %t/sys %t/members.cpp
// The LEADING_ERROR run adds a leading unrelated error so every later function
// is analyzed through the post-error path.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -fblocks -isystem %t/sys %t/members.cpp
// -Wunreachable-code makes the main path build a fully linearized CFG.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -fblocks -isystem %t/sys %t/members.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/locals.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/locals.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/locals.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/locals.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/submembers.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/submembers.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/submembers.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/submembers.cpp

// std::init: ordinary storage a lifecycle call of a body names -- a member of
// the current object, a local, a by-value parameter, or a member below
// tracked storage -- has a lifetime of its own in that body
// (ProfilesFramework.rst, "Binding Pointers and References"): destroying it
// twice, using it after a destroy, and constructing over it while it is live
// are rejected, and destroying then reconstructing it is accepted.

//--- sys/lifetimes.h
// The shapes of libstdc++'s <memory>, <new>, and <string> the tests use.
namespace std {
using size_t = decltype(sizeof 0);
template <class T> constexpr T *addressof(T &r) noexcept;
template <class T, class... A> constexpr T *construct_at(T *p, A &&...args);
template <class T> constexpr void destroy_at(T *p);
template <class FwdIt> constexpr void destroy(FwdIt first, FwdIt last);
template <class FwdIt, class Size> constexpr FwdIt destroy_n(FwdIt first, Size n);
template <class T> T *now_init(T *p);
template <class T> struct allocator {
  T *allocate(size_t n);
  void deallocate(T *p, size_t n);
};
template <class A> struct allocator_traits {
  template <class U, class... Args>
  static constexpr void construct(A &a, U *p, Args &&...args);
  template <class U> static constexpr void destroy(A &a, U *p);
};
namespace ranges {
struct __destroy_at_fn {
  template <class T> constexpr void operator()(T *p) const noexcept;
};
inline constexpr __destroy_at_fn destroy_at{};
struct __destroy_fn {
  template <class I, class S> constexpr I operator()(I first, S last) const noexcept;
  template <class R> constexpr R &operator()(R &&r) const noexcept;
};
inline constexpr __destroy_fn destroy{};
} // namespace ranges
class string {
public:
  string();
  string(const char *s);
  string(const string &o);
  string &operator=(const char *s);
  string &operator=(const string &o);
  ~string();
  size_t size() const;

private:
  char *p_;
  size_t n_;
};
} // namespace std
void *operator new(std::size_t, void *p) noexcept;

//--- members.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <lifetimes.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

void take(const std::string &s);
void take_int(int i);
std::size_t use(std::string s);

struct A {
  std::string s;
  int i;
  std::string arr[2];
  std::allocator<std::string> al;

  void destroy_twice();
  void destroy_then_call();
  void destroy_then_assign();
  void destroy_then_copy();
  void destroy_then_bind();
  void destroy_then_construct();
  void destructor_call_then_placement();
  void construct_live();
  void placement_live();
  void destroy_one_path(bool c);
  void destroy_construct_loop(int n);
  void destroy_spellings();
  void destroy_range_spellings();
  void scalar_destroy_construct();
  int scalar_read_after_destroy();
  int scalar_store_after_destroy();
  void addressof_destroy_construct();
  void marked_binding_live();
  void marked_binding_destroyed();
  void lambda_destroy();
  void lambda_destroy_twice();
  void lambda_construct_live();
  void block_construct_live();
};

// A member destroyed twice in one body is a double destruction
void A::destroy_twice() {
  std::destroy_at(&s);
  std::destroy_at(&s); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
  std::construct_at(&s);
}

// A member call on a destroyed member binds its object to uninitialized memory
void A::destroy_then_call() {
  std::destroy_at(&s);
  (void)s.size(); // expected-error {{calling member function 'size' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  std::construct_at(&s);
}

// An assignment to a destroyed class-type member is a member call on uninitialized memory
void A::destroy_then_assign() {
  std::destroy_at(&s);
  s = "b"; // expected-error {{calling member function 'operator=' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  std::construct_at(&s);
}

// A copy of a destroyed member reads uninitialized memory
void A::destroy_then_copy() {
  std::destroy_at(&s);
  (void)use(s); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&s);
}

// A reference bound to a destroyed member refers to uninitialized memory
void A::destroy_then_bind() {
  std::destroy_at(&s);
  take(s); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&s);
}

// Destroying and reconstructing a member is accepted
void A::destroy_then_construct() {
  std::destroy_at(&s);
  std::construct_at(&s, "b");
  take(s);
}

// A destructor call and a placement new reconstruct a member
void A::destructor_call_then_placement() {
  s.~string();
  new (&s) std::string("b");
  take(s);
}

// std::construct_at on a live member constructs over a live object
void A::construct_live() {
  std::construct_at(&s, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A placement new on a live member constructs over a live object
void A::placement_live() {
  new (&s) std::string("b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A construction after a destroy on one path constructs over the object the other path kept
void A::destroy_one_path(bool c) {
  if (c)
    std::destroy_at(&s);
  std::construct_at(&s, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// Destroying and reconstructing a member in a loop is accepted
void A::destroy_construct_loop(int n) {
  for (int k = 0; k != n; ++k) {
    std::destroy_at(&s);
    std::construct_at(&s, "b");
  }
}

// Each destroy spelling destroys the member
void A::destroy_spellings() {
  std::ranges::destroy_at(&s);
  std::construct_at(&s);
  std::allocator_traits<std::allocator<std::string>>::destroy(al, &s);
  std::construct_at(&s);
  s.~string();
  std::destroy_at(&s); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
  std::construct_at(&s);
}

// std::destroy and std::destroy_n destroy an array member whole
void A::destroy_range_spellings() {
  std::destroy(arr, arr + 2);
  std::destroy_n(arr, 2); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
}

// A scalar member destroyed and reconstructed is accepted
void A::scalar_destroy_construct() {
  std::destroy_at(&i);
  std::construct_at(&i, 2);
  take_int(i);
}

// A read of a destroyed scalar member reads uninitialized memory
int A::scalar_read_after_destroy() {
  std::destroy_at(&i);
  return i; // expected-error {{read of a subobject of uninitialized storage accesses uninitialized memory under profile 'std::init'}}
}

// A store to a destroyed scalar member initializes it
int A::scalar_store_after_destroy() {
  std::destroy_at(&i);
  i = 2;
  return i;
}

// std::addressof names the member as & does
void A::addressof_destroy_construct() {
  std::destroy_at(std::addressof(s));
  std::construct_at(std::addressof(this->s), "b"); // OK
  std::construct_at(std::addressof(this->s), "c"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A marked pointer bound to a live member is rejected
void A::marked_binding_live() {
  std::string *p [[ref_to_uninit]] = &s; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)p;
}

// A marked pointer bound to a destroyed member is accepted, and constructing through it reconstructs the member
void A::marked_binding_destroyed() {
  std::destroy_at(&s);
  std::string *p [[ref_to_uninit]] = &s;
  std::construct_at(p, "b");
  take(s);
}

// A lambda's body judges a member by form, which accepts its destroy
void A::lambda_destroy() {
  [this] { std::destroy_at(&s); }();
}

// A lambda's body judges a member by form, which accepts a second destroy
void A::lambda_destroy_twice() {
  [this] {
    std::destroy_at(&s);
    std::destroy_at(&s);
  }();
}

// A lambda's body judges a member by form, which rejects a construction over it
void A::lambda_construct_live() {
  [this] { std::construct_at(&s, "b"); }(); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A block's body judges a member by form, which rejects a construction over it
void A::block_construct_live() {
  ^{ std::construct_at(&s, "b"); }(); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

struct TeardownDestroyed {
  std::string s;
  // A destructor that leaves a member destroyed destroys it again at its member teardown
  ~TeardownDestroyed() {
    std::destroy_at(&s);
  } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

struct TeardownReconstructed {
  std::string s;
  // A destructor that destroys and reconstructs a member is accepted
  ~TeardownReconstructed() {
    std::destroy_at(&s);
    std::construct_at(&s);
  }
};

struct TeardownOnePath {
  std::string s;
  bool c;
  // A destructor that destroys a member on one path destroys it again there at its member teardown
  ~TeardownOnePath() {
    if (c)
      std::destroy_at(&s);
  } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

struct TrivialCtor { int v; ~TrivialCtor(); };
struct TeardownUninitMember {
  TrivialCtor t [[uninit]];
  // A tracked [[uninit]] member destroyed in a destructor is destroyed again at its member teardown
  ~TeardownUninitMember() {
    std::destroy_at(&t); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

struct TeardownScalar {
  int i;
  // A scalar member has no member teardown
  ~TeardownScalar() { std::destroy_at(&i); }
};

struct TeardownNowInit {
  std::string d;
  // A destroy through std::now_init's result does not reach the member, whose live state now_init's argument rejects
  ~TeardownNowInit() {
    std::destroy_at(std::now_init(&d)); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

struct BaseWithMember { std::string bs; };
struct DerivedOfMember : BaseWithMember {
  // A member of a base class is the current object's member
  void destroy_base_member_twice() {
    std::destroy_at(&bs);
    std::destroy_at(&bs); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
    std::construct_at(&bs);
  }
};

struct StrView {
  std::string *first, *last;
  std::string *begin() const;
  std::string *end() const;
  std::size_t size() const;
};
struct ViewHolder {
  StrView v;
  // A range destroy of a member view destroys the view's elements, not the member
  void range_destroy() {
    std::ranges::destroy(v);
    (void)v.size();
  }
};

union UnionClass {
  std::string s;
  int i;
  UnionClass() {}
  ~UnionClass() {}
  // A member of a union class has no lifetime of its own
  void destroy_twice() {
    std::destroy_at(&s);
    std::destroy_at(&s);
  }
};

struct WithAnonymousUnion {
  union {
    std::string s;
    int i;
  };
  WithAnonymousUnion();
  ~WithAnonymousUnion();
  // A member of an anonymous union has no lifetime of its own
  void destroy_twice() {
    std::destroy_at(&s);
    std::destroy_at(&s);
  }
};

struct WithReference {
  std::string &r;
  // A reference member has no lifetime of its own
  void destroy_twice() {
    std::destroy_at(&r);
    std::destroy_at(&r);
  }
};

struct WithRawStorage {
  alignas(std::string) unsigned char buf[sizeof(std::string)];
  // A raw-storage member has no lifetime of its own, and a placement new into it is not judged
  void place_twice() {
    new (buf) std::string("a");
    new (buf) std::string("b");
  }
};

//--- constructors.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <lifetimes.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

void take(const std::string &s);

// A construct_at over a member its mem-initializer constructed constructs over a live object
struct MemInit {
  std::string s;
  MemInit() : s("a") {
    std::construct_at(&s, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// A construct_at over a member its default member initializer constructed constructs over a live object
struct DefaultInit {
  std::string s = "a";
  DefaultInit() {
    std::construct_at(&s, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// A construct_at over an implicitly default-constructed member constructs over a live object
struct ImplicitInit {
  std::string s;
  ImplicitInit() {
    std::construct_at(&s, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// std::addressof(this->s) names the member as &s does
struct AddressofInit {
  std::string s;
  AddressofInit() : s("a") {
    std::construct_at(std::addressof(this->s), "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// A construct_at over a scalar member its default member initializer initialized constructs over a live object
struct ScalarDefault {
  int i = 0;
  ScalarDefault() {
    std::construct_at(&i, 1); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// A placement new over a mem-initialized member constructs over a live object
struct PlacementInit {
  std::string s;
  PlacementInit() : s("a") {
    new (&s) std::string("b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

struct InitBase {
  std::string b;
  InitBase() : b("x") {}
};

// A construct_at over a base its initializer constructed constructs over a live object
struct DerivedInit : InitBase {
  DerivedInit() : InitBase() {
    std::construct_at(static_cast<InitBase *>(this)); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// A construct_at over an implicitly constructed base constructs over a live object
struct DerivedImplicit : InitBase {
  DerivedImplicit() {
    std::construct_at(static_cast<InitBase *>(this)); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// Destroying and reconstructing a base is accepted
struct DerivedRebuild : InitBase {
  DerivedRebuild() {
    std::destroy_at(static_cast<InitBase *>(this));
    std::construct_at(static_cast<InitBase *>(this));
  }
};

// A base destroyed twice through a reference conversion is a double destruction
struct DerivedDestroyTwice : InitBase {
  DerivedDestroyTwice() {
    std::destroy_at(std::addressof(static_cast<InitBase &>(*this)));
    std::destroy_at(std::addressof(static_cast<InitBase &>(*this))); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
    std::construct_at(static_cast<InitBase *>(this));
  }
};

// A lambda's body in a constructor judges a member by form, which leaves the object under construction unclassified
struct LambdaInCtor {
  std::string s;
  LambdaInCtor() : s("a") {
    [this] { std::construct_at(&s, "b"); }();
  }
};

// A delegating constructor's target constructs every member
struct Delegating {
  std::string s;
  Delegating() : s("a") {}
  Delegating(int) : Delegating() {
    std::construct_at(&s, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// Destroying and reconstructing a mem-initialized member is accepted
struct Rebuild {
  std::string s;
  Rebuild() : s("a") {
    std::destroy_at(&s);
    std::construct_at(&s, "b");
    take(s);
  }
};

// A construct_at after a destroy on one path constructs over the object the other path kept
struct RebuildOnePath {
  std::string s;
  RebuildOnePath(bool c) : s("a") {
    if (c)
      std::destroy_at(&s);
    std::construct_at(&s, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  }
};

// A marked member initialized with the address of a member declared before it refers to a live object
struct MarkedBefore {
  std::string b;
  std::string *p [[ref_to_uninit]];
  MarkedBefore() : b("x"), p(&b) {} // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
};

// A marked member initialized with the address of a member declared after it refers to uninitialized memory
struct MarkedAfter {
  std::string *p [[ref_to_uninit]];
  std::string b;
  MarkedAfter() : p(&b), b("x") {}
};

// A member used by an earlier member's initializer is used before its own initializer constructs it
struct EarlyUse {
  std::size_t n;
  std::string b;
  EarlyUse() : n(b.size()), b("x") { // expected-error {{calling member function 'size' binds its implicit object parameter to uninitialized memory under profile 'std::init'}} \
                                     // expected-warning {{field 'b' is uninitialized when used here}} \
                                     // no-profiles-warning {{field 'b' is uninitialized when used here}}
    std::destroy_at(&b);
    std::construct_at(&b);
  }
};

// A constructor that leaves a member destroyed hands the object's destructor a destroyed member
struct LeftDestroyed {
  std::string s;
  LeftDestroyed() : s("a") { std::destroy_at(&s); } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

// A constructor that destroys and reconstructs a member is accepted
struct LeftReconstructed {
  std::string s;
  LeftReconstructed() : s("a") {
    std::destroy_at(&s);
    std::construct_at(&s, "b");
  }
};

// A constructor that destroys a member on one path leaves it destroyed there
struct LeftDestroyedOnePath {
  std::string s;
  LeftDestroyedOnePath(bool c) : s("a") {
    if (c)
      std::destroy_at(&s);
  } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

// A return before the reconstruction leaves the member destroyed
struct LeftDestroyedReturn {
  std::string s;
  LeftDestroyedReturn(bool c) : s("a") {
    std::destroy_at(&s);
    if (c)
      return;
    std::construct_at(&s, "b");
  } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

void may_throw();

// An exception between the destroy and the reconstruction is not a normal exit
struct LeftDestroyedThrow {
  std::string s;
  LeftDestroyedThrow(bool c) : s("a") {
    std::destroy_at(&s);
    may_throw();
    if (c)
      throw 1;
    std::construct_at(&s, "b");
  }
};

// A delegating constructor that leaves a member destroyed hands it to the destructor destroyed
struct LeftDestroyedDelegating {
  std::string s;
  LeftDestroyedDelegating() : s("a") {}
  LeftDestroyedDelegating(int) : LeftDestroyedDelegating() { std::destroy_at(&s); } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

// A scalar member left destroyed has no destructor to run
struct LeftDestroyedScalar {
  int i = 0;
  LeftDestroyedScalar() { std::destroy_at(&i); }
};

// A scalar member without an initializer is uninitialized in the body
struct ScalarNoInit {
  int i; // expected-note {{member 'i' declared here}}
  ScalarNoInit() { std::construct_at(&i, 1); } // expected-error {{constructor does not initialize member 'i' under profile 'std::init'}}
};

//--- locals.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <lifetimes.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

void take(const std::string &s);
void take_ptr(std::string *p);
struct X { int v; ~X(); };
struct D { int v; ~D(); };
struct Agg { D d [[uninit]]; int k [[uninit]]; };
union U { int i; float f; };

// A destructor call ends a local's lifetime, so its scope exit destroys it again
void destructor_call_then_scope_exit() {
  std::string s("a"); // expected-note {{'s' is destroyed at the end of its scope}}
  s.~string();
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// std::destroy over a local's address destroys it
void destroy_range_then_scope_exit() {
  X x{}; // expected-note {{'x' is destroyed at the end of its scope}}
  std::destroy(&x, &x + 1);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// std::destroy_n over a local's address destroys it
void destroy_n_then_scope_exit() {
  X x{}; // expected-note {{'x' is destroyed at the end of its scope}}
  std::destroy_n(&x, 1);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// std::ranges::destroy_at destroys a local
void ranges_destroy_at_then_scope_exit() {
  X x{}; // expected-note {{'x' is destroyed at the end of its scope}}
  std::ranges::destroy_at(&x);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// allocator_traits::destroy destroys a local
void allocator_destroy_then_scope_exit() {
  std::allocator<X> al;
  X x{}; // expected-note {{'x' is destroyed at the end of its scope}}
  std::allocator_traits<std::allocator<X>>::destroy(al, &x);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// std::destroy over a whole local array destroys it
void array_destroy_then_scope_exit() {
  X a[2]{}; // expected-note {{'a' is destroyed at the end of its scope}}
  std::destroy(a, a + 2);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// std::ranges::destroy over a whole local array destroys it
void array_ranges_destroy_then_scope_exit() {
  X a[2]{}; // expected-note {{'a' is destroyed at the end of its scope}}
  std::ranges::destroy(a);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// A local destroyed twice is a double destruction
void destroy_twice() {
  std::string s("a");
  std::destroy_at(&s);
  std::destroy_at(&s); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
  std::construct_at(&s);
}

// Destroying and reconstructing a local is accepted
void destroy_then_construct() {
  std::string s("a");
  std::destroy_at(&s);
  std::construct_at(&s, "b");
  take(s);
}

// A destructor call and a placement new reconstruct a local
void destructor_call_then_placement() {
  std::string s("a");
  s.~string();
  new (&s) std::string("b");
  take(s);
}

// A construction after a destroy on one path constructs over the object the other path kept
void destroy_one_path_then_placement(bool c) {
  std::string s("a");
  if (c)
    s.~string();
  new (&s) std::string("b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A placement new over a live local constructs over a live object
void placement_over_live() {
  std::string s("a");
  new (&s) std::string("b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A construction through a conditional whose other arm is null constructs over the live local
void construct_null_arm(bool c) {
  std::string s("a");
  std::construct_at(c ? &s : nullptr, "b"); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// Destroying and reconstructing a local in a loop is accepted
void destroy_construct_loop(int n) {
  std::string s("a");
  for (int k = 0; k != n; ++k) {
    std::destroy_at(&s);
    std::construct_at(&s, "b");
  }
}

// A copy of a destroyed local reads uninitialized memory
void destroy_then_copy() {
  std::string s("a");
  std::destroy_at(&s);
  std::string t = s; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&s);
}

// An assignment to a destroyed local is a member call on uninitialized memory
void destroy_then_assign() {
  std::string s("a");
  std::destroy_at(&s);
  s = "b"; // expected-error {{calling member function 'operator=' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  std::construct_at(&s);
}

// A member call on a destroyed local binds its object to uninitialized memory
void destroy_then_call() {
  std::string s("a");
  std::destroy_at(&s);
  (void)s.size(); // expected-error {{calling member function 'size' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  std::construct_at(&s);
}

// A pointer to a destroyed local bound to an unmarked parameter refers to uninitialized memory
void destroy_then_pass() {
  std::string s("a");
  std::destroy_at(&s);
  take_ptr(&s); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&s);
}

// A read of a member of a destroyed local reads uninitialized memory
int destroy_then_member_read() {
  X x{1};
  std::destroy_at(&x);
  int r = x.v; // expected-error {{read of a subobject of uninitialized storage accesses uninitialized memory under profile 'std::init'}}
  std::construct_at(&x);
  return r;
}

// A store to a member of a destroyed local initializes nothing
void destroy_then_member_store() {
  X x{1};
  std::destroy_at(&x);
  x.v = 2; // expected-error {{writing a member of uninitialized storage does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
  std::construct_at(&x);
}

// A scalar local destroyed and reconstructed is accepted, and its plain read after the destroy is the uninitialized-values analysis's
int scalar_destroy_construct() {
  int i = 1;
  std::destroy_at(&i);
  int r = i;
  std::construct_at(&i, 2);
  return r + i;
}

// std::construct_at over a live scalar local constructs over a live object
void scalar_construct_live() {
  int i = 1;
  std::construct_at(&i, 2); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A marked pointer bound to a destroyed local is accepted, and constructing through it reconstructs the local
void marked_binding_destroyed() {
  std::string s("a");
  std::destroy_at(&s);
  std::string *p [[ref_to_uninit]] = &s;
  std::construct_at(p, "b");
  take(s);
}

// A destroy through the pointer a construction returned destroys the local
void destroy_through_construction_result() {
  std::string s("a");
  std::destroy_at(&s);
  std::string *q = std::construct_at(&s, "b");
  std::destroy_at(q);
  std::construct_at(&s, "c");
}

// A by-value parameter destroyed twice is a double destruction
void parameter_destroy_twice(X x) {
  std::destroy_at(&x);
  std::destroy_at(&x); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
  std::construct_at(&x);
}

// A by-value parameter destroyed and reconstructed is accepted
void parameter_destroy_construct(X x) {
  std::destroy_at(&x);
  std::construct_at(&x);
}

// A by-value parameter left destroyed is destroyed again by its owner
void parameter_left_destroyed(X x) { // expected-note {{'x' is destroyed at the end of its scope}}
  std::destroy_at(&x);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// A by-value parameter destroyed on one path is destroyed again there by its owner
void parameter_left_destroyed_one_path(X x, bool c) { // expected-note {{'x' is destroyed at the end of its scope}}
  if (c)
    std::destroy_at(&x);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

void may_throw();

// An exception between a parameter's destroy and its reconstruction is not a normal exit
void parameter_destroy_throw(X x, bool c) {
  std::destroy_at(&x);
  may_throw();
  if (c)
    throw 1;
  std::construct_at(&x);
}

// A lambda's by-value parameter left destroyed is destroyed again by its owner
void lambda_parameter_left_destroyed() {
  [](X x) { // expected-note {{'x' is destroyed at the end of its scope}}
    std::destroy_at(&x);
  }(X{}); // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}
}

// A catch variable is constructed at the handler's entry
void catch_variable_destroy() {
  try {
    throw 1;
  } catch (std::string s) { // expected-note {{'s' is destroyed at the end of its scope}}
    std::destroy_at(&s);
  } // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}
}

// A lambda's own local has a lifetime in the lambda's body
void lambda_own_local() {
  [] {
    std::string s("a"); // expected-note {{'s' is destroyed at the end of its scope}}
    std::destroy_at(&s);
  }(); // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}
}

// A local a lambda captures is judged by form in the lambda's body
void lambda_captured_local() {
  std::string s("a");
  [&] { std::destroy_at(&s); }();
}

struct XView {
  X *first, *last;
  X *begin() const;
  X *end() const;
  std::size_t size() const;
};

// A range destroy of a local view destroys the view's elements, not the view
void view_range_destroy(XView v) {
  std::ranges::destroy(v);
  (void)v.size();
  std::destroy_at(&v);
  std::construct_at(&v);
}

// A destroy through a reference is not attributed to the local
void destroy_through_reference() {
  X x{};
  X &r = x;
  std::destroy_at(&r);
}

// A local with [[uninit]] members destroyed whole twice is a double destruction
void aggregate_destroy_twice() {
  Agg a; // expected-note {{'a' is destroyed at the end of its scope}}
  std::construct_at(&a.d);
  std::destroy_at(&a);
  std::destroy_at(&a); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// A local with [[uninit]] members destroyed and reconstructed whole is accepted
void aggregate_destroy_construct() {
  Agg a;
  std::construct_at(&a.d);
  std::destroy_at(&a);
  std::construct_at(&a);
}

// A union local has no lifetime of its own
void union_destroy_twice() {
  U u{1};
  std::destroy_at(&u.i);
  std::destroy_at(&u.i);
}

// A raw byte buffer has no lifetime of its own, and a placement new into it is not judged
void raw_buffer_place_twice() {
  alignas(std::string) unsigned char buf[sizeof(std::string)] = {};
  new (buf) std::string("a");
  new (buf) std::string("b");
}

//--- submembers.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <lifetimes.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

struct D { int v; ~D(); };
struct S2 { D d; int k; };
struct S3 { D d [[uninit]]; int k [[uninit]]; };
struct DView {
  D *first, *last;
  D *begin() const;
  D *end() const;
  std::size_t size() const;
};
struct WithView { DView v; int k; };

// A member destroyed below an [[uninit]] local is destroyed again by the local's scope exit
void member_destroy_then_scope_exit() {
  S2 s [[uninit]]; // expected-note {{'s' is destroyed at the end of its scope}}
  std::construct_at(&s);
  std::destroy_at(&s.d);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// A member destroyed twice below an [[uninit]] local is a double destruction
void member_destroy_twice() {
  S2 s [[uninit]];
  std::construct_at(&s);
  std::destroy_at(&s.d);
  std::destroy_at(&s.d); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
  std::construct_at(&s.d);
}

// A read below a destroyed member reads uninitialized memory
int member_destroy_then_read() {
  S2 s [[uninit]];
  std::construct_at(&s);
  std::destroy_at(&s.d);
  int r = s.d.v; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  std::construct_at(&s.d);
  return r;
}

// A member destroyed and reconstructed below an [[uninit]] local is accepted
void member_destroy_construct() {
  S2 s [[uninit]];
  std::construct_at(&s);
  std::destroy_at(&s.d);
  std::construct_at(&s.d);
}

// A read of another member after a member's destroy is accepted
int member_destroy_sibling_read() {
  S2 s [[uninit]];
  std::construct_at(&s);
  std::destroy_at(&s.d);
  int r = s.k;
  std::construct_at(&s.d);
  return r;
}

// A destroy of the storage whole after a member's destroy destroys the member again
void member_destroy_then_whole(bool c) {
  S2 s [[uninit]]; // expected-note {{'s' is destroyed at the end of its scope}}
  std::construct_at(&s);
  if (c)
    std::destroy_at(&s.d);
  std::destroy_at(&s); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// An [[uninit]] member destroyed twice is a double destruction, a read of it after is rejected, and the scope exit destroys it again
void uninit_member_destroy_twice() {
  S3 s [[uninit]]; // expected-note {{'s' is destroyed at the end of its scope}}
  std::construct_at(&s);
  std::destroy_at(&s.d);
  std::destroy_at(&s.d); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
  int r = s.d.v; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)r;
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// An [[uninit]] member destroyed and reconstructed is accepted
void uninit_member_destroy_construct() {
  S3 s [[uninit]];
  std::construct_at(&s);
  std::destroy_at(&s.d);
  std::construct_at(&s.d);
}

// A member destroyed below an ordinary local is destroyed again by the local's scope exit
void local_member_destroy_then_scope_exit() {
  S2 s{}; // expected-note {{'s' is destroyed at the end of its scope}}
  std::destroy_at(&s.d);
} // expected-error {{storage already destroyed is destroyed again by its scope-exit destructor under profile 'std::init'}}

// A member destroyed and reconstructed below an ordinary local is accepted
void local_member_destroy_construct() {
  S2 s{};
  std::destroy_at(&s.d);
  std::construct_at(&s.d);
}

struct Outer {
  S2 s;
  // A destructor that destroys a member of a member destroys it again at the member teardown
  ~Outer() {
    std::destroy_at(&s.d);
  } // expected-error {{storage already destroyed is destroyed again by the destructor's member teardown under profile 'std::init'}}
};

// A range destroy below tracked storage destroys the range's elements, not the member
void member_range_destroy(WithView *p [[ref_to_uninit]]) {
  std::construct_at(p);
  std::ranges::destroy(p->v);
  (void)p->v.size();
  std::destroy_at(p);
}
