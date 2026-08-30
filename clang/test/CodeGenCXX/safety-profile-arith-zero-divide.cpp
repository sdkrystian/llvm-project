// The test::arith profile's zero_divide rule (framework pattern 5): under
// enforcement, integer division and remainder get a runtime zero-divisor
// check that branches to a trap (llvm.ubsantrap with the ProfileViolation
// handler's own immediate).

// Enforced: checks are emitted, honoring [[profiles::suppress]].
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fprofiles-test-profiles -std=c++23 -emit-llvm -o - %s | FileCheck %s
//
// Not enforced, no -fprofiles, or test:: profiles not activated: no checks.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -fprofiles-test-profiles -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
//
// Under -fsanitize-debug-trap-reasons (default Detailed) with debug info, the
// trap's debug location names the violated profile.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fprofiles-test-profiles -std=c++23 -debug-info-kind=limited -emit-llvm -o - %s | FileCheck %s --check-prefix=TRAPMSG
//
// Sanitizer independence: the profile check is emitted regardless of
// sanitizer configuration, so combining enforcement with
// -fsanitize=integer-divide-by-zero instruments the same predicate twice --
// redundant, never wrong -- and no sanitizer knob can void the profile's
// guarantee.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fprofiles-test-profiles -fsanitize=integer-divide-by-zero -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(test::arith)]];
#endif

extern int g;

// CHECK-LABEL: define {{.*}} @_Z9basic_divii(
// CHECK: icmp ne i32
// CHECK: br i1 {{.*}}, label %cont, label %trap
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// BOTH-LABEL: define {{.*}} @_Z9basic_divii(
// BOTH: call void @__ubsan_handle_divrem_overflow
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int basic_div(int a, int b) { return a / b; }

// CHECK-LABEL: define {{.*}} @_Z9basic_remii(
// CHECK: call void @llvm.ubsantrap(i8
int basic_rem(int a, int b) { return a % b; }

// Compound assignments funnel through the same division/remainder emitters.
// CHECK-LABEL: define {{.*}} @_Z10div_assignii(
// CHECK: call void @llvm.ubsantrap(i8
int div_assign(int a, int b) { a /= b; return a; }

// CHECK-LABEL: define {{.*}} @_Z10rem_assignii(
// CHECK: call void @llvm.ubsantrap(i8
int rem_assign(int a, int b) { a %= b; return a; }

// A constant nonzero divisor cannot be zero: no dead check.
// CHECK-LABEL: define {{.*}} @_Z9const_divi(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int const_div(int a) { return a / 2; }

// Suppression at function granularity.
// CHECK-LABEL: define {{.*}} @_Z13fn_suppressedii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
[[profiles::suppress(test::arith)]] int fn_suppressed(int a, int b) {
  return a / b;
}

// Suppression at statement granularity.
// CHECK-LABEL: define {{.*}} @_Z15stmt_suppressedii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int stmt_suppressed(int a, int b) {
  [[profiles::suppress(test::arith)]] return a / b;
}

// Suppression on a local declaration covers its initializer.
// CHECK-LABEL: define {{.*}} @_Z15decl_suppressedii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int decl_suppressed(int a, int b) {
  [[profiles::suppress(test::arith)]] int x = a / b;
  return x;
}

// A condition variable is emitted without an enclosing DeclStmt; it is
// checked, and suppression on its declarator is honored.
// CHECK-LABEL: define {{.*}} @_Z8cond_varii(
// CHECK: call void @llvm.ubsantrap(i8
int cond_var(int a, int b) {
  if (int x = a / b)
    return x;
  return 0;
}

// CHECK-LABEL: define {{.*}} @_Z19cond_var_suppressedii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int cond_var_suppressed(int a, int b) {
  if ([[profiles::suppress(test::arith)]] int x = a / b)
    return x;
  return 0;
}

// Rule granularity: naming the violated rule suppresses; naming another rule
// of the same profile does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int rule_match(int a, int b) {
  [[profiles::suppress(test::arith, rule: "zero_divide")]] return a / b;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchii(
// CHECK: call void @llvm.ubsantrap(i8
int rule_mismatch(int a, int b) {
  [[profiles::suppress(test::arith, rule: "other_rule")]] return a / b;
}

// A lambda body is checked (the call operator is the emitted function)...
// CHECK-LABEL: define {{.*}} @_Z14lambda_checkedii(
// CHECK-LABEL: define internal {{.*}} @"_ZZ14lambda_checkediiENK3$_0clEii"(
// CHECK: call void @llvm.ubsantrap(i8
int lambda_checked(int a, int b) {
  auto l = [](int x, int y) { return x / y; };
  return l(a, b);
}

// ...and statement suppression around the lambda covers it, through the
// implicit attributes Sema propagates onto the call operator.
// CHECK-LABEL: define {{.*}} @_Z22lambda_stmt_suppressedii(
// CHECK-LABEL: define internal {{.*}} @"_ZZ22lambda_stmt_suppressediiENK3$_0clEii"(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int lambda_stmt_suppressed(int a, int b) {
  [[profiles::suppress(test::arith)]] auto l = [](int x, int y) {
    return x / y;
  };
  return l(a, b);
}

