// The std::core_ub profile's float_cast_overflow rule (P4317 {conv.fpint.*}
// and {conv.double.out.of.range}): under enforcement, a floating-point to
// integer conversion gets a runtime range check -- the value must fit in the
// destination after truncation toward zero -- that branches to a trap before
// the fptosi executes.

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
// Sanitizer independence: with -fsanitize=float-cast-overflow the same
// conversion is instrumented twice -- redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=float-cast-overflow -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

// The value must lie strictly inside the widest representable range.
// CHECK-LABEL: define {{.*}} @_Z3f2id(
// CHECK: fcmp ogt double
// CHECK: fcmp olt double
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// CHECK: fptosi double
// BOTH-LABEL: define {{.*}} @_Z3f2id(
// BOTH: call void @__ubsan_handle_float_cast_overflow
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int f2i(double d) { return (int)d; }

// An unsigned destination checks its own range.
// CHECK-LABEL: define {{.*}} @_Z3f2uf(
// CHECK: fcmp ogt float
// CHECK: call void @llvm.ubsantrap(i8
// CHECK: fptoui float
unsigned f2u(float f) { return (unsigned)f; }

// An implicit conversion funnels through the same emitter.
// CHECK-LABEL: define {{.*}} @_Z8implicitd(
// CHECK: call void @llvm.ubsantrap(i8
int implicit(double d) { return d; }

// A float-to-float conversion cannot overflow: unguarded.
// CHECK-LABEL: define {{.*}} @_Z3f2fd(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret float
float f2f(double d) { return (float)d; }

// So is an integer-to-integer conversion...
// CHECK-LABEL: define {{.*}} @_Z3i2si(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i16
short i2s(int i) { return (short)i; }

// ...and an integer-to-float one.
// CHECK-LABEL: define {{.*}} @_Z3i2fi(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret float
float i2f(int i) { return i; }

// Rule granularity: naming the violated rule suppresses; naming another rule
// of the same profile does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchd(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int rule_match(double d) {
  [[profiles::suppress(std::core_ub, rule: "float_cast_overflow")]] return (
      int)d;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchd(
// CHECK: call void @llvm.ubsantrap(i8
int rule_mismatch(double d) {
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return (int)d;
}

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$floating-point to integer conversion overflow under profile 'std::core_ub'
