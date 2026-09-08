// A PCH built under -fprofiles-enforce= records its enforcements like any
// other, and its consumer has to be built under the same list: the option is
// a Compatible language option, so a differing list is a PCH mismatch in
// either direction. (A module built under the option imports into a compile
// with a different list; see safety-profile-framework-cmdline-modules.cppm.)

// RUN: %clang_cc1 %s -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -std=c++20 -emit-pch -o %t
// RUN: %clang_cc1 %s -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -std=c++20 -fsyntax-only -include-pch %t -verify
// RUN: %clang_cc1 %s -fprofiles-test-profiles -fprofiles-enforce=test::type_cast,test::type_cast -std=c++20 -fsyntax-only -include-pch %t -verify
// RUN: not %clang_cc1 %s -fprofiles -fprofiles-test-profiles -std=c++20 -fsyntax-only -include-pch %t 2>&1 | FileCheck %s
// RUN: not %clang_cc1 %s -fprofiles-test-profiles -fprofiles-enforce=test::other -std=c++20 -fsyntax-only -include-pch %t 2>&1 | FileCheck %s
// RUN: not %clang_cc1 %s -fprofiles-test-profiles -fprofiles-enforce=test::other,test::type_cast -std=c++20 -fsyntax-only -include-pch %t 2>&1 | FileCheck %s
// RUN: %clang_cc1 %s -fprofiles -fprofiles-test-profiles -std=c++20 -emit-pch -o %t.plain
// RUN: not %clang_cc1 %s -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -std=c++20 -fsyntax-only -include-pch %t.plain 2>&1 | FileCheck %s

// CHECK: error: enforced profiles differs in precompiled file '{{.*}}' vs. current file

#ifndef HEADER
#define HEADER

int in_pch(int);

#else

void test() {
  int *p = reinterpret_cast<int*>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

#endif
