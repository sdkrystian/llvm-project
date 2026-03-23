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

  u.i = 42;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{reading union member 'i' is discouraged by profile std::type}}

  float val = u.f;
  // enforced-error@-1 {{reading union member 'f' is not allowed when profile std::type is enforced}}
  // applied-warning@-2 {{reading union member 'f' is discouraged by profile std::type}}
}

// --- Suppression in unevaluated contexts ---

void test_unevaluated() {
  int x = 0;
  // sizeof is an unevaluated context - no profile diagnostic for the cast
  (void)sizeof(reinterpret_cast<long*>(&x));
}
