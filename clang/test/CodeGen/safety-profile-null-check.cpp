// Comprehensive CodeGen tests for P3081R2 §6.2 null dereference profile checks.
//
// P3081R2 §3.2: "A condition C at program point Q being profile-checked by
//   profile P means: If P is enabled at Q, C is evaluated and this evaluation
//   is an implicit contract assertion; if it does not evaluate to true, then
//   std::profile_violation(PP) is invoked where PP is the detection_mode
//   value corresponding to P."
//
// P3081R2 §6.2: "the condition indirection_not_null(p) is profile-checked
//   by profile std::lifetime"
//
// P3081R2 [dcl.attr.profile]: "At a program point Q, a profile P is suppressed
//   if any enclosing scope of Q has the profile attribute
//   profiles::suppress(P)."
//
// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-enforce=lifetime -o - %s | FileCheck %s --check-prefixes=CHECK,ENABLED
// RUN: %clang_cc1 -emit-llvm -std=c++20 -o - %s | FileCheck %s --check-prefixes=CHECK,DISABLED

struct S {
  int x;
  int y;
  S *next;
};

// =====================================================================
// §6.2: *p is profile-checked — enforce emits the check.
// detection_mode::lifetime == 2.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_deref
// ENABLED:       %[[NONNULL:.*]] = icmp ne ptr %{{.*}}, null
// ENABLED-NEXT:  br i1 %[[NONNULL]], label %profile.cont, label %profile.trap
// ENABLED:      profile.trap:
// ENABLED-NEXT:  call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED-NEXT:  unreachable
// ENABLED:      profile.cont:
// DISABLED-NOT: profile.trap
int test_deref(int *p) {
  return *p;
}

// =====================================================================
// §6.2: p->x is syntactic sugar for (*p).x — same null check.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_arrow
// ENABLED:       %[[NONNULL:.*]] = icmp ne ptr %{{.*}}, null
// ENABLED-NEXT:  br i1 %[[NONNULL]], label %profile.cont, label %profile.trap
// ENABLED:      profile.trap:
// ENABLED-NEXT:  call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED-NEXT:  unreachable
// ENABLED:      profile.cont:
// DISABLED-NOT: profile.trap
int test_arrow(S *p) {
  return p->x;
}

// =====================================================================
// The check fires for writes through a pointer too.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_write
// ENABLED:      icmp ne ptr %{{.*}}, null
// ENABLED:      profile.trap:
// ENABLED-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// DISABLED-NOT: profile.trap
void test_write(int *p, int val) {
  *p = val;
}

// =====================================================================
// **pp: two dereferences, two independent checks.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_nested
// ENABLED:      icmp ne ptr %{{.*}}, null
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED:      profile.cont:
// ENABLED:      icmp ne ptr %{{.*}}, null
// ENABLED:      profile.trap{{[0-9]+}}:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 2)
// DISABLED-NOT: profile.trap
int test_nested(int **pp) {
  return **pp;
}

// =====================================================================
// *a + *b: two separate checks.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_two_ptrs
// ENABLED:      icmp ne ptr
// ENABLED:      profile.trap:
// ENABLED:      profile.cont:
// ENABLED:      icmp ne ptr
// ENABLED:      profile.trap{{[0-9]+}}:
// DISABLED-NOT: profile.trap
int test_two_ptrs(int *a, int *b) {
  return *a + *b;
}

// =====================================================================
// p->next->x: two arrow accesses, two checks.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_chained
// ENABLED:      icmp ne ptr
// ENABLED:      profile.trap:
// ENABLED:      profile.cont:
// ENABLED:      icmp ne ptr
// ENABLED:      profile.trap{{[0-9]+}}:
// DISABLED-NOT: profile.trap
int test_chained(S *p) {
  return p->next->x;
}

// =====================================================================
// Suppression §2.1: suppress on a single statement.
// [[profiles::suppress(std::lifetime)]] return *p;
// The return statement is the attributed statement's sub-statement,
// so the dereference is inside the suppressed scope → no check.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_stmt
// ENABLED-NOT: profile.trap
// ENABLED:     ret i32
int test_suppress_stmt(int *p) {
  [[profiles::suppress(std::lifetime)]]
  return *p;
}

// =====================================================================
// Suppression §2.1: suppress on a compound statement.
// The first deref (*a) is outside → checked.
// The second deref (*b) is inside the suppress block → not checked.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_block
// ENABLED:      icmp ne ptr
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED:      profile.cont:
// ENABLED-NOT:  profile.trap
// ENABLED:      ret i32
int test_suppress_block(int *a, int *b) {
  int x = *a;
  [[profiles::suppress(std::lifetime)]] {
    x += *b;
  }
  return x;
}

// =====================================================================
// Suppression does NOT persist past the attributed statement.
// Only the first deref is suppressed; the second is checked.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_scope
// ENABLED-NOT:  profile.trap
// ENABLED:      icmp ne ptr
// ENABLED:      profile.trap
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 2)
int test_suppress_scope(int *a, int *b) {
  [[profiles::suppress(std::lifetime)]] {
    (void)*a;
  }
  return *b;
}

// =====================================================================
// Suppressing std::lifetime does not suppress std::bounds or others.
// Only the lifetime null check is suppressed; the code otherwise
// compiles normally with no lifetime check.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_granular
// ENABLED-NOT: profile.trap
// ENABLED:     ret i32
int test_suppress_granular(int *p) {
  [[profiles::suppress(std::lifetime)]] {
    return *p;
  }
}

// =====================================================================
// 'this' pointer: the paper notes the compiler may elide the check
// when it statically determines p is non-null. this is always non-null,
// and our implementation skips this + DeclRefExpr bases for arrow.
// =====================================================================

struct Widget {
  int val;
  int get() const;
};

int Widget::get() const { return this->val; }

// CHECK-LABEL: define {{.*}}Widget{{.*}}get
// CHECK-NOT:   profile.trap
// CHECK:       ret i32

// =====================================================================
// Check inside a loop body.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_loop
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 2)
// DISABLED-NOT: profile.trap
int test_loop(int *p, int n) {
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += *p;
  return sum;
}

// =====================================================================
// No profile enabled → no checks anywhere.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_no_profile
// DISABLED-NOT: profile.trap
// DISABLED:     ret i32
int test_no_profile(int *p) {
  return *p;
}
