// Test this without pch.
// RUN: %clang_cc1 %s -fprofiles -fprofiles-test-profiles -std=c++20 -fsyntax-only -include %s -verify

// Test with pch.
// RUN: %clang_cc1 %s -fprofiles -fprofiles-test-profiles -std=c++20 -emit-pch -o %t
// RUN: %clang_cc1 %s -fprofiles -fprofiles-test-profiles -std=c++20 -fsyntax-only -include-pch %t -verify

// A header containing a non-empty declaration must fail the main file's
// enforce placement check -- through the parsed-decl walk (with a note) when
// textually included, and through the PCH's has-non-empty-decl bit (no
// deserialization, so no note) when included as a PCH.
// RUN: %clang_cc1 %s -DPCH_DECL -fprofiles -fprofiles-test-profiles -std=c++20 -fsyntax-only -include %s -verify=expected,after,afternote
// RUN: %clang_cc1 %s -DPCH_DECL -fprofiles -fprofiles-test-profiles -std=c++20 -emit-pch -o %t.decl
// RUN: %clang_cc1 %s -DPCH_DECL -fprofiles -fprofiles-test-profiles -std=c++20 -fsyntax-only -include-pch %t.decl -verify=expected,after

// A designator mismatch against the header's enforcement points its note at
// the header's attribute -- textually included and PCH-restored alike (the
// serialized enforcement carries its location).
// RUN: %clang_cc1 %s -DMISMATCH -fprofiles -fprofiles-test-profiles -std=c++20 -fsyntax-only -include %s -verify=expected,mismatch
// RUN: %clang_cc1 %s -DMISMATCH -fprofiles -fprofiles-test-profiles -std=c++20 -emit-pch -o %t.mismatch
// RUN: %clang_cc1 %s -DMISMATCH -fprofiles -fprofiles-test-profiles -std=c++20 -fsyntax-only -include-pch %t.mismatch -verify=expected,mismatch

#ifndef HEADER
#define HEADER

[[profiles::enforce(test::type_cast)]];

#ifdef PCH_DECL
int decl_in_pch; // afternote-note {{declaration declared here}}
#endif

#else

// Repeating the enforcement is valid after a header that contains only
// empty-declarations, and diagnosed when the header contributed a real
// declaration.
[[profiles::enforce(test::type_cast)]]; // after-error {{'profiles::enforce' attribute on empty-declaration must precede all non-empty declarations}}

#ifdef MISMATCH
// The note's absolute line is the header enforce; in the PCH runs the header
// text is never relexed, so the directive must live here.
// mismatch-error@+2 {{repeated enforcement of profile 'test::type_cast' with different designator}}
// mismatch-note@26 {{previous attribute is here}}
[[profiles::enforce(test::type_cast(strict: true))]];
#endif

void test() {
  int *p = reinterpret_cast<int*>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

#endif
