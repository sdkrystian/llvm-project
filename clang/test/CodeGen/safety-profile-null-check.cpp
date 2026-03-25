// Comprehensive CodeGen tests for P3081R2 §6.2 null dereference profile checks.
//
// P3081R2 [dcl.attr.profile]: "At a program point Q, a profile P is suppressed
//   if any enclosing scope of Q has the profile attribute
//   profiles::suppress(P)."
//
// P3081R2 [expr.unary.op]: "the condition indirection_not_null(p) is
//   profile-checked by profile std::lifetime"
//
// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-enforce=lifetime -o - %s | FileCheck %s --check-prefixes=CHECK,ENABLED
// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-apply=lifetime -o - %s | FileCheck %s --check-prefixes=CHECK,ENABLED
// RUN: %clang_cc1 -emit-llvm -std=c++20 -o - %s | FileCheck %s --check-prefixes=CHECK,DISABLED

struct S {
  int x;
  int y;
  S *next;
};

// =====================================================================
// Basic dereference: *p — profile-checked by std::lifetime
// Both enforce and apply modes emit the runtime check.
// No profile enabled → no check.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_deref
// ENABLED: icmp ne ptr %{{.*}}, null
// ENABLED-NEXT: br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED: profile.trap:
// ENABLED-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED-NEXT: unreachable
// ENABLED: profile.cont:
// DISABLED-NOT: profile.trap
int test_deref(int *p) {
  return *p;
}

// =====================================================================
// Arrow member access: p->x — same check
// =====================================================================

// CHECK-LABEL: define {{.*}}test_arrow
// ENABLED: icmp ne ptr %{{.*}}, null
// ENABLED-NEXT: br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED: profile.trap:
// ENABLED-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED-NEXT: unreachable
// ENABLED: profile.cont:
// DISABLED-NOT: profile.trap
int test_arrow(S *p) {
  return p->x;
}

// =====================================================================
// Write through pointer: *p = val
// =====================================================================

// CHECK-LABEL: define {{.*}}test_assign
// ENABLED: icmp ne ptr %{{.*}}, null
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// DISABLED-NOT: profile.trap
void test_assign(int *p, int val) {
  *p = val;
}

// =====================================================================
// Nested dereference: **pp — two null checks (one per deref)
// =====================================================================

// CHECK-LABEL: define {{.*}}test_nested
// ENABLED: icmp ne ptr %{{.*}}, null
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED: profile.cont:
// ENABLED: icmp ne ptr %{{.*}}, null
// ENABLED: profile.trap{{[0-9]*}}:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// DISABLED-NOT: profile.trap
int test_nested(int **pp) {
  return **pp;
}

// =====================================================================
// Multiple dereferences in one function — each gets its own check
// =====================================================================

// CHECK-LABEL: define {{.*}}test_multiple
// ENABLED: icmp ne ptr
// ENABLED: profile.trap:
// ENABLED: icmp ne ptr
// ENABLED: profile.trap{{[0-9]*}}:
// DISABLED-NOT: profile.trap
int test_multiple(int *a, int *b) {
  return *a + *b;
}

// =====================================================================
// Chained arrow: p->next->x — two null checks
// =====================================================================

// CHECK-LABEL: define {{.*}}test_chained
// ENABLED: icmp ne ptr
// ENABLED: profile.trap:
// ENABLED: profile.cont:
// ENABLED: icmp ne ptr
// ENABLED: profile.trap{{[0-9]*}}:
// DISABLED-NOT: profile.trap
int test_chained(S *p) {
  return p->next->x;
}

// =====================================================================
// Suppress on a single statement
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_single
// ENABLED-NOT: profile.trap
// ENABLED: }
int test_suppress_single(int *p) {
  [[profiles::suppress(std::lifetime)]]
  return *p;
}

// =====================================================================
// Suppress on a compound statement: only the block is suppressed,
// code outside gets checked.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_compound
// ENABLED: icmp ne ptr
// ENABLED: profile.trap:
// ENABLED-NOT: profile.trap
// ENABLED: }
int test_suppress_compound(int *a, int *b) {
  int x = *a;
  [[profiles::suppress(std::lifetime)]] {
    x += *b;
  }
  return x;
}

// =====================================================================
// Suppress only lifetime — unrelated profiles unaffected
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_only_life
// ENABLED-NOT: icmp ne ptr
// ENABLED-NOT: profile.trap
// ENABLED: }
int test_suppress_only_life(int *p) {
  [[profiles::suppress(std::lifetime)]] {
    return *p;
  }
}

// =====================================================================
// 'this' pointer in a member function — should NOT be null-checked
// because it is always assumed non-null.
// =====================================================================

struct Widget {
  int val;
  int get() const;
};

int Widget::get() const { return this->val; }

// CHECK-LABEL: define {{.*}}Widget{{.*}}get
// CHECK-NOT: profile.trap
// CHECK: }

// =====================================================================
// Dereference inside a loop — check emitted on each iteration
// =====================================================================

// CHECK-LABEL: define {{.*}}test_loop
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// DISABLED-NOT: profile.trap
int test_loop(int *p, int n) {
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += *p;
  return sum;
}

// =====================================================================
// Suppress does NOT persist outside its statement scope.
// The second deref (*b) is NOT suppressed.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_no_persist
// ENABLED: icmp ne ptr
// ENABLED: profile.trap:
// ENABLED: profile.cont:
// ENABLED: icmp ne ptr
// ENABLED: profile.trap{{[0-9]*}}:
int test_suppress_no_persist(int *a, int *b) {
  [[profiles::suppress(std::lifetime)]]
  int x = *a;
  return x + *b;
}
