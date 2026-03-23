// RUN: %clang_cc1 -fsyntax-only -verify=enforced -std=c++20 -fsafety-profile-enforce=type %s
// RUN: %clang_cc1 -fsyntax-only -verify=applied -std=c++20 -fsafety-profile-apply=type -Wsafety-profile-type %s

namespace std {
  enum class byte : unsigned char {};
}
using uintptr_t = __UINTPTR_TYPE__;

// --- Rule 4.1: reinterpret_cast ---

void test_reinterpret_cast() {
  int x = 0;
  int *p = &x;

  long *q = reinterpret_cast<long*>(p);
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'reinterpret_cast' is discouraged by profile std::type}}

  long &r = reinterpret_cast<long&>(x);
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'reinterpret_cast' is discouraged by profile std::type}}

  // Allowed: cast to std::byte*
  std::byte *bp = reinterpret_cast<std::byte*>(p);
  const std::byte *cbp = reinterpret_cast<const std::byte*>(p);

  // Allowed: pointer to uintptr_t
  uintptr_t u = reinterpret_cast<uintptr_t>(p);
}

// --- Rule 4.2: const_cast ---

void test_const_cast() {
  const int x = 42;
  const int *cp = &x;

  int *p = const_cast<int*>(cp);
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'const_cast' that casts away constness is discouraged by profile std::type}}

  const int &cr = x;
  int &r = const_cast<int&>(cr);
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'const_cast' that casts away constness is discouraged by profile std::type}}

  // Adding const is fine
  int y = 0;
  const int *cp2 = const_cast<const int*>(&y);
}

// --- Rule 4.3: static_cast ---

struct Base { virtual ~Base() = default; };
struct Derived : Base { int val = 0; };

void test_static_cast_narrowing() {
  long long ll = 100;
  int i = static_cast<int>(ll);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'long long' to 'int' is discouraged by profile std::type}}

  double d = 3.14;
  int j = static_cast<int>(d);
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'int' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'double' to 'int' is discouraged by profile std::type}}

  double e = 1.0;
  float f = static_cast<float>(e);
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'float' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'double' to 'float' is discouraged by profile std::type}}

  long long big = 1LL;
  float g = static_cast<float>(big);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'float' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'long long' to 'float' is discouraged by profile std::type}}

  // Cast to bool is allowed
  bool b = static_cast<bool>(ll);

  // Same-width or wider cast is allowed
  long long ll2 = static_cast<long long>(i);
}

void test_static_cast_downcast() {
  Derived derived;
  Base &base = derived;
  Derived &d = static_cast<Derived&>(base);
  // enforced-error@-1 {{downcasting 'static_cast' from 'Base' to 'Derived &' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{downcasting 'static_cast' from 'Base' to 'Derived &' is discouraged by profile std::type}}

  Base *bp = &derived;
  Derived *dp = static_cast<Derived*>(bp);
  // enforced-error@-1 {{downcasting 'static_cast' from 'Base *' to 'Derived *' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{downcasting 'static_cast' from 'Base *' to 'Derived *' is discouraged by profile std::type}}
}

void test_static_cast_unrelated() {
  int x = 42;
  void *vp = &x;
  int *ip = static_cast<int*>(vp);
  // enforced-error@-1 {{'static_cast' between unrelated types 'void *' and 'int *' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'static_cast' between unrelated types 'void *' and 'int *' is discouraged by profile std::type}}

  int *p = &x;
  void *vp2 = static_cast<void*>(p);
  // enforced-error@-1 {{'static_cast' between unrelated types 'int *' and 'void *' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'static_cast' between unrelated types 'int *' and 'void *' is discouraged by profile std::type}}

  void *vp3 = (void*)p;
  // enforced-error@-1 {{'static_cast' between unrelated types 'int *' and 'void *' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'static_cast' between unrelated types 'int *' and 'void *' is discouraged by profile std::type}}

  // void* to void* is fine (identity)
  void *vp4 = static_cast<void*>(vp);
}

// --- Rule 4.7: uninitialized variables ---

void test_uninit() {
  int x;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{uninitialized variable declaration is discouraged by profile std::type}}

  double d;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{uninitialized variable declaration is discouraged by profile std::type}}

  int *p;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{uninitialized variable declaration is discouraged by profile std::type}}

  // Initialized variables are fine
  int y = 0;
  double e = 0.0;
  int *q = nullptr;

  // Static/thread_local storage duration: zero-initialized, not vacuous
  static int si;
  thread_local int ti;
}

// --- Rule 4.8: va_arg ---

int sum(int count, ...) {
  __builtin_va_list args;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{uninitialized variable declaration is discouraged by profile std::type}}
  __builtin_va_start(args, count);
  int total = 0;
  for (int i = 0; i < count; ++i) {
    total += __builtin_va_arg(args, int);
    // enforced-error@-1 {{'va_arg' is not allowed when profile std::type is enforced}}
    // applied-warning@-2 {{'va_arg' is discouraged by profile std::type}}
  }
  __builtin_va_end(args);
  return total;
}

// --- Rule 4.9: union member access ---

union U {
  int i;
  float f;
};

void test_union() {
  U u = {0};

  // Writes to union members are allowed (LHS of assignment)
  u.i = 42;

  // Compound assignment is allowed (LHS of assignment operator)
  u.i += 1;

  // Reads are rejected
  float val = u.f;
  // enforced-error@-1 {{reading union member 'f' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{reading union member 'f' is discouraged by profile std::type}}

  int ival = u.i;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{reading union member 'i' is discouraged by profile std::type}}

  // Increment/decrement reads the member and is not an assignment
  u.i++;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{reading union member 'i' is discouraged by profile std::type}}

  ++u.i;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{reading union member 'i' is discouraged by profile std::type}}
}

// --- Rule 4.5/4.6: C-style and functional casts ---

void test_cstyle_casts() {
  int x = 0;
  int *p = &x;

  // C-style reinterpret_cast equivalent
  long *q = (long*)p;
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'reinterpret_cast' is discouraged by profile std::type}}

  // C-style const_cast equivalent
  const int *cp = &x;
  int *mp = (int*)cp;
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{'const_cast' that casts away constness is discouraged by profile std::type}}

  // C-style static_cast narrowing
  double d = 3.14;
  int i = (int)d;
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'int' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'double' to 'int' is discouraged by profile std::type}}

  // Functional cast narrowing
  long long ll = 100;
  int j = int(ll);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'long long' to 'int' is discouraged by profile std::type}}
}

// --- static_cast narrowing: signed/unsigned same-width ---

void test_signedness_narrowing() {
  int s = 42;
  unsigned u = static_cast<unsigned>(s);
  // enforced-error@-1 {{narrowing 'static_cast' from 'int' to 'unsigned int' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'int' to 'unsigned int' is discouraged by profile std::type}}

  unsigned u2 = 42u;
  int s2 = static_cast<int>(u2);
  // enforced-error@-1 {{narrowing 'static_cast' from 'unsigned int' to 'int' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{narrowing 'static_cast' from 'unsigned int' to 'int' is discouraged by profile std::type}}
}

// --- Suppression in unevaluated contexts ---

void test_unevaluated() {
  int x = 0;
  // sizeof is an unevaluated context - no profile diagnostic for the cast
  (void)sizeof(reinterpret_cast<long*>(&x));
}
