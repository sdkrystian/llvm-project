// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/members.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/members.cpp
// The LEADING_ERROR run adds a leading unrelated error so every later function
// is analyzed through the post-error path.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/members.cpp
// -Wunreachable-code makes the main path build a fully linearized CFG.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/members.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/constructors.cpp

// std::init: ordinary storage a lifecycle call of a body names -- a member of
// the current object here -- has a lifetime of its own in that body
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

// A lambda's body enters with the member in an unknown state, so its destroy is not judged
void A::lambda_destroy() {
  [this] { std::destroy_at(&s); }();
}

// A lambda's body that destroys a member twice is a double destruction
void A::lambda_destroy_twice() {
  [this] {
    std::destroy_at(&s);
    std::destroy_at(&s); // expected-error {{storage already destroyed is destroyed again by a '[[now_uninit]]' function under profile 'std::init'}}
  }();
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

// A scalar member without an initializer is uninitialized in the body
struct ScalarNoInit {
  int i; // expected-note {{member 'i' declared here}}
  ScalarNoInit() { std::construct_at(&i, 1); } // expected-error {{constructor does not initialize member 'i' under profile 'std::init'}}
};
