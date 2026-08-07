// The std::core_ub profile's signed_overflow rule (P4317
// {expr.mul.representable.type.result}): under enforcement, signed +, -, *,
// ++/--, unary -, and the INT_MIN/-1 arm of integer division and remainder
// get a runtime overflow check that branches to a trap.

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
// Sanitizer independence: with -fsanitize=signed-integer-overflow the same
// operation is instrumented twice -- profile trap and sanitizer handler --
// redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=signed-integer-overflow -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH
//
// -fwrapv defines wrapping for signed +, -, and *: those checks vanish.
// Division overflow (INT_MIN/-1) is undefined even under -fwrapv and stays
// checked -- the only traps in that run belong to the intmin_* functions at
// the bottom of this file (their zero_divide checks included).
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fwrapv -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=WRAPV

// NONE-NOT: llvm.ubsantrap
// WRAPV-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

// The check computes the would-be result with the matching
// llvm.s*.with.overflow intrinsic and traps on its overflow bit; the actual
// arithmetic is emitted independently afterwards.
// CHECK-LABEL: define {{.*}} @_Z3addii(
// CHECK: call { i32, i1 } @llvm.sadd.with.overflow.i32(
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// CHECK: add nsw i32
// BOTH-LABEL: define {{.*}} @_Z3addii(
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// BOTH: call void @__ubsan_handle_add_overflow
int add(int a, int b) { return a + b; }

// CHECK-LABEL: define {{.*}} @_Z3subii(
// CHECK: call { i32, i1 } @llvm.ssub.with.overflow.i32(
// CHECK: call void @llvm.ubsantrap(i8
// CHECK: sub nsw i32
int sub(int a, int b) { return a - b; }

// CHECK-LABEL: define {{.*}} @_Z3mulii(
// CHECK: call { i32, i1 } @llvm.smul.with.overflow.i32(
// CHECK: call void @llvm.ubsantrap(i8
// CHECK: mul nsw i32
int mul(int a, int b) { return a * b; }

// Unary minus lowers through EmitSub as 0 - x: covered for free.
// CHECK-LABEL: define {{.*}} @_Z3negi(
// CHECK: call { i32, i1 } @llvm.ssub.with.overflow.i32(i32 0, i32
// CHECK: call void @llvm.ubsantrap(i8
int neg(int a) { return -a; }

// ++/-- emits its own add rather than funneling through EmitAdd/EmitSub.
// CHECK-LABEL: define {{.*}} @_Z3inci(
// CHECK: call { i32, i1 } @llvm.sadd.with.overflow.i32(i32 {{.*}}, i32 1)
// CHECK: call void @llvm.ubsantrap(i8
int inc(int a) { return ++a; }

// CHECK-LABEL: define {{.*}} @_Z3deci(
// CHECK: call { i32, i1 } @llvm.ssub.with.overflow.i32(i32 {{.*}}, i32 1)
// CHECK: call void @llvm.ubsantrap(i8
int dec(int a) { return a--; }

// Compound assignment funnels through the same emitters.
// CHECK-LABEL: define {{.*}} @_Z10add_assignii(
// CHECK: call { i32, i1 } @llvm.sadd.with.overflow.i32(
// CHECK: call void @llvm.ubsantrap(i8
int add_assign(int a, int b) { a += b; return a; }

// Unsigned arithmetic wraps by definition: never guarded.
// CHECK-LABEL: define {{.*}} @_Z12unsigned_addjj(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
unsigned unsigned_add(unsigned a, unsigned b) { return a + b; }

// Constant operands that provably cannot overflow: no dead check.
// CHECK-LABEL: define {{.*}} @_Z9const_addv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int const_add() { return 1 + 2; }

// Widened operands cannot overflow: promoted short arithmetic is unchecked,
// as is ++/-- of a promotable type (the operation happens at int width).
// CHECK-LABEL: define {{.*}} @_Z8wide_addss(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int wide_add(short a, short b) { return a + b; }

// CHECK-LABEL: define {{.*}} @_Z9short_incs(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i16
short short_inc(short s) { return ++s; }

// Rule granularity: naming the violated rule suppresses; naming another rule
// of the same profile does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int rule_match(int a, int b) {
  [[profiles::suppress(std::core_ub, rule: "signed_overflow")]] return a + b;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchii(
// CHECK: call void @llvm.ubsantrap(i8
int rule_mismatch(int a, int b) {
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return a + b;
}

// INT_MIN / -1 (and % -1) is the signed_overflow rule riding the division
// emitters, next to zero_divide (two rules, two checks on a plain signed
// division). Undefined even under -fwrapv, so it survives that run.
// CHECK-LABEL: define {{.*}} @_Z10intmin_divii(
// CHECK: icmp ne i32 {{.*}}, -2147483648
// CHECK: call void @llvm.ubsantrap(i8
// WRAPV-LABEL: define {{.*}} @_Z10intmin_divii(
// WRAPV: icmp ne i32 {{.*}}, -2147483648
// WRAPV: call void @llvm.ubsantrap(i8
int intmin_div(int a, int b) { return a / b; }

// CHECK-LABEL: define {{.*}} @_Z10intmin_remii(
// CHECK: icmp ne i32 {{.*}}, -2147483648
// CHECK: call void @llvm.ubsantrap(i8
int intmin_rem(int a, int b) { return a % b; }

// A constant divisor other than -1 statically rules the overflow out (the
// zero_divide check is likewise elided for a constant nonzero divisor).
// CHECK-LABEL: define {{.*}} @_Z8div_by_2i(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int div_by_2(int a) { return a / 2; }

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$signed integer overflow under profile 'std::core_ub'
