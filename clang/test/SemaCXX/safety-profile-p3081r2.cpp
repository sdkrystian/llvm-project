// Comprehensive test for P3081R2 wording.
// Each test cites the normative sentence it covers.
//
// RUN: %clang_cc1 -fsyntax-only -verify=enforced -std=c++20 -fsafety-profile-enforce=type -fsafety-profile-enforce=bounds -fsafety-profile-enforce=lifetime -fsafety-profile-enforce=arithmetic %s

namespace std {
  enum class byte : unsigned char {};
}
using uintptr_t = __UINTPTR_TYPE__;
extern "C" void free(void *);

// =====================================================================
// [dcl.attr.profile]: "profiles defaults that behave as-if
//   profiles::enforce(P) had been written on an empty-declaration may
//   be defined in an implementation-defined manner."
//
// Tested by -fsafety-profile-enforce=* flags on the RUN line above.
// =====================================================================

// =====================================================================
// [dcl.attr.profile]: "At a program point Q, a profile P is enforced if
//   it is not suppressed and: the profile attribute profiles::enforce(P)
//   has been encountered"
//
// [dcl.attr.profile]: "if P is enforced at Q, the implementation shall
//   emit a profile-rejected diagnostic that C is not allowed when
//   profile P is enforced."
// =====================================================================

void test_enforced_diagnostic() {
  int x;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}

// =====================================================================
// [dcl.attr.profile]: profile-rejected only if "potentially evaluated"
// =====================================================================

void test_unevaluated_sizeof() {
  int x = 0;
  (void)sizeof(reinterpret_cast<long *>(&x));
}

void test_unevaluated_decltype() {
  int x = 0;
  using T = decltype(reinterpret_cast<long *>(&x));
}

void test_unevaluated_noexcept() {
  int x = 0;
  int *p = &x;
  (void)noexcept(reinterpret_cast<long *>(p));
}

// =====================================================================
// [dcl.attr.profile]: profile-rejected only if "outside a requires
//   expression"
// =====================================================================

template <typename T>
concept Derefable = requires(T t) { *t; };
static_assert(Derefable<int *>);

template <typename T>
concept Castable = requires(T t) { reinterpret_cast<long *>(&t); };
static_assert(Castable<int>);

// =====================================================================
// [dcl.attr.profile]: profile-rejected only if "outside a discarded
//   statement"
// =====================================================================

void test_discarded_if_constexpr() {
  int x = 0;
  if constexpr (false) {
    (void)reinterpret_cast<long *>(&x);
  }
}

// =====================================================================
// [dcl.attr.profile]: profile-rejected only if "outside of a candidate
//   function template specialization that is not named by an expression"
// =====================================================================

template <typename T>
auto sfinae_overload(T *p) -> decltype(reinterpret_cast<long *>(p), 0) {
  return 0;
}
auto sfinae_overload(...) { return 1; }

void test_sfinae() {
  int x = 0;
  sfinae_overload(&x);
}

// =====================================================================
// [dcl.attr.profile]: "enforcing a profile does not change the
//   semantics of the program's code, such as the results of overload
//   resolution or whether special member functions are deleted."
//
// [dcl.attr.profile] example: "If the profile std::type is not
//   enforced, x equals 1. If the profile std::type is enforced, the
//   implementation does not accept the translation unit. A program in
//   which x equals 0 is never produced."
// =====================================================================

template <class T>
int overload_check(T, T * = const_cast<T *>((const T *)nullptr)) { return 1; }
// enforced-error@-1 {{'const_cast' that casts away constness is not allowed}}
int overload_check(...) { return 0; }
int overload_result = overload_check(42);
// enforced-note@-1 {{in instantiation of default function argument}}

// =====================================================================
// [dcl.attr.profile]: "At a program point Q, a profile P is suppressed
//   if any enclosing scope of Q has the profile attribute
//   profiles::suppress(P)."
// =====================================================================

// --- Suppress on a single statement ---
void test_suppress_single_statement() {
  long long ll = 100;
  int a = static_cast<int>(ll);
  // enforced-error@-1 {{narrowing 'static_cast'}}

  [[profiles::suppress(std::type)]]
  int b = static_cast<int>(ll);

  int c = static_cast<int>(ll);
  // enforced-error@-1 {{narrowing 'static_cast'}}
}

