// The std::core_ub profile's enum_out_of_range rule (P4317
// {expr.static.cast.enum.outside.range}): under enforcement, loading an
// enumeration value gets a runtime range check -- the value must lie within
// the enumeration's representable range -- that branches to a trap. Emitted
// from EmitScalarRangeCheck with no sanitizer enabled.

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
// Sanitizer independence: with -fsanitize=enum the same load is instrumented
// twice -- redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=enum -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH
//
// The check must survive the optimizer: EmitScalarRangeCheck reports the
// profile check so no MD_range metadata is attached to the load, which
// would otherwise let the optimizer fold the comparison away.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -std=c++23 -O1 -emit-llvm -o - %s | FileCheck %s --check-prefix=OPT

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

enum E { A, B, C };

// E's representable range is [0, 3]. (The dereference also carries the
// null/alignment profile checks; the ule compare is this rule's.)
// CHECK-LABEL: define {{.*}} @_Z3useP1E(
// CHECK: icmp ule i32 {{.*}}, 3
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// BOTH-LABEL: define {{.*}} @_Z3useP1E(
// BOTH: call void @__ubsan_handle_load_invalid_value
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// OPT-LABEL: define {{.*}} @_Z3useP1E(
// OPT-NOT: !range
// OPT: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int use(E *p) { return *p; }

// An enumeration with a fixed underlying type can hold every value of that
// type: no range to violate, no check (no ule compare; the null/alignment
// rules still ride the access).
enum class Fixed : int { X, Y };
// CHECK-LABEL: define {{.*}} @_Z9use_fixedP5Fixed(
// CHECK-NOT: icmp ule
// CHECK: ret i32
int use_fixed(Fixed *p) { return (int)*p; }

// A bool load is not this rule's concern (P4317's case is enum-specific);
// with the pointer rules suppressed the function carries no checks at all.
// CHECK-LABEL: define {{.*}} @_Z8use_boolPb(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int use_bool(bool *p) {
  [[profiles::suppress(std::core_ub, rule: "null_dereference")]] return *p;
}

// Rule granularity: naming the violated rule suppresses -- the ule range
// compare disappears while the pointer rules keep their checks -- and
// naming another rule does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchP1E(
// CHECK-NOT: icmp ule
// CHECK: ret i32
int rule_match(E *p) {
  [[profiles::suppress(std::core_ub, rule: "enum_out_of_range")]] return *p;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchP1E(
// CHECK: icmp ule i32 {{.*}}, 3
// CHECK: call void @llvm.ubsantrap(i8
int rule_mismatch(E *p) {
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return *p;
}

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$load of an out-of-range enumeration value under profile 'std::core_ub'
