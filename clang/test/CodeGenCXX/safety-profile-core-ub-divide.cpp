// The std::core_ub profile's zero_divide rule (P4317 {expr.mul.div.by.zero},
// framework pattern 5): under enforcement, integer division and remainder
// get a runtime zero-divisor check that branches to a trap (llvm.ubsantrap
// with the ProfileViolation handler's own immediate). std::core_ub is a real
// (std::-prefixed) profile: plain -fprofiles suffices, no test-profiles gate.

// Enforced: checks are emitted, honoring [[profiles::suppress]].
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -std=c++23 -emit-llvm -o - %s | FileCheck %s
//
// Not enforced, or no -fprofiles: no checks.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
//
// Under -fsanitize-debug-trap-reasons (default Detailed) with debug info, the
// trap's debug location names the violated profile.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -std=c++23 -debug-info-kind=limited -emit-llvm -o - %s | FileCheck %s --check-prefix=TRAPMSG
//
// Sanitizer independence: the profile check is emitted regardless of
// sanitizer configuration, so combining enforcement with
// -fsanitize=integer-divide-by-zero instruments the same predicate twice --
// redundant, never wrong -- and no sanitizer knob can void the profile's
// guarantee.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=integer-divide-by-zero -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH
//
// Two profiles ride the div/rem site as independent per-profile calls:
// enforcing std::core_ub and the test::arith pilot together duplicates the
// predicate and the trap per profile -- redundant, never wrong -- with each
// trap attributed to its profile.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -DENFORCE_ARITH -fprofiles -fprofiles-test-profiles -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=RIDER

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif
#ifdef ENFORCE_ARITH
[[profiles::enforce(test::arith)]];
#endif

// CHECK-LABEL: define {{.*}} @_Z9basic_divii(
// CHECK: icmp ne i32
// CHECK: br i1 {{.*}}, label %cont, label %trap
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// BOTH-LABEL: define {{.*}} @_Z9basic_divii(
// BOTH: call void @__ubsan_handle_divrem_overflow
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// RIDER-LABEL: define {{.*}} @_Z9basic_divii(
// RIDER: icmp ne i32
// RIDER: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// RIDER: icmp ne i32
// RIDER: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int basic_div(int a, int b) { return a / b; }

// CHECK-LABEL: define {{.*}} @_Z9basic_remii(
// CHECK: call void @llvm.ubsantrap(i8
// RIDER-LABEL: define {{.*}} @_Z9basic_remii(
// RIDER: call void @llvm.ubsantrap(i8
// RIDER: call void @llvm.ubsantrap(i8
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
[[profiles::suppress(std::core_ub)]] int fn_suppressed(int a, int b) {
  return a / b;
}

// Suppression at statement granularity.
// CHECK-LABEL: define {{.*}} @_Z15stmt_suppressedii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int stmt_suppressed(int a, int b) {
  [[profiles::suppress(std::core_ub)]] return a / b;
}

// Suppression on a local declaration covers its initializer.
// CHECK-LABEL: define {{.*}} @_Z15decl_suppressedii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int decl_suppressed(int a, int b) {
  [[profiles::suppress(std::core_ub)]] int x = a / b;
  return x;
}

// Rule granularity: naming the violated rule suppresses; naming another rule
// of the same profile does not. Unsigned division, so zero_divide is the
// only std::core_ub rule riding the operation (a signed division would also
// carry the signed_overflow INT_MIN/-1 check).
// CHECK-LABEL: define {{.*}} @_Z10rule_matchjj(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
unsigned rule_match(unsigned a, unsigned b) {
  [[profiles::suppress(std::core_ub, rule: "zero_divide")]] return a / b;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchjj(
// CHECK: call void @llvm.ubsantrap(i8
unsigned rule_mismatch(unsigned a, unsigned b) {
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return a / b;
}

// Suppressing only the pilot profile does not remove std::core_ub's check
// (and vice versa: each per-profile call gates on its own suppression).
// Unsigned, so exactly one check -- std::core_ub's zero_divide -- remains.
// RIDER-LABEL: define {{.*}} @_Z16other_suppressedjj(
// RIDER: call void @llvm.ubsantrap(i8
// RIDER-NOT: call void @llvm.ubsantrap(i8
// RIDER: ret i32
unsigned other_suppressed(unsigned a, unsigned b) {
  [[profiles::suppress(test::arith)]] return a / b;
}

// A template instantiation's body is checked.
template <typename T> T tdiv(T a, T b) { return a / b; }
// CHECK-LABEL: define {{.*}} @_Z12use_templateii(
// CHECK-LABEL: define linkonce_odr {{.*}} @_Z4tdivIiET_S0_S0_(
// CHECK: call void @llvm.ubsantrap(i8
int use_template(int a, int b) { return tdiv(a, b); }

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$integer division or remainder by zero under profile 'std::core_ub'
