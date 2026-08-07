// The std::core_ub profile's null_dereference rule (P4317
// {expr.unary.dereference}): under enforcement, an access through a pointer
// gets a runtime non-null check that branches to a trap. Emitted from
// EmitTypeCheck with no sanitizer enabled; check kinds where a null pointer
// is legal (pointer downcasts, dynamic_cast, typeid) are not checked.

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
// Sanitizer independence: with -fsanitize=null the same access is
// instrumented twice -- redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=null -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

// A dereference: compare the pointer against null, trap on failure. (The
// misaligned_access rule rides the same access; the null check comes
// first.)
// CHECK-LABEL: define {{.*}} @_Z5derefPi(
// CHECK: icmp ne ptr {{.*}}, null
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// BOTH-LABEL: define {{.*}} @_Z5derefPi(
// BOTH: call void @__ubsan_handle_type_mismatch
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int deref(int *p) { return *p; }

// Member access through a pointer checks the base pointer.
// CHECK-LABEL: define {{.*}} @_Z6memberP1S(
// CHECK: icmp ne ptr {{.*}}, null
// CHECK: call void @llvm.ubsantrap(i8
struct S {
  int a, b;
  int f();
};
int member(S *p) { return p->b; }

// So does a member call.
// CHECK-LABEL: define {{.*}} @_Z4callP1S(
// CHECK: icmp ne ptr {{.*}}, null
// CHECK: call void @llvm.ubsantrap(i8
int call(S *p) { return p->f(); }

// A pointer statically known non-null -- a local's address -- is not
// checked (and its alloca satisfies the alignment rule too: no traps).
// CHECK-LABEL: define {{.*}} @_Z8local_okv(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int local_ok() {
  int x = 5;
  return *&x;
}

// A pointer downcast may legally take null: no null_dereference check (and
// the empty class carries no alignment requirement, so no trap at all).
struct Base {};
struct Der : Base {};
// CHECK-LABEL: define {{.*}} @_Z4downP4Base(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret ptr
Der *down(Base *p) { return static_cast<Der *>(p); }

// Suppression at function granularity.
// CHECK-LABEL: define {{.*}} @_Z13fn_suppressedPi(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
[[profiles::suppress(std::core_ub)]] int fn_suppressed(int *p) { return *p; }

// Rule granularity: naming the violated rule suppresses; naming another
// rule of the same profile does not. A char access carries no alignment
// requirement, so null_dereference is the only rule riding it.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchPc(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i8
char rule_match(char *p) {
  [[profiles::suppress(std::core_ub, rule: "null_dereference")]] return *p;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchPc(
// CHECK: icmp ne ptr {{.*}}, null
// CHECK: call void @llvm.ubsantrap(i8
char rule_mismatch(char *p) {
  [[profiles::suppress(std::core_ub, rule: "other_rule")]] return *p;
}

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$null pointer dereference under profile 'std::core_ub'
