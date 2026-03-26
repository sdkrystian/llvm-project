// Comprehensive CodeGen tests for P3081R2 §5.3 subscript bounds profile checks.
//
// P3081R2 §5.3: "the condition index_in_range(a,b) is profile-checked by
//   profile std::bounds"
//
// P3081R2 §3.2: "If P is enabled at Q, C is evaluated ... if it does not
//   evaluate to true, then std::profile_violation(PP) is invoked where PP is
//   the detection_mode value corresponding to P."
//   detection_mode::bounds == 1.
//
// Array subscripts on C arrays involve array-to-pointer decay, which is
// profile-rejected at the Sema level.  We suppress that specific rule via
// rule: "conv.array" so that the runtime bounds checks can still be tested.
//
// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-enforce=bounds -o - %s | FileCheck %s --check-prefixes=CHECK,ENABLED
// RUN: %clang_cc1 -emit-llvm -std=c++20 -o - %s | FileCheck %s --check-prefixes=CHECK,DISABLED

// =====================================================================
// Signed index: checks 0 <= idx && idx < N.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_signed
// ENABLED:       %[[IDX:.*]] = load i32,
// ENABLED-DAG:   icmp slt i32 %[[IDX]], 10
// ENABLED-DAG:   icmp sge i32 %[[IDX]], 0
// ENABLED:       and i1
// ENABLED:       br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED:      profile.trap:
// ENABLED-NEXT:  call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// ENABLED-NEXT:  unreachable
// ENABLED:      profile.cont:
// DISABLED-NOT: profile.trap
int test_signed(int i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[10] = {};
  return arr[i];
  }
}

// =====================================================================
// Unsigned index: only checks idx < N (no negative check needed).
// =====================================================================

// CHECK-LABEL: define {{.*}}test_unsigned
// ENABLED:       %[[IDX:.*]] = load i32,
// ENABLED:       icmp ult i32 %[[IDX]], 4
// ENABLED-NOT:   icmp sge
// ENABLED:       br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED:      profile.trap:
// ENABLED-NEXT:  call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// ENABLED-NEXT:  unreachable
// ENABLED:      profile.cont:
// DISABLED-NOT: profile.trap
int test_unsigned(unsigned i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[4] = {};
  return arr[i];
  }
}

// =====================================================================
// Write: arr[i] = val — check on the store side too.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_write
// ENABLED:      icmp ult i32
// ENABLED:      profile.trap:
// ENABLED-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
void test_write(int val, unsigned i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[8] = {};
  arr[i] = val;
  }
}

// =====================================================================
// Different element types: double, char, struct.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_double
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
double test_double(int i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  double arr[5] = {};
  return arr[i];
  }
}

// CHECK-LABEL: define {{.*}}test_char
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
char test_char(int i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  char arr[16] = {};
  return arr[i];
  }
}

struct S { int x; int y; };

// CHECK-LABEL: define {{.*}}test_struct
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
int test_struct(int i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  S arr[3] = {};
  return arr[i].x;
  }
}

// =====================================================================
// Size-1 array — only index 0 is valid.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_size_one
// ENABLED:      icmp slt i32 %{{.*}}, 1
// ENABLED:      profile.trap:
// DISABLED-NOT: profile.trap
int test_size_one(int i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[1] = {};
  return arr[i];
  }
}

// =====================================================================
// Two arrays, two accesses — each gets its own check.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_multi
// ENABLED:      profile.trap:
// ENABLED:      profile.cont:
// ENABLED:      profile.trap{{[0-9]+}}:
// ENABLED:      profile.cont{{[0-9]+}}:
// DISABLED-NOT: profile.trap
int test_multi(int i, int j) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int a[5] = {};
  int b[3] = {};
  return a[i] + b[j];
  }
}

// =====================================================================
// Array access in a loop.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_loop
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
int test_loop(int n) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[100] = {};
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += arr[i];
  return sum;
  }
}

// =====================================================================
// Pointer subscript: p[i] — no ConstantArrayType, no check.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_pointer
// CHECK-NOT:   profile.trap
// CHECK:       ret i32
int test_pointer(int *p, int i) {
  return p[i];
}

// =====================================================================
// Suppression §2.1: suppress on a single statement.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_stmt
// ENABLED-NOT: profile.trap
// ENABLED:     ret i32
int test_suppress_stmt(int i) {
  [[profiles::suppress(std::bounds)]] {
  int arr[10] = {};
  return arr[i];
  }
}

// =====================================================================
// Suppression §2.1: suppress on a compound statement.
// The first access (arr[i]) is outside → checked.
// The second access (arr[j]) is inside the block → not checked.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_block
// ENABLED:      profile.trap:
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 1)
// ENABLED:      profile.cont:
// ENABLED-NOT:  profile.trap
// ENABLED:      ret i32
int test_suppress_block(int i, int j) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[10] = {};
  int x = arr[i];
  [[profiles::suppress(std::bounds)]] {
    x += arr[j];
  }
  return x;
  }
}

// =====================================================================
// Suppression does NOT persist past the attributed statement.
// Only the first access is suppressed; the second is checked.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_scope
// ENABLED-NOT:  profile.trap
// ENABLED:      icmp
// ENABLED:      profile.trap
// ENABLED:      @_ZSt17profile_violationSt14detection_mode(i32 1)
int test_suppress_scope(int i, int j) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[10] = {};
  [[profiles::suppress(std::bounds)]] {
    (void)arr[i];
  }
  return arr[j];
  }
}

// =====================================================================
// No profile enabled → no checks anywhere.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_no_profile
// DISABLED-NOT: profile.trap
// DISABLED:     ret i32
int test_no_profile(int i) {
  [[profiles::suppress(std::bounds, rule: "conv.array")]] {
  int arr[10] = {};
  return arr[i];
  }
}
