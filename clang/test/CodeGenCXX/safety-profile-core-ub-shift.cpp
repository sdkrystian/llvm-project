// The std::core_ub profile's invalid_shift rule (P4317
// {expr.shift.neg.and.width}): under enforcement, shifts get a runtime
// exponent check (0 <= amount < width of the promoted left operand) that
// branches to a trap. From C++20 on, a signed left shift losing set bits is
// defined behavior, so in C++23 only the exponent arm is live; a pre-C++20
// RUN pins the additional signed base arm.

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
// Sanitizer independence: with -fsanitize=shift-base,shift-exponent the
// same operation is instrumented twice -- redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=shift-base,shift-exponent -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH
//
// Pre-C++20, a signed left shift must also not move a set bit out of the
// sign bit: the check grows the sanitizer-shaped branchy base arm.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -std=c++17 -emit-llvm -o - %s | FileCheck %s --check-prefix=BASE

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

// C++23: exponent arm only.
// CHECK-LABEL: define {{.*}} @_Z3shlii(
// CHECK: icmp ule i32 {{.*}}, 31
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// CHECK: shl i32
// BOTH-LABEL: define {{.*}} @_Z3shlii(
// BOTH: call void @__ubsan_handle_shift_out_of_bounds
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// Pre-C++20: exponent arm, then the base arm behind its valid-exponent
// branch, merged through a PHI.
// BASE-LABEL: define {{.*}} @_Z3shlii(
// BASE: icmp ule i32 {{.*}}, 31
// BASE: br i1
// BASE: lshr i32
// BASE: icmp eq i32
// BASE: phi i1
// BASE: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int shl(int a, int b) { return a << b; }

// CHECK-LABEL: define {{.*}} @_Z3shrii(
// CHECK: icmp ule i32 {{.*}}, 31
// CHECK: call void @llvm.ubsantrap(i8
// CHECK: ashr i32
int shr(int a, int b) { return a >> b; }

// Compound assignment funnels through the same emitters.
// CHECK-LABEL: define {{.*}} @_Z10shl_assignii(
// CHECK: call void @llvm.ubsantrap(i8
int shl_assign(int a, int b) { a <<= b; return a; }

// A constant in-range exponent cannot violate the exponent arm, and in
// C++23 no base arm applies: no dead check. Pre-C++20 the signed base arm
// still depends on the runtime left operand, so the check stays there.
// CHECK-LABEL: define {{.*}} @_Z9shl_consti(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
// BASE-LABEL: define {{.*}} @_Z9shl_consti(
// BASE: call void @llvm.ubsantrap(i8
int shl_const(int a) { return a << 3; }

// CHECK-LABEL: define {{.*}} @_Z9shr_consti(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int shr_const(int a) { return a >> 3; }

// Rule granularity: naming the violated rule suppresses; naming another rule
// of the same profile does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int rule_match(int a, int b) {
  [[profiles::suppress(std::core_ub, rule: "invalid_shift")]] return a << b;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchii(
// CHECK: call void @llvm.ubsantrap(i8
int rule_mismatch(int a, int b) {
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return a << b;
}

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$shift out of bounds under profile 'std::core_ub'