// A template instantiation's body is checked.
template <typename T> T tdiv(T a, T b) { return a / b; }
// CHECK-LABEL: define {{.*}} @_Z12use_templateii(
// CHECK-LABEL: define linkonce_odr {{.*}} @_Z4tdivIiET_S0_S0_(
// CHECK: call void @llvm.ubsantrap(i8
int use_template(int a, int b) { return tdiv(a, b); }

// A suppression on the constructor does not cover the NSDMI emitted within
// it (the field's construct, not the constructor's), while the suppression
// does cover the constructor's own body. The base-object constructors are
// emitted deferred, at the end of the module; their checks are at the bottom
// of this file.
struct NsdmiVsCtor {
  int m = 1 / g;
  [[profiles::suppress(test::arith)]] NsdmiVsCtor() {
    int x = g / g;
    (void)x;
  }
};
// CHECK-LABEL: define {{.*}} @_Z9use_nsdmiv(
void use_nsdmi() { NsdmiVsCtor n; }

// A suppression on the field does cover its NSDMI (checked at the bottom of
// this file).
struct NsdmiOnField {
  [[profiles::suppress(test::arith)]] int m = 1 / g;
  NsdmiOnField() {}
};
// CHECK-LABEL: define {{.*}} @_Z18use_nsdmi_on_fieldv(
void use_nsdmi_on_field() { NsdmiOnField n; }

// A default argument's tokens belong to the parameter's construct: the
// calling function's suppression does not cover its emission.
int callee(int x = 1 / g);
// CHECK-LABEL: define {{.*}} @_Z6callerv(
// CHECK: call void @llvm.ubsantrap(i8
[[profiles::suppress(test::arith)]] int caller() { return callee(); }

// A suppression on the parameter of the declaration that writes the default
// argument covers its emission at the call site.
int suppressed_dfl([[profiles::suppress(test::arith)]] int x = 1 / g);
// CHECK-LABEL: define {{.*}} @_Z18use_suppressed_dflv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int use_suppressed_dfl() { return suppressed_dfl(); }

// The same holds when the called definition inherits the default argument:
// the anchor is the parameter of the declaration that wrote it.
int inherited_dfl([[profiles::suppress(test::arith)]] int x = 1 / g);
int inherited_dfl(int x) { return x; }
// CHECK-LABEL: define {{.*}} @_Z17use_inherited_dflv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int use_inherited_dfl() { return inherited_dfl(); }

// A suppression on the definition's parameter has no default argument in its
// dominion: an inherited default argument's tokens stay checked.
int later_suppressed(int x = 1 / g);
int later_suppressed([[profiles::suppress(test::arith)]] int x) { return x; }
// CHECK-LABEL: define {{.*}} @_Z20use_later_suppressedv(
// CHECK: call void @llvm.ubsantrap(i8
int use_later_suppressed() { return later_suppressed(); }

// An inlined inheriting constructor (variadic base constructor) raises the
// suppression floor: the statement suppression at the use site does not
// cover the NSDMI emitted within the inlined constructor.
struct VBase {
  VBase(...);
};
struct DerivedI : VBase {
  using VBase::VBase;
  int m = 1 / g;
};
// CHECK-LABEL: define {{.*}} @_Z13use_inheritedi(
// CHECK: call void @llvm.ubsantrap(i8
void use_inherited(int a) {
  [[profiles::suppress(test::arith)]] DerivedI d(a);
}

// A global dynamic initializer is checked.
// CHECK-LABEL: define internal void @__cxx_global_var_init(
// CHECK: call void @llvm.ubsantrap(i8
int global_dyn = 10 / g;

// Documented emitter gaps, pinned: GCC vector-extension integer division and
// _Complex int division do not funnel through the scalar division emitter and
// get no check. (A third gap, scalar compound assignment with a _Complex
// computation type, is ill-formed in C++ and unreachable from this test.)
typedef int v4 __attribute__((vector_size(16)));
// CHECK-LABEL: define {{.*}} @_Z4vdivDv4_iS_(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret <4 x i32>
v4 vdiv(v4 a, v4 b) { return a / b; }

// CHECK-LABEL: define {{.*}} @_Z4cdivCiS_(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i64
_Complex int cdiv(_Complex int a, _Complex int b) { return a / b; }

// The deferred base-object constructors, emitted at the end of the module.
// NsdmiVsCtor: the NSDMI's division is checked, the suppressed constructor
// body's division is not.
// CHECK-LABEL: define linkonce_odr void @_ZN11NsdmiVsCtorC2Ev(
// CHECK: call void @llvm.ubsantrap(i8
// CHECK: sdiv
// CHECK-NOT: llvm.ubsantrap
// CHECK: sdiv
// CHECK: ret void
// NsdmiOnField: the field-suppressed NSDMI is not checked.
// CHECK-LABEL: define linkonce_odr void @_ZN12NsdmiOnFieldC2Ev(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret void

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$integer division or remainder by zero under profile 'test::arith'
