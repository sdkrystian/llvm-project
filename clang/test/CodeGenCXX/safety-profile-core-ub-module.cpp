// A std::core_ub type check (null_dereference) in a named module unit: module
// code keeps the module's own enforcement wherever it is emitted, so an
// enforcing module's inline function emitted in a non-enforcing importer is
// checked while the importer's own code is not; and a diagnostic option in
// the importer previews compile-time rules only, instrumenting nothing.
// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -emit-module-interface -o %t/m.pcm %t/m.cppm
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -fmodule-file=M=%t/m.pcm -emit-llvm -o - %t/use.cpp | FileCheck %s --check-prefix=IMPORTER
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -emit-module-interface -o %t/n.pcm %t/n.cppm
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++23 -fprofiles -Wprofile-std-core-ub -fmodule-file=N=%t/n.pcm -emit-llvm -o - %t/use_n.cpp | FileCheck %s --check-prefix=NONE

// NONE-NOT: llvm.ubsantrap

//--- m.cppm
export module M [[profiles::enforce(std::core_ub)]];
export inline int mderef(int *p) { return *p; }

//--- use.cpp
import M;
// IMPORTER-LABEL: define {{.*}} @_Z3usePi(
// IMPORTER-NOT: llvm.ubsantrap
// IMPORTER: ret i32
// IMPORTER-LABEL: define {{.*}} @_ZW1M6mderefPi(
// IMPORTER: icmp ne ptr {{.*}}, null
// IMPORTER: call void @llvm.ubsantrap(i8
int use(int *p) { return *p + mderef(p); }

//--- n.cppm
export module N;
export inline int nderef(int *p) { return *p; }

//--- use_n.cpp
import N;
int use_n(int *p) { return *p + nderef(p); }
