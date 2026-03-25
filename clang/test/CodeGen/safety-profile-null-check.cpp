// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-enforce=lifetime -o - %s | FileCheck %s
// RUN: %clang_cc1 -emit-llvm -std=c++20 -fsafety-profile-apply=lifetime -o - %s | FileCheck %s

struct S {
  int x;
};

// CHECK-LABEL: define {{.*}} @_Z10test_derefPi(
// CHECK: %[[NONNULL:.*]] = icmp ne ptr %{{.*}}, null
// CHECK-NEXT: br i1 %[[NONNULL]], label %profile.cont, label %profile.trap
// CHECK: profile.trap:
// CHECK-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// CHECK-NEXT: unreachable
// CHECK: profile.cont:
int test_deref(int *p) {
  return *p;
}

// CHECK-LABEL: define {{.*}} @_Z10test_arrowP1S(
// CHECK: %[[NONNULL:.*]] = icmp ne ptr %{{.*}}, null
// CHECK-NEXT: br i1 %[[NONNULL]], label %profile.cont, label %profile.trap
// CHECK: profile.trap:
// CHECK-NEXT: call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// CHECK-NEXT: unreachable
// CHECK: profile.cont:
int test_arrow(S *p) {
  return p->x;
}

// CHECK-LABEL: define {{.*}} @_Z13test_suppressPi(
// CHECK-NOT: icmp ne ptr
// CHECK-NOT: profile.trap
// CHECK: }
int test_suppress(int *p) {
  [[profiles::suppress(std::lifetime)]]
  return *p;
}
