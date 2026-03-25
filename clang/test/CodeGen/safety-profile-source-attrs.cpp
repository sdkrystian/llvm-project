// Tests that profile runtime checks (bounds, null) are emitted when profiles
// are enabled via source attributes rather than CLI flags.
//
// RUN: %clang_cc1 -emit-llvm -std=c++20 -o - %s | FileCheck %s --check-prefixes=CHECK,ENABLED
// RUN: %clang_cc1 -emit-llvm -std=c++20 -o - %s -DNO_ATTRS | FileCheck %s --check-prefixes=CHECK,DISABLED

#ifndef NO_ATTRS
[[profiles::apply(std::bounds)]];
[[profiles::apply(std::lifetime)]];
#endif

struct S {
  int x;
};

// CHECK-LABEL: define {{.*}}test_bounds
// ENABLED:       icmp ult i32
// ENABLED:       br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED:      profile.trap:
// ENABLED-NEXT:  call void @_ZSt17profile_violationSt14detection_mode(i32 1)
// ENABLED-NEXT:  unreachable
// ENABLED:      profile.cont:
// DISABLED-NOT: profile.trap
int test_bounds(unsigned i) {
  int arr[4] = {};
  return arr[i];
}

// CHECK-LABEL: define {{.*}}test_null_deref
// ENABLED:       icmp ne ptr %{{.*}}, null
// ENABLED:       br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED:      profile.trap:
// ENABLED-NEXT:  call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED-NEXT:  unreachable
// ENABLED:      profile.cont:
// DISABLED-NOT: profile.trap
int test_null_deref(int *p) {
  return *p;
}

// CHECK-LABEL: define {{.*}}test_null_arrow
// ENABLED:       icmp ne ptr %{{.*}}, null
// ENABLED:       br i1 %{{.*}}, label %profile.cont, label %profile.trap
// ENABLED:      profile.trap:
// ENABLED-NEXT:  call void @_ZSt17profile_violationSt14detection_mode(i32 2)
// ENABLED-NEXT:  unreachable
// ENABLED:      profile.cont:
// DISABLED-NOT: profile.trap
int test_null_arrow(S *p) {
  return p->x;
}