// --- Suppress on a compound-statement ---
void test_suppress_compound_statement() {
  int s = 42;
  unsigned u1 = s;
  // enforced-error@-1 {{implicit conversion changes signedness}}

  [[profiles::suppress(std::arithmetic)]] {
    unsigned u2 = s;
  }

  unsigned u3 = s;
  // enforced-error@-1 {{implicit conversion changes signedness}}
}

// --- NullStmt form: suppression does NOT persist past the NullStmt ---
void test_suppress_null_stmt_does_not_persist() {
  // At function body scope
  int s = 42;
  [[profiles::suppress(std::arithmetic)]];
  unsigned u1 = s;
  // enforced-error@-1 {{implicit conversion changes signedness}}

  // Inside a nested block
  {
    [[profiles::suppress(std::type)]];
    int x;
    // enforced-error@-1 {{uninitialized variable declaration is not allowed}}
  }

  // Multiple NullStmt suppresses in sequence
  [[profiles::suppress(std::type)]];
  [[profiles::suppress(std::bounds)]];
  int y;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed}}
  int arr[4] = {};
  int *p = arr;
  // enforced-error@-1 {{array-to-pointer decay is not allowed}}
}

// --- Suppress one profile, others remain active ---
void test_suppress_granularity() {
  [[profiles::suppress(std::type)]] {
    int x;
    int *p = new int(1);
    delete p;
    // enforced-error@-1 {{'delete' is not allowed when profile std::lifetime is enforced}}
  }
  [[profiles::suppress(std::bounds)]] {
    int arr[4] = {};
    int *p = arr;
    p++;
    int y;
    // enforced-error@-1 {{uninitialized variable declaration is not allowed}}
  }
}

// --- Suppress on a declaration-statement (uninit) ---
void test_suppress_declaration_statement() {
  [[profiles::suppress(std::type)]]
  int x;

  int y;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed}}
}

// --- Suppress on an expression statement ---
void test_suppress_expression_statement() {
  int x = 0;
  int *p = &x;

  [[profiles::suppress(std::bounds)]]
  p++;

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

// --- Suppress on a for statement ---
void test_suppress_for_statement() {
  int arr[4] = {};

  [[profiles::suppress(std::bounds)]]
  for (int *p = arr, *end = arr + 4; p != end; ++p) {}

  int *q = arr;
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
}

// --- Suppress on an if statement ---
void test_suppress_if_statement() {
  int *p = new int(42);

  [[profiles::suppress(std::lifetime)]]
  if (p) { delete p; }

  int *q = new int(1);
  delete q;
  // enforced-error@-1 {{'delete' is not allowed when profile std::lifetime is enforced}}
}

// --- Multiple suppresses on one statement ---
void test_suppress_multiple() {
  int x = 0;
  int *p = &x;
  [[profiles::suppress(std::type)]]
  [[profiles::suppress(std::bounds)]] {
    long *q = reinterpret_cast<long *>(p);
    q++;
  }
}

// =====================================================================
// [expr.reinterpret.cast]: "If T is not a pointer or reference to cv
//   std::byte, and either v is not a pointer or T is not uintptr_t,
//   the expression is profile-rejected by profile std::type"
// =====================================================================

void test_reinterpret_cast(int *p) {
  // Pointer to unrelated pointer: rejected
  (void)reinterpret_cast<long *>(p);
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}

  // Reference reinterpret_cast: rejected
  int x = 0;
  (void)reinterpret_cast<long &>(x);
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}

  // Exception: T is pointer to cv std::byte
  (void)reinterpret_cast<std::byte *>(p);
  (void)reinterpret_cast<const std::byte *>(p);
  (void)reinterpret_cast<volatile std::byte *>(p);

  // Exception: v is a pointer and T is uintptr_t
  (void)reinterpret_cast<uintptr_t>(p);
}

// =====================================================================
// [expr.const.cast]: "If casting away constness, the expression is
//   profile-rejected by profile std::type."
// =====================================================================

