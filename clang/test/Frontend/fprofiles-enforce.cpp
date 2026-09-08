// Each ill-formed name given to -fprofiles-enforce= is rejected individually;
// a profile-name is identifiers joined by "::".
// RUN: not %clang_cc1 -fsyntax-only -std=c++23 -fprofiles-enforce=std::,bad-name -fprofiles-enforce=::x,ok::name %s 2>&1 | FileCheck %s
// CHECK: error: invalid value 'std::' in '-fprofiles-enforce=std::,bad-name'
// CHECK-NEXT: error: invalid value 'bad-name' in '-fprofiles-enforce=std::,bad-name'
// CHECK-NEXT: error: invalid value '::x' in '-fprofiles-enforce=::x,ok::name'
// CHECK-NOT: error:

// Well-formed names are accepted, unknown and repeated ones included, and the
// option implies -fprofiles: the attributes are no longer ignored.
// RUN: %clang_cc1 -fsyntax-only -std=c++23 -fprofiles-enforce=acme::hardened,acme::hardened -fprofiles-enforce=acme::hardened -verify=enabled %s
// RUN: %clang_cc1 -fsyntax-only -std=c++23 -verify=disabled %s

// The option is C++-only, like -fprofiles: in C it is accepted and inert.
// RUN: %clang_cc1 -fsyntax-only -x c -std=c23 -fprofiles-enforce=std::init -verify=c %s

// enabled-no-diagnostics
// c-no-diagnostics
#ifdef __cplusplus
// disabled-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(acme::hardened)]];
#endif
