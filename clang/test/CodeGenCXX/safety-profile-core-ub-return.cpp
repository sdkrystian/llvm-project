// The std::core_ub profile's missing_return rule (P4317
// {stmt.return.flow.off}): under enforcement, flowing off the end of a
// value-returning function traps. Reaching the fall-off point is itself the
// violation, so the check is an unconditional branch to the trap.

// Enforced: checks are emitted, honoring [[profiles::suppress]].
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -std=c++23 -emit-llvm -o - %s | FileCheck %s
//
// Not enforced, or no -fprofiles: no profile trap -- the fall-off path
// keeps its plain unreachable (with llvm.trap at -O0 under the default
// strict-return), and llvm.ubsantrap never appears.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
//
// Under -fsanitize-debug-trap-reasons (default Detailed) with debug info, the
// trap's debug location names the violated profile.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -std=c++23 -debug-info-kind=limited -emit-llvm -o - %s | FileCheck %s --check-prefix=TRAPMSG
//
// Sanitizer independence: with -fsanitize=return the handler call and the
// profile trap coexist -- redundant, never wrong.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -DENFORCE -fprofiles -fsanitize=return -std=c++23 -emit-llvm -o - %s | FileCheck %s --check-prefix=BOTH

// NONE-NOT: llvm.ubsantrap

#ifdef ENFORCE
[[profiles::enforce(std::core_ub)]];
#endif

// The fall-off path branches unconditionally into the profile trap, and the
// block is terminated by unreachable.
// CHECK-LABEL: define {{.*}} @_Z1fb(
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// CHECK: unreachable
// NONE-LABEL: define {{.*}} @_Z1fb(
// NONE: unreachable
// BOTH-LABEL: define {{.*}} @_Z1fb(
// BOTH: call void @__ubsan_handle_missing_return
// BOTH: call void @llvm.ubsantrap(i8 {{[0-9]+}})
int f(bool b) {
  if (b)
    return 1;
}

// A void function cannot flow off into undefined behavior: unchecked.
// CHECK-LABEL: define {{.*}} @_Z1gb(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret void
void g(bool b) {
  if (b)
    return;
}

// A function that returns on every path never reaches the fall-off point:
// unchecked.
// CHECK-LABEL: define {{.*}} @_Z1hb(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
int h(bool b) {
  if (b)
    return 1;
  return 0;
}

// Rule granularity: naming the violated rule suppresses -- the fall-off
// path reverts to the plain trap+unreachable -- and naming another rule
// does not.
// CHECK-LABEL: define {{.*}} @_Z10rule_matchb(
// CHECK-NOT: llvm.ubsantrap
// CHECK: unreachable
[[profiles::suppress(std::core_ub, rule: "missing_return")]] int rule_match(
    bool b) {
  if (b)
    return 1;
}

// CHECK-LABEL: define {{.*}} @_Z13rule_mismatchb(
// CHECK: call void @llvm.ubsantrap(i8
[[profiles::suppress(std::core_ub, rule: "other_rule")]] int rule_mismatch(
    bool b) {
  if (b)
    return 1;
}

// TRAPMSG: {{.*}}__clang_trap_msg$C++ Profiles$flowing off the end of a value-returning function under profile 'std::core_ub'