void test_const_cast() {
  const int x = 42;
  const int *cp = &x;

  // Casting away constness on a pointer: rejected
  int *mp = const_cast<int *>(cp);
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}

  // Casting away constness on a reference: rejected
  const int &cr = x;
  int &mr = const_cast<int &>(cr);
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}

  // Adding const is fine
  int y = 0;
  const int *cp2 = const_cast<const int *>(&y);
}

// =====================================================================
// [expr.static.cast]: "If the conversion from v to T is a narrowing
//   conversion ([dcl.init.list]) and T is not bool, the expression is
//   profile-rejected by profile std::type."
// =====================================================================

void test_static_cast_narrowing() {
  long long ll = 100;
  int a = static_cast<int>(ll);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'int' is not allowed when profile std::type is enforced}}

  double d = 3.14;
  int b = static_cast<int>(d);
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'int' is not allowed when profile std::type is enforced}}

  double e = 1.0;
  float f = static_cast<float>(e);
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'float'}}

  long long big = 1LL;
  float g = static_cast<float>(big);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'float'}}

  // "T is not bool" exception: cast to bool is allowed
  bool b2 = static_cast<bool>(ll);

  // Non-narrowing (same or wider): allowed
  long long ll2 = static_cast<long long>(a);
}

// =====================================================================
// [expr.static.cast]: "If Deref_T is derived from Deref_From and if
//   not during constant evaluation ([expr.const]), then the expression
//   is profile-rejected by profile std::type."
// =====================================================================

struct Base {
  virtual ~Base() = default;
};
struct Derived : Base {
  int val = 0;
};

void test_static_cast_downcast() {
  Derived derived;
  Base &base = derived;

  // Pointer downcast: rejected
  Base *bp = &derived;
  Derived *dp = static_cast<Derived *>(bp);
  // enforced-error@-1 {{downcasting 'static_cast' from 'Base *' to 'Derived *' is not allowed when profile std::type is enforced}}

  // Reference downcast: rejected
  Derived &dr = static_cast<Derived &>(base);
  // enforced-error@-1 {{downcasting 'static_cast' from 'Base' to 'Derived &' is not allowed when profile std::type is enforced}}
}

// "if not during constant evaluation": spec exempts constant evaluation,
// but the implementation checks during Sema (before evaluation), so
// consteval downcasts are still diagnosed.

// =====================================================================
// [expr.static.cast]: "if Deref_T is unrelated to Deref_From, then
//   the expression is profile-rejected by profile std::type."
// =====================================================================

void test_static_cast_unrelated() {
  int x = 42;
  void *vp = &x;

  // void* -> T*: unrelated, rejected
  int *ip = static_cast<int *>(vp);
  // enforced-error@-1 {{'static_cast' between unrelated types 'void *' and 'int *' is not allowed when profile std::type is enforced}}

  // T* -> void*: unrelated, rejected
  int *p = &x;
  void *vp2 = static_cast<void *>(p);
  // enforced-error@-1 {{'static_cast' between unrelated types 'int *' and 'void *' is not allowed when profile std::type is enforced}}
}

// =====================================================================
// §4.5/4.6: "C-style (T)expression or functional T(expression) casts"
//   follow the same restrictions as the named cast they resolve to.
// =====================================================================

void test_c_style_and_functional_casts() {
  int x = 0;
  int *p = &x;

  // C-style cast resolving to reinterpret_cast
  long *q = (long *)p;
  // enforced-error@-1 {{'reinterpret_cast' is not allowed when profile std::type is enforced}}

  // C-style cast resolving to const_cast
  const int *cp = &x;
  int *mp = (int *)cp;
  // enforced-error@-1 {{'const_cast' that casts away constness is not allowed when profile std::type is enforced}}

  // C-style cast resolving to narrowing static_cast
  double d = 3.14;
  int i = (int)d;
  // enforced-error@-1 {{narrowing 'static_cast' from 'double' to 'int'}}

  // Functional cast resolving to narrowing static_cast
  long long ll = 100;
  int j = int(ll);
  // enforced-error@-1 {{narrowing 'static_cast' from 'long long' to 'int'}}
}

