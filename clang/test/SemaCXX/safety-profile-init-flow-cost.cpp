// REQUIRES: asserts
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -fcxx-exceptions -fexceptions -std=c++23 -DPOINTERS=8 %s
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fcxx-exceptions -fexceptions -std=c++23 -print-stats -DPOINTERS=4 %s 2>&1 | FileCheck %s --check-prefix=POINTERS64
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fcxx-exceptions -fexceptions -std=c++23 -print-stats -DPOINTERS=8 %s 2>&1 | FileCheck %s --check-prefix=POINTERS128
// expected-no-diagnostics

// The std::init flow engine's cost grows linearly with the body: a reseat
// checks only the pointers that can have copied the replaced referent.

[[profiles::enforce(std::init)]];

void use(int *p [[ref_to_uninit]]);

#define REPEAT4(M, n) M(n##0) M(n##1) M(n##2) M(n##3)
#define REPEAT16(M, n) REPEAT4(M, n##0) REPEAT4(M, n##1) REPEAT4(M, n##2) REPEAT4(M, n##3)

// Marked pointers, each copied once and reseated to a fresh allocation
// POINTERS64: 256 std-init-flow - Number of std::init pointers checked for a replaced referent
// POINTERS128: 512 std-init-flow - Number of std::init pointers checked for a replaced referent
#ifdef POINTERS
#define POINTER(n)                                                             \
  int *p##n [[ref_to_uninit]] = new int;                                       \
  int *q##n [[ref_to_uninit]] = p##n;                                          \
  use(q##n);                                                                   \
  p##n = new int;
void pointers() {
  REPEAT16(POINTER, 1)
  REPEAT16(POINTER, 2)
  REPEAT16(POINTER, 3)
  REPEAT16(POINTER, 4)
#if POINTERS > 4
  REPEAT16(POINTER, 5)
  REPEAT16(POINTER, 6)
  REPEAT16(POINTER, 7)
  REPEAT16(POINTER, 8)
#endif
}
#endif
