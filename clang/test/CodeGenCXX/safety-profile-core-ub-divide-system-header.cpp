// std::core_ub's runtime zero_divide check honors the temporary
// system-header exemption (stopgap for the not-yet-implemented
// [[profiles::exempt]], P3589R2 s1.1.6): by default no check is emitted for
// a division whose tokens lie in a system header, while a normal (-I)
// header's division is still checked; -fno-profiles-exempt-system-headers
// restores checks in system-header code.

// RUN: rm -rf %t
// RUN: split-file %s %t
//
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -std=c++23 -I %t -emit-llvm -o - %t/main.cpp | FileCheck %s --check-prefixes=CHECK,EXEMPT
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -fno-profiles-exempt-system-headers -std=c++23 -I %t -emit-llvm -o - %t/main.cpp | FileCheck %s --check-prefixes=CHECK,STRICT

//--- user.h
// A header reached via -I is not a system header: checked in both runs.
inline int user_div(int a, int b) { return a / b; }

//--- sys.h
#pragma clang system_header
// A system header: exempt by default, checked only under
// -fno-profiles-exempt-system-headers.
inline int sys_div(int a, int b) { return a / b; }

//--- main.cpp
[[profiles::enforce(std::core_ub)]];
#include "sys.h"
#include "user.h"

int use(int a, int b) { return sys_div(a, b) + user_div(a, b); }

// CHECK-LABEL: define {{.*}} @_Z3useii(
// CHECK-LABEL: define linkonce_odr {{.*}} @_Z7sys_divii(
// EXEMPT-NOT: llvm.ubsantrap
// STRICT: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// CHECK-LABEL: define linkonce_odr {{.*}} @_Z8user_divii(
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
