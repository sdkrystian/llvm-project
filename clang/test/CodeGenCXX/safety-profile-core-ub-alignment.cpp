// The std::core_ub profile's misaligned_access rule (P4317
// {basic.align.object.alignment}): under enforcement, an access through a
// pointer gets a runtime alignment check -- low bits of the address masked
// against the type's alignment -- that branches to a trap. Emitted from
// EmitTypeCheck with no sanitizer enabled.

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
// Sanitizer independence: with -fsanitize=alignment the same access is
// instrumented twice -- redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=alignment -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

// A load through a possibly-misaligned pointer: mask the low bits, compare
// against zero, trap on failure.
// CHECK-LABEL: define {{.*}} @_Z8load_intPv(
// CHECK: ptrtoint ptr {{.*}} to i64
// CHECK: and i64 {{.*}}, 3
// CHECK: icmp eq i64 {{.*}}, 0
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// BOTH-LABEL: define {{.*}} @_Z8load_intPv(
// BOTH: call void @__ubsan_handle_type_mismatch
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int load_int(void *p) { return *static_cast<int *>(p); }

// A store is checked the same way.
// CHECK-LABEL: define {{.*}} @_Z9store_intPvi(
// CHECK: and i64 {{.*}}, 3
// CHECK: call void @llvm.ubsantrap(i8
void store_int(void *p, int v) { *static_cast<int *>(p) = v; }

// A wider type masks more low bits.
// CHECK-LABEL: define {{.*}} @_Z11load_doublePv(
// CHECK: and i64 {{.*}}, 7
// CHECK: call void @llvm.ubsantrap(i8
double load_double(void *p) { return *static_cast<double *>(p); }

// Access to a local whose alloca already satisfies the alignment: statically
// impossible to misalign, no check (and char accesses need no check at all).
// CHECK-LABEL: define {{.*}} @_Z8local_okv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int local_ok() {
  int x = 5;
  return *&x;
}

// CHECK-LABEL: define {{.*}} @_Z9load_charPc(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i8
char load_char(char *p) { return *p; }

// Suppression at function granularity.
// CHECK-LABEL: define {{.*}} @_Z13fn_suppressedPv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
[[profiles::suppress(std::core_ub)]] int fn_suppressed(void *p) {
  return *static_cast<int *>(p);
}

// Rule granularity: naming the violated rule suppresses; naming another rule
// of the same profile does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchPv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int rule_match(void *p) {
  [[profiles::suppress(std::core_ub, rule: "misaligned_access")]] return *
      static_cast<int *>(p);
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchPv(
// CHECK: call void @llvm.ubsantrap(i8
int rule_mismatch(void *p) {
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return *
      static_cast<int *>(p);
}

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$misaligned pointer access under profile 'std::core_ub'
