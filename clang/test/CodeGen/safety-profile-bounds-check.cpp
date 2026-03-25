// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-apply=bounds -o - %s | FileCheck %s

// CHECK-LABEL: define {{.*}} @_Z14test_array_subi(
// CHECK: %[[IDX:.*]] = load i32,
// CHECK: %[[LT:.*]] = icmp slt i32 %[[IDX]], 10
// CHECK: %[[GE:.*]] = icmp sge i32 %[[IDX]], 0
// CHECK: %[[OK:.*]] = and i1 %[[GE]], %[[LT]]
// CHECK: br i1 %[[OK]], label %profile.cont, label %profile.trap
// CHECK: profile.trap:
// CHECK-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// CHECK-NEXT: unreachable
// CHECK: profile.cont:
int test_array_sub(int i) {
  int arr[10] = {};
  return arr[i];
}

// CHECK-LABEL: define {{.*}} @_Z19test_array_unsignedj(
// CHECK: %[[IDX:.*]] = load i32,
// CHECK: %[[LT:.*]] = icmp ult i32 %[[IDX]], 4
// CHECK: br i1 %[[LT]], label %profile.cont, label %profile.trap
// CHECK: profile.trap:
// CHECK-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 1)
int test_array_unsigned(unsigned i) {
  int arr[4] = {};
  return arr[i];
}

// CHECK-LABEL: define {{.*}} @_Z13test_suppressi(
// CHECK-NOT: profile.trap
// CHECK: }
int test_suppress(int i) {
  int arr[10] = {};
  [[profiles::suppress(std::bounds)]]
  return arr[i];
}

// Pointer subscript (not a ConstantArrayType) should NOT get a profile check.
// CHECK-LABEL: define {{.*}} @_Z12test_pointerPii(
// CHECK-NOT: profile.trap
// CHECK: }
int test_pointer(int *p, int i) {
  return p[i];
}
