// The std::core_ub profile's out_of_bounds rule (P4317
// {expr.add.out.of.bounds}): under enforcement, a subscript of an array
// whose bound is known at the access gets a runtime index-below-bound check
// that branches to a trap. Emitted from EmitBoundsCheck with no sanitizer
// enabled.

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
// Sanitizer independence: with -fsanitize=array-bounds the same subscript is
// instrumented twice -- redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=array-bounds -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

// The index (widened to the size type) must be below the array bound. (The
// element access also carries the null/alignment profile checks; the ult
// compare is the bounds rule's.)
// CHECK-LABEL: define {{.*}} @_Z3getRA4_ii(
// CHECK: icmp ult i64 {{.*}}, 4
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// BOTH-LABEL: define {{.*}} @_Z3getRA4_ii(
// BOTH: call void @__ubsan_handle_out_of_bounds
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int get(int (&a)[4], int i) { return a[i]; }

// A local array is checked too (the element access additionally carries
// the null/alignment rules: its address is a runtime GEP, not the bare
// alloca).
// CHECK-LABEL: define {{.*}} @_Z9local_arri(
// CHECK: icmp ult i64 {{.*}}, 4
// CHECK: call void @llvm.ubsantrap(i8
int local_arr(int i) {
  int a[4] = {1, 2, 3, 4};
  return a[i];
}

// A constant index statically inside the constant bound: no dead check (and
// the constant element address folds to the alloca, eliding the other
// rules -- no traps at all).
// CHECK-LABEL: define {{.*}} @_Z11first_localv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int first_local() {
  int a[4] = {1, 2, 3, 4};
  return a[0];
}

// Taking a mere address may point one past the end: the check relaxes to
// index <= bound.
// CHECK-LABEL: define {{.*}} @_Z4addrRA4_ii(
// CHECK: icmp ule i64 {{.*}}, 4
// CHECK: call void @llvm.ubsantrap(i8
int *addr(int (&a)[4], int i) { return &a[i]; }

// ...and the constant one-past-the-end address is statically fine.
// CHECK-LABEL: define {{.*}} @_Z7pastendRA4_i(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret ptr
int *pastend(int (&a)[4]) { return &a[4]; }

// Rule granularity: naming the violated rule suppresses -- the ult bounds
// compare disappears while the null/alignment rules riding the element
// access keep their checks -- and naming another rule does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchi(
// CHECK-NOT: icmp ult
// CHECK: ret i32
int rule_match(int i) {
  int a[4] = {1, 2, 3, 4};
  [[profiles::suppress(std::core_ub, rule: "out_of_bounds")]] return a[i];
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchi(
// CHECK: icmp ult i64 {{.*}}, 4
// CHECK: call void @llvm.ubsantrap(i8
int rule_mismatch(int i) {
  int a[4] = {1, 2, 3, 4};
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return a[i];
}

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$array index out of bounds under profile 'std::core_ub'