// =====================================================================
// [basic.life]: "if a variable has vacuous initialization or
//   initialization to an erroneous value, its definition is
//   profile-rejected by profile std::type"
// =====================================================================

struct POD {
  int x; // enforced-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  float y; // enforced-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
};

void test_uninit_variables() {
  // Scalar types
  int a;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  double b;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  int *c;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}

  // POD class with trivial default constructor: both the local variable
  // and its uninitialized members are diagnosed.
  POD pod;
  // enforced-error@-1 {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
  // enforced-note@-2 {{in implicit default constructor for 'POD' first required here}}

  // Initialized: fine
  int d = 0;
  double e = 0.0;
  int *f = nullptr;
  POD pod2 = {0, 0.0f};

  // Static storage duration: zero-initialized, not vacuous
  static int si;
  thread_local int ti;
}

// =====================================================================
// [cstdarg.syn]: "If va_arg is used, the use is profile-rejected by
//   profile std::type"
// =====================================================================

int sum(int count, ...) {
  [[profiles::suppress(std::bounds)]] {
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
}

// =====================================================================
// [class.union.general]: "For a member access expression that
//   nominates a union member M, if the use of M is not the left
//   operand of an assignment operator, and M is not part of the
//   union's common initial sequence, then the expression is
//   profile-rejected by profile std::type"
// =====================================================================

union U {
  int i;
  float f;
};

void test_union_access() {
  U u = {0};

  // Write via assignment: allowed (left operand of assignment operator)
  u.i = 42;
  u.f = 1.0f;

  // Compound assignment: allowed (left operand of assignment operator)
  u.i += 1;

  // Read: rejected
  int val = u.i;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}

  float fval = u.f;
  // enforced-error@-1 {{reading union member 'f' is not allowed when profile std::type is enforced}}

  // Pre/post increment reads the member (not assignment LHS)
  u.i++;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}

  ++u.i;
  // enforced-error@-1 {{reading union member 'i' is not allowed when profile std::type is enforced}}
}

// =====================================================================
// [expr.add]: "If the type of either operand is a pointer, the
//   expression is profile-rejected by profile std::bounds"
// =====================================================================

void test_pointer_addition() {
  int x = 0;
  int *p = &x;

  // ptr + int
  int *q = p + 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  // int + ptr
  int *r = 1 + p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  // Non-pointer addition: fine
  int i = 1 + 2;
}

// =====================================================================
// [expr.sub]: "If the type of either expression is a pointer, the
//   expression is profile-rejected by profile std::bounds"
// =====================================================================

