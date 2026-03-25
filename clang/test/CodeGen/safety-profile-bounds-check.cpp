// Comprehensive CodeGen tests for P3081R2 §5.3 subscript bounds profile checks.
//
// P3081R2 [dcl.attr.profile]: "At a program point Q, a profile P is suppressed
//   if any enclosing scope of Q has the profile attribute
//   profiles::suppress(P)."
//
// The bounds profile rejects array-to-pointer decay at Sema level when enforced.
// Use 'apply' mode (warnings are DefaultIgnore) so the code compiles to IR
// while still emitting runtime bounds checks.
//
// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-apply=bounds -o - %s | FileCheck %s --check-prefixes=CHECK,ENABLED
// RUN: %clang_cc1 -emit-llvm -std=c++20 -o - %s | FileCheck %s --check-prefixes=CHECK,DISABLED

// =====================================================================
// Signed index on a constant-size array
// =====================================================================

// CHECK-LABEL: define {{.*}}test_signed_index
// ENABLED: %[[IDX:.*]] = load i32,
// ENABLED: icmp slt i32 %[[IDX]], 10
// ENABLED: icmp sge i32 %[[IDX]], 0
// ENABLED: and i1
// ENABLED: br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED: profile.trap:
// ENABLED-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// ENABLED-NEXT: unreachable
// ENABLED: profile.cont:
// DISABLED-NOT: profile.trap
int test_signed_index(int i) {
  int arr[10] = {};
  return arr[i];
}

// =====================================================================
// Unsigned index on a constant-size array — no signed check needed
// =====================================================================

// CHECK-LABEL: define {{.*}}test_unsigned_index
// ENABLED: %[[IDX:.*]] = load i32,
// ENABLED: icmp ult i32 %[[IDX]], 4
// ENABLED: br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED: profile.trap:
// ENABLED-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// ENABLED-NEXT: unreachable
// ENABLED: profile.cont:
// DISABLED-NOT: profile.trap
int test_unsigned_index(unsigned i) {
  int arr[4] = {};
  return arr[i];
}

// =====================================================================
// Write to array element: arr[i] = val
// =====================================================================

// CHECK-LABEL: define {{.*}}test_array_write
// ENABLED: icmp ult i32
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
void test_array_write(int val, unsigned i) {
  int arr[8] = {};
  arr[i] = val;
}

// =====================================================================
// Different array element types
// =====================================================================

// CHECK-LABEL: define {{.*}}test_double_array
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
double test_double_array(int i) {
  double arr[5] = {};
  return arr[i];
}

// CHECK-LABEL: define {{.*}}test_char_array
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
char test_char_array(int i) {
  char arr[16] = {};
  return arr[i];
}

// =====================================================================
// Array of structs
// =====================================================================

struct S { int x; int y; };

// CHECK-LABEL: define {{.*}}test_struct_array
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
int test_struct_array(int i) {
  S arr[3] = {};
  return arr[i].x;
}

// =====================================================================
// Suppress on a single statement
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_single
// ENABLED-NOT: profile.trap
// ENABLED: }
int test_suppress_single(int i) {
  int arr[10] = {};
  [[profiles::suppress(std::bounds)]]
  return arr[i];
}

// =====================================================================
// Suppress on a compound statement: only the block is suppressed,
// code outside gets checked.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_compound
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// ENABLED: profile.cont:
int test_suppress_compound(int i, int j) {
  int arr[10] = {};
  int x = arr[i];
  [[profiles::suppress(std::bounds)]] {
    x += arr[j];
  }
  return x;
}

// =====================================================================
// Suppress does NOT persist outside its statement scope.
// The second access is NOT suppressed.
// =====================================================================

// CHECK-LABEL: define {{.*}}test_suppress_no_persist
// ENABLED: profile.trap:
// ENABLED: profile.cont:
// ENABLED: profile.trap{{[0-9]*}}:
int test_suppress_no_persist(int i, int j) {
  int arr[10] = {};
  [[profiles::suppress(std::bounds)]]
  int x = arr[i];
  return x + arr[j];
}

// =====================================================================
// Pointer subscript: p[i] does NOT get a profile bounds check
// (no ConstantArrayType, no size info available)
// =====================================================================

// CHECK-LABEL: define {{.*}}test_pointer
// CHECK-NOT: profile.trap
// CHECK: }
int test_pointer(int *p, int i) {
  return p[i];
}

// =====================================================================
// Multiple subscripts in one function — each gets its own check
// =====================================================================

// CHECK-LABEL: define {{.*}}test_multi_access
// ENABLED: profile.trap:
// ENABLED: profile.cont:
// ENABLED: profile.trap{{[0-9]*}}:
// ENABLED: profile.cont{{[0-9]*}}:
// DISABLED-NOT: profile.trap
int test_multi_access(int i, int j) {
  int a[5] = {};
  int b[3] = {};
  return a[i] + b[j];
}

// =====================================================================
// Array access in a loop — check emitted each iteration
// =====================================================================

// CHECK-LABEL: define {{.*}}test_loop
// ENABLED: profile.trap:
// ENABLED: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// DISABLED-NOT: profile.trap
int test_loop(int *out, int n) {
  int arr[100] = {};
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += arr[i];
  return sum;
}

// =====================================================================
// Size-1 array — valid index is only 0
// =====================================================================

// CHECK-LABEL: define {{.*}}test_size_one
// ENABLED: icmp slt i32 %{{.*}}, 1
// ENABLED: profile.trap:
// DISABLED-NOT: profile.trap
int test_size_one(int i) {
  int arr[1] = {};
  return arr[i];
}
