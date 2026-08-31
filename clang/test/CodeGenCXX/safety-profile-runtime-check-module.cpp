// A pattern-5 runtime profile check (test::arith / zero_divide) in a named
// module unit: the unit's enforcements -- advertised on the module-declaration
// or recorded TU-locally by a purview empty-declaration -- are restored when
// the unit is code-generated from its BMI, so both compilation paths emit the
// same checks, while an importer of the module stays unenforced. A BMI
// compile takes its language options from the BMI, so the profile flags are
// given only when the BMI is built.

// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
//
// From source.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fprofiles-test-profiles -emit-llvm -o - %t/m.cppm | FileCheck %s --check-prefix=ADVERTISED
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fprofiles-test-profiles -emit-llvm -o - %t/n.cppm | FileCheck %s --check-prefix=LOCAL
//
// From the BMI.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fprofiles-test-profiles -emit-module-interface -o %t/m.pcm %t/m.cppm
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -emit-llvm -o - %t/m.pcm | FileCheck %s --check-prefix=ADVERTISED
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fprofiles-test-profiles -emit-module-interface -o %t/n.pcm %t/n.cppm
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -emit-llvm -o - %t/n.pcm | FileCheck %s --check-prefix=LOCAL
//
// Importing an enforcing module does not enforce its profiles in the importer.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fprofiles-test-profiles -fmodule-file=M=%t/m.pcm -emit-llvm -o - %t/use.cpp | FileCheck %s --check-prefix=IMPORTER
//
// Global-module-fragment code precedes the enforcement, so it is outside the
// dominion on both compilation paths.
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fprofiles-test-profiles -emit-llvm -o - %t/g.cppm | FileCheck %s --check-prefix=GMF
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fprofiles-test-profiles -emit-module-interface -o %t/g.pcm %t/g.cppm
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -emit-llvm -o - %t/g.pcm | FileCheck %s --check-prefix=GMF

//--- m.cppm
export module M [[profiles::enforce(test::arith)]];
// ADVERTISED-LABEL: define {{.*}} @_ZW1M4mdivii(
// ADVERTISED: call void @llvm.ubsantrap(i8
export int mdiv(int a, int b) { return a / b; }

//--- n.cppm
export module N;
[[profiles::enforce(test::arith)]];
// LOCAL-LABEL: define {{.*}} @_ZW1N4nremii(
// LOCAL: call void @llvm.ubsantrap(i8
export int nrem(int a, int b) { return a % b; }

//--- use.cpp
import M;
// IMPORTER-LABEL: define {{.*}} @_Z3useii(
// IMPORTER-NOT: llvm.ubsantrap
// IMPORTER: ret i32
int use(int a, int b) { return mdiv(a, b) / b; }

//--- g.cppm
// Neither the eagerly emitted nor the deferred inline GMF function is
// checked, while a GMF-defined macro invoked in the purview still is: the
// dominion compares expansion locations, and the invocation tokens are
// purview tokens.
module;
#define GMF_DIV(x, y) ((x) / (y))
int gmfdiv(int a, int b) { return a / b; }
inline int gmf_inline_div(int a, int b) { return a / b; }
export module G [[profiles::enforce(test::arith)]];
// GMF-LABEL: define {{.*}} @_Z6gmfdivii(
// GMF-NOT: call void @llvm.ubsantrap
// GMF-LABEL: define {{.*}} @_ZW1G4guseii(
// GMF: call void @llvm.ubsantrap(i8
// GMF-LABEL: define {{.*}} @_Z14gmf_inline_divii(
// GMF-NOT: call void @llvm.ubsantrap
// GMF: declare void @llvm.ubsantrap
export int guse(int a, int b) {
  return gmfdiv(a, b) + gmf_inline_div(a, b) + GMF_DIV(a, b);
}
