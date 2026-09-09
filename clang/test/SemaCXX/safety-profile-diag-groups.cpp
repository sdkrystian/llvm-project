// Every profile rule diagnostic belongs to its rule's diagnostic group, and
// enforcement is the group's mapping, so the ordinary diagnostic controls
// apply to it: the message names the group, -Wprofile-... fires the rules as
// warnings without an enforcement, -Weverything does not, an enforced rule
// survives -w and -Wno-profiles, and diagnostic pragmas move a rule's
// severity while an enforcement outlives a push/pop pair.

// RUN: not %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -std=c++23 %s 2>&1 | FileCheck %s --check-prefix=OPTION
// RUN: not %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -fno-diagnostics-show-option -std=c++23 %s 2>&1 | FileCheck %s --check-prefix=NOOPTION
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -Wprofile-test-type-cast -std=c++23 -DNO_ENFORCE %s 2>&1 | FileCheck %s --check-prefix=WARN
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -Weverything -std=c++23 -DNO_ENFORCE %s 2>&1 | FileCheck %s --check-prefix=NONE --allow-empty
// RUN: not %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -w -std=c++23 %s 2>&1 | FileCheck %s --check-prefix=OPTION
// RUN: not %clang_cc1 -fsyntax-only -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -Wno-profiles -std=c++23 -DNO_ENFORCE %s 2>&1 | FileCheck %s --check-prefix=OPTION
// RUN: not %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -std=c++23 -DPUSH_POP %s 2>&1 | FileCheck %s --check-prefix=OPTION
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++23 -DPRAGMAS %s

#ifndef NO_ENFORCE
[[profiles::enforce(test::type_cast)]];
#endif
#ifdef PUSH_POP
// A push lexed right behind the enforce declaration saves a state the
// enforcement still reaches, so the pop below restores an enforced state.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic pop
#endif

long l;
// OPTION: error: 'reinterpret_cast' is unsafe under profile 'test::type_cast' [-Wprofile-test-type-cast-reinterpret-cast]
// NOOPTION: error: 'reinterpret_cast' is unsafe under profile 'test::type_cast'{{$}}
// WARN: warning: 'reinterpret_cast' is unsafe under profile 'test::type_cast' [-Wprofile-test-type-cast-reinterpret-cast]
// NONE-NOT: {{(warning|error): 'reinterpret_cast' is unsafe}}
int *p = reinterpret_cast<int *>(l); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}

#ifdef PRAGMAS
#pragma clang diagnostic ignored "-Wprofile-test-type-cast-reinterpret-cast"
int *p_ignored = reinterpret_cast<int *>(l);
#pragma clang diagnostic warning "-Wprofile-test-type-cast"
int *p_warning = reinterpret_cast<int *>(l); // expected-warning {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
#pragma clang diagnostic error "-Wprofile-test-type-cast"
int *p_error = reinterpret_cast<int *>(l); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprofiles"
int *p_pushed = reinterpret_cast<int *>(l);
#pragma clang diagnostic pop
int *p_popped = reinterpret_cast<int *>(l); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
#endif
