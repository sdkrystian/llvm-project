// REQUIRES: asserts
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fprofiles-test-profiles -std=c++23 -print-stats %s 2>&1 | FileCheck %s

// Each suppression dominion creates a fixed number of diagnostic states,
// including one that begins in a token-cached body parsed after the
// dominions of the later members: the count grows linearly with the number
// of dominions.
// CHECK: 48 diagnostic - Number of diagnostic states created by positional replacements

[[profiles::enforce(test::type_cast)]];

#define RC (void)reinterpret_cast<int *>(0L)

// Eight late-parsed statement suppressions before eight suppressed members
struct S {
  void n1() { [[profiles::suppress(test::type_cast)]] RC; }
  void n2() { [[profiles::suppress(test::type_cast)]] RC; }
  void n3() { [[profiles::suppress(test::type_cast)]] RC; }
  void n4() { [[profiles::suppress(test::type_cast)]] RC; }
  void n5() { [[profiles::suppress(test::type_cast)]] RC; }
  void n6() { [[profiles::suppress(test::type_cast)]] RC; }
  void n7() { [[profiles::suppress(test::type_cast)]] RC; }
  void n8() { [[profiles::suppress(test::type_cast)]] RC; }
  void m1 [[profiles::suppress(test::type_cast)]] () { RC; }
  void m2 [[profiles::suppress(test::type_cast)]] () { RC; }
  void m3 [[profiles::suppress(test::type_cast)]] () { RC; }
  void m4 [[profiles::suppress(test::type_cast)]] () { RC; }
  void m5 [[profiles::suppress(test::type_cast)]] () { RC; }
  void m6 [[profiles::suppress(test::type_cast)]] () { RC; }
  void m7 [[profiles::suppress(test::type_cast)]] () { RC; }
  void m8 [[profiles::suppress(test::type_cast)]] () { RC; }
};