void test_pointer_subtraction() {
  int x = 0;
  int *p = &x;
  int *q = &x;

  // ptr - int
  int *r = p - 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  // ptr - ptr
  auto diff = q - p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

// =====================================================================
// [expr.pre.incr]: "If the type of the operand is a pointer, the
//   expression is profile-rejected by profile std::bounds"
// [expr.post.incr]: same
// =====================================================================

void test_pointer_incr_decr() {
  int x = 0;
  int *p = &x;

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  p--;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  ++p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  --p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  // Non-pointer incr/decr: fine
  int i = 0;
  i++;
  ++i;
}

// =====================================================================
// [expr.pre.ass]: "If the type of the left operand is a pointer, the
//   expression is profile-rejected by profile std::bounds"
// =====================================================================

void test_pointer_compound_assign() {
  int x = 0;
  int *p = &x;

  p += 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  p -= 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

// =====================================================================
// [conv.array]: "An expression that performs this conversion is
//   profile-rejected by profile std::bounds"
// =====================================================================

void test_array_to_pointer_decay() {
  int arr[10] = {};

  // Implicit decay
  int *p = arr;
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}

  // String literal decay
  const char *s = "hello";
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
}

// =====================================================================
// [expr.delete]: "The expression is profile-rejected by profile
//   std::lifetime"
// =====================================================================

void test_delete() {
  int *p = new int(42);
  delete p;
  // enforced-error@-1 {{'delete' is not allowed when profile std::lifetime is enforced}}

  int *arr = new int[10u];
  delete[] arr;
  // enforced-error@-1 {{'delete' is not allowed when profile std::lifetime is enforced}}
}

// =====================================================================
// [c.malloc]: "Invoking free is profile-rejected by profile
//   std::lifetime"
// =====================================================================

void test_free() {
  int *p = new int(42);
  free(p);
  // enforced-error@-1 {{calling 'free' is not allowed when profile std::lifetime is enforced}}
}

// =====================================================================
// [expr.unary.op]: "the condition indirection_not_null(p) is
//   profile-checked by profile std::lifetime"
//
// Profile-CHECKED means runtime check, NOT compile-time rejection.
// Pointer dereference must NOT produce a profile-rejected diagnostic.
// =====================================================================

void test_null_dereference_not_rejected() {
  int x = 0;
  int *p = &x;
  int v = *p;
}

// =====================================================================
// [dcl.init.list]: "For a narrowing conversion from type From to type
//   T, if T is not void and T is not bool, the conversion is
//   profile-rejected by profile std::arithmetic"
// =====================================================================

void test_narrowing_init_list() {
  long long ll = 1LL;
  double d = 1.0;

  int a{ll};
  // enforced-error@-1 {{non-constant-expression cannot be narrowed from type 'long long' to 'int'}}
  // enforced-note@-2 {{insert an explicit cast}}
  // enforced-error@-3 {{narrowing conversion from 'long long' to 'int' is not allowed when profile std::arithmetic is enforced}}

  int b{d};
  // enforced-error@-1 {{type 'double' cannot be narrowed to 'int'}}
  // enforced-note@-2 {{insert an explicit cast}}
  // enforced-error@-3 {{narrowing conversion from 'double' to 'int' is not allowed when profile std::arithmetic is enforced}}

  // "T is not bool" exception: profile narrowing diagnostic not emitted for bool
  bool c{ll};
  // enforced-error@-1 {{non-constant-expression cannot be narrowed from type 'long long' to 'bool'}}
  // enforced-note@-2 {{insert an explicit cast}}
}

// =====================================================================
// [conv.integral]: "An integral conversion from a signed integer type
//   to an unsigned integer type, or from an unsigned integer type to a
//   signed integer type, is profile-rejected by profile
//   std::arithmetic"
// =====================================================================

void test_signedness_conversion() {
  int s = 42;
  unsigned u = 42u;

  // signed -> unsigned: rejected
  unsigned x = s;
  // enforced-error@-1 {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}

  // unsigned -> signed: rejected
  int y = u;
  // enforced-error@-1 {{implicit conversion changes signedness from 'unsigned int' to 'int' when profile std::arithmetic is enforced}}

  // Same signedness: fine
  long sl = s;
  unsigned long ul = u;
}

// =====================================================================
// [expr.pre]: "If during the evaluation of an expression, the result
//   is not mathematically defined or not in the range of representable
//   values for its type, the behavior is undefined and is
//   profile-rejected by profile std::arithmetic"
// =====================================================================

void test_arithmetic_overflow() {
  int a = __INT_MAX__ + 1;
  // enforced-warning@-1 {{overflow in expression; result is}}
  // enforced-error@-2 {{arithmetic overflow is not allowed when profile std::arithmetic is enforced}}

  int b = -(-__INT_MAX__ - 1);
  // enforced-warning@-1 {{overflow in expression; result is}}
  // enforced-error@-2 {{arithmetic overflow is not allowed when profile std::arithmetic is enforced}}
}

// =====================================================================
// Attribute-level wording tested by companion files:
//
// [dcl.attr.profile]: "profiles::enforce may be applied only to
//   empty-declaration"
//   -> safety-profile-attrs.cpp lines 10-14
//
// [dcl.attr.profile]: "that empty-declaration shall be the lexically
//   first declaration (if any) in the translation unit"
//   -> safety-profile-first-decl.cpp
//
// [dcl.attr.profile]: "Enforcing profile std::strict is equivalent to
//   enforcing all of profiles std::type, std::bounds, and std::lifetime
//   individually."
//   -> safety-profile-strict.cpp
//
// [dcl.attr.profile]: "unknown safety profile"
//   -> safety-profile-attrs.cpp lines 17-18
// =====================================================================
