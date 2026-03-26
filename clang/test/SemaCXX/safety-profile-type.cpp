// RUN: %clang_cc1 -fsyntax-only -verify=enforced -std=c++20 -fsafety-profile-enforce=type %s

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

  long &r = reinterpret_cast<long&>(x);
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}

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

  const int &cr = x;
  int &r = const_cast<int&>(cr);
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}

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

  double d = 3.14;
  int j = static_cast<int>(d);
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'int' is not allowed when profile std::type is enforced}}

  double e = 1.0;
  float f = static_cast<float>(e);
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'float' is not allowed when profile std::type is enforced}}

  long long big = 1LL;
  float g = static_cast<float>(big);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'float' is not allowed when profile std::type is enforced}}

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

  Base *bp = &derived;
  Derived *dp = static_cast<Derived*>(bp);
  // enforced-error@-1 {{downcasting 'static_cast' from 'Base *' to 'Derived *' is not allowed when profile std::type is enforced}}
}

void test_static_cast_unrelated() {
  int x = 42;
  void *vp = &x;
  int *ip = static_cast<int*>(vp);
  // enforced-error@-1 {{'static_cast' between unrelated types 'void *' and 'int *' is not allowed when profile std::type is enforced}}

  int *p = &x;
  void *vp2 = static_cast<void*>(p);
  // enforced-error@-1 {{'static_cast' between unrelated types 'int *' and 'void *' is not allowed when profile std::type is enforced}}

  void *vp3 = (void*)p;
  // enforced-error@-1 {{'static_cast' between unrelated types 'int *' and 'void *' is not allowed when profile std::type is enforced}}

  // void* to void* is fine (identity)
  void *vp4 = static_cast<void*>(vp);
}

// --- Rule 4.7: uninitialized variables ---

void test_uninit() {
  int x;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

  double d;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

  int *p;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

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
  __builtin_va_start(args, count);
  int total = 0;
  for (int i = 0; i < count; ++i) {
    total += __builtin_va_arg(args, int);
    // enforced-error@-1 {{'va_arg' is not allowed when profile std::type is enforced}}
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

  int ival = u.i;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}

  // Increment/decrement reads the member and is not an assignment
  u.i++;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}

  ++u.i;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}
}

// --- Rule 4.5/4.6: C-style and functional casts ---

void test_cstyle_casts() {
  int x = 0;
  int *p = &x;

  // C-style reinterpret_cast equivalent
  long *q = (long*)p;
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}

  // C-style const_cast equivalent
  const int *cp = &x;
  int *mp = (int*)cp;
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}

  // C-style static_cast narrowing
  double d = 3.14;
  int i = (int)d;
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'int' is not allowed when profile std::type is enforced}}

  // Functional cast narrowing
  long long ll = 100;
  int j = int(ll);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
}

// --- static_cast narrowing: signed/unsigned same-width ---

void test_signedness_narrowing() {
  int s = 42;
  unsigned u = static_cast<unsigned>(s);
  // enforced-error@-1 {{narrowing 'static_cast' from 'int' to 'unsigned int' is not allowed when profile std::type is enforced}}

  unsigned u2 = 42u;
  int s2 = static_cast<int>(u2);
  // enforced-error@-1 {{narrowing 'static_cast' from 'unsigned int' to 'int' is not allowed when profile std::type is enforced}}
}

// --- Suppression in unevaluated contexts ---

void test_unevaluated() {
  int x = 0;
  // sizeof is an unevaluated context - no profile diagnostic for the cast
  (void)sizeof(reinterpret_cast<long*>(&x));
}
