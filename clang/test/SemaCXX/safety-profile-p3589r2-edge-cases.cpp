// Edge-case tests for P3589R2 and P3081R2 profile enforcement.
//
// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 -fsafety-profile-enforce=type -fsafety-profile-enforce=bounds -fsafety-profile-enforce=lifetime -fsafety-profile-enforce=arithmetic %s

namespace std {
  enum class byte : unsigned char {};
}
using uintptr_t = __UINTPTR_TYPE__;

// =====================================================================
// Template instantiation: diagnostics fire at both the definition (for
// non-dependent constructs) and at the instantiation point.
// =====================================================================

template<typename T> void tmpl_uninit() {
  int x;
  // expected-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  // expected-error@-2 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

template<typename T> void tmpl_dependent_cast(T *p) {
  long *q = reinterpret_cast<long *>(p); // expected-error {{'reinterpret_cast' is not allowed when profile std::type is enforced}}
}

void test_template_instantiation() {
  tmpl_uninit<int>(); // expected-note {{in instantiation of function template specialization}}
  int x = 0;
  tmpl_dependent_cast(&x); // expected-note {{in instantiation of function template specialization}}
}

// =====================================================================
// Lambda expressions: profile enforcement applies inside lambdas.
// =====================================================================

void test_lambda() {
  auto f = []() {
    int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  };
}

void test_lambda_capture_suppress() {
  [[profiles::suppress(std::type)]] {
    auto f = []() {
      int x;
    };
  }
  auto g = []() {
    int y; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  };
}

// =====================================================================
// Nested suppression: inner suppress adds, outer restore undoes all.
// =====================================================================

void test_nested_suppress() {
  [[profiles::suppress(std::type)]] {
    { int x; }
    int y;
  }
  int z; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

void test_nested_suppress_different_profiles() {
  [[profiles::suppress(std::type)]] {
    [[profiles::suppress(std::bounds)]] {
      int arr[4] = {};
      int *p = arr;
      p++;
      int x;
    }
    int arr2[4] = {};
    int *p2 = arr2; // expected-error {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
    int y;
  }
  int z; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

// =====================================================================
// Combined C-style cast violations: both const_cast-away and
// reinterpret_cast should be diagnosed.
// =====================================================================

void test_combined_cast() {
  int x = 0;
  const int *cp = &x;
  long *q = (long *)cp;
  // expected-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}
  // expected-error@-2 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}
}

// =====================================================================
// constexpr: narrowing static_cast is always diagnosed regardless of
// whether the value fits, per P3081R2 [expr.static.cast].
// =====================================================================

constexpr int const_narrow() {
  return static_cast<int>(1LL); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}
}

// =====================================================================
// Default function arguments: profile enforcement applies.
// =====================================================================

void default_arg_fn(int x = static_cast<int>(42LL)); // expected-error {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}

// =====================================================================
// Suppress on function declaration: propagates to body.
// =====================================================================

[[profiles::suppress(std::type)]]
void suppressed_fn() {
  int x;
  long long ll = 100;
  int i = static_cast<int>(ll);
  int *p = &i;
  long *q = reinterpret_cast<long *>(p);
}

void non_suppressed_fn() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

// =====================================================================
// Suppress on function declaration: per-profile granularity.
// =====================================================================

[[profiles::suppress(std::type)]]
void suppressed_type_only() {
  int *p = new int(42);
  delete p; // expected-error {{'delete' is not allowed when profile std::lifetime is enforced}}
}

// =====================================================================
// Multiple suppress attributes on one statement: all profiles
// suppressed.
// =====================================================================

void test_multiple_suppress_attrs() {
  int x = 0;
  int *p = &x;
  [[profiles::suppress(std::type)]]
  [[profiles::suppress(std::bounds)]] {
    long *q = reinterpret_cast<long *>(p);
    q++;
  }
}

// =====================================================================
// Suppress on various statement types.
// =====================================================================

void test_suppress_switch() {
  int x = 1;
  [[profiles::suppress(std::type)]]
  switch (x) {
    case 1: { int y; break; }
    default: break;
  }
  int z; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

void test_suppress_while() {
  int x = 0;
  int *p = &x;
  [[profiles::suppress(std::bounds)]]
  while (false) { p++; }
  p++; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

// =====================================================================
// SFINAE: profile enforcement does NOT change overload resolution.
// The first overload is still selected, and the const_cast violation
// is diagnosed at instantiation.
// =====================================================================

template<class T>
int sfinae_check(T, T * = const_cast<T *>((const T *)nullptr)) { return 1; }
// expected-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}
int sfinae_check(...) { return 0; }
int sfinae_result = sfinae_check(42);
// expected-note@-1 {{in instantiation of default function argument expression}}

// =====================================================================
// Profile enforcement in discarded if-constexpr branch: NOT diagnosed.
// =====================================================================

void test_discarded_branch() {
  int x = 0;
  if constexpr (false) {
    (void)reinterpret_cast<long *>(&x);
    int y;
  }
  if constexpr (true) {
    int z; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  }
}

// =====================================================================
// Profile enforcement in requires-expressions: NOT diagnosed.
// =====================================================================

template<typename T>
concept HasReinterpretCast = requires(T t) { reinterpret_cast<long *>(&t); };
static_assert(HasReinterpretCast<int>);

// =====================================================================
// Profile enforcement in unevaluated contexts: NOT diagnosed.
// =====================================================================

void test_unevaluated() {
  int x = 0;
  (void)sizeof(reinterpret_cast<long *>(&x));
  using T = decltype(reinterpret_cast<long *>(&x));
  (void)noexcept(reinterpret_cast<long *>(&x));
}

// =====================================================================
// Suppress appertaining to null-statement does NOT persist.
// =====================================================================

void test_null_stmt_suppress() {
  [[profiles::suppress(std::type)]];
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

// =====================================================================
// Suppress std::strict on a function declaration: suppresses type,
// bounds, and lifetime simultaneously.
// =====================================================================

[[profiles::suppress(std::strict)]]
void strict_suppressed_fn() {
  int x;
  int *p = new int(42);
  delete p;
  int arr[4] = {};
  int *q = arr;
  q++;
  int s = 42;
  unsigned u = s; // expected-error {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
}

// =====================================================================
// Pointer compound assignment: profile-rejected by std::bounds.
// =====================================================================

void test_compound_assign() {
  int x = 0;
  int *p = &x;
  p += 1; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  p -= 1; // expected-error {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}
