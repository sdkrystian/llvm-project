// A pattern-5 runtime profile check (test::arith / zero_divide) behind a PCH
// boundary: enforcement recorded in a PCH is restored into the including
// compile's ASTContext, so an inline function deserialized from the PCH is
// emitted with its runtime check, a [[profiles::suppress]] serialized in the
// PCH is honored, and the main TU's own code is checked under the restored
// enforcement -- including through a chained PCH.

// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
//
// Without PCH (headers included textually).
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -fprofiles-test-profiles -std=c++23 -include %t/first.h -include %t/second.h -emit-llvm -o - %t/main.cpp | FileCheck %s
//
// With one PCH.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -fprofiles-test-profiles -std=c++23 -x c++-header -emit-pch -o %t/first.pch %t/first.h
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -fprofiles-test-profiles -std=c++23 -include-pch %t/first.pch -include %t/second.h -emit-llvm -o - %t/main.cpp | FileCheck %s
//
// With a chained PCH.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -fprofiles-test-profiles -std=c++23 -x c++-header -include-pch %t/first.pch -emit-pch -o %t/second.pch %t/second.h
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fprofiles -fprofiles-test-profiles -std=c++23 -include-pch %t/second.pch -emit-llvm -o - %t/main.cpp | FileCheck %s

//--- first.h
[[profiles::enforce(test::arith)]];

inline int pch_div(int a, int b) { return a / b; }

[[profiles::suppress(test::arith)]] inline int pch_div_suppressed(int a,
                                                                   int b) {
  return a / b;
}

//--- second.h
// A second header, built on top of the first as a chained PCH in the last
// RUN configuration: the first PCH's enforcement must reach it and the final
// compile (and re-reading both files' ENFORCED_PROFILES records must not
// duplicate the entry).
inline int chained_div(int a, int b) { return a / b; }

//--- main.cpp
int use(int a, int b) {
  return pch_div(a, b) + pch_div_suppressed(a, b) + chained_div(a, b) +
         a / b;
}

// The main TU's own division is checked under the PCH-restored enforcement.
// CHECK-LABEL: define {{.*}} @_Z3useii(
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
//
// The PCH's inline function is emitted with its check; the suppressed one
// without; the chained header's with.
// CHECK-LABEL: define linkonce_odr {{.*}} @_Z7pch_divii(
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
// CHECK-LABEL: define linkonce_odr {{.*}} @_Z18pch_div_suppressedii(
// CHECK-NOT: llvm.ubsantrap
// CHECK: ret i32
// CHECK-LABEL: define linkonce_odr {{.*}} @_Z11chained_divii(
// CHECK: call void @llvm.ubsantrap(i8 {{[0-9]+}})
