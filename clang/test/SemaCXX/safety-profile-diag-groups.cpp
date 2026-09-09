// Every profile rule diagnostic belongs to its rule's diagnostic group, so a
// violation names the group like any other grouped diagnostic, and
// -fno-diagnostics-show-option hides it.

// RUN: not %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -std=c++23 %s 2>&1 | FileCheck %s --check-prefix=OPTION
// RUN: not %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -fno-diagnostics-show-option -std=c++23 %s 2>&1 | FileCheck %s --check-prefix=NOOPTION

[[profiles::enforce(test::type_cast)]];

// OPTION: error: 'reinterpret_cast' is unsafe under profile 'test::type_cast' [-Wprofile-test-type-cast-reinterpret-cast]
// NOOPTION: error: 'reinterpret_cast' is unsafe under profile 'test::type_cast'{{$}}
int *p = reinterpret_cast<int *>(0);
