// REQUIRES: asserts
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -fcxx-exceptions -fexceptions -std=c++23 -DLOCALS=4 -DPOINTERS=8 %s
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fcxx-exceptions -fexceptions -std=c++23 -print-stats -DLOCALS=2 %s 2>&1 | FileCheck %s --check-prefix=LOCALS32
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fcxx-exceptions -fexceptions -std=c++23 -print-stats -DLOCALS=4 %s 2>&1 | FileCheck %s --check-prefix=LOCALS64
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fcxx-exceptions -fexceptions -std=c++23 -print-stats -DPOINTERS=4 %s 2>&1 | FileCheck %s --check-prefix=POINTERS64
// RUN: %clang_cc1 -fsyntax-only -fprofiles -fcxx-exceptions -fexceptions -std=c++23 -print-stats -DPOINTERS=8 %s 2>&1 | FileCheck %s --check-prefix=POINTERS128
// expected-no-diagnostics

// The std::init flow engine's cost grows linearly with the body: each block's
// state shares every node but the path to the entities its events change, a
// block joins its predecessors pairwise, and a reseat checks only the pointers
// that can have copied the replaced referent.

[[profiles::enforce(std::init)]];

int use(int);
void use(int *p [[ref_to_uninit]]);

struct In {
  int m0 [[uninit]], m1 [[uninit]], m2 [[uninit]], m3 [[uninit]];
  int m4 [[uninit]], m5 [[uninit]], m6 [[uninit]], m7 [[uninit]];
};
struct Out {
  In i0, i1, i2, i3, i4, i5, i6, i7;
  int z = 0;
};

#define REPEAT4(M, n) M(n##0) M(n##1) M(n##2) M(n##3)
#define REPEAT16(M, n) REPEAT4(M, n##0) REPEAT4(M, n##1) REPEAT4(M, n##2) REPEAT4(M, n##3)

// Locals of 64 tracked members each, reset by one event and written once per
// block; the exit block joins every block
// LOCALS32: 160 std-init-flow - Number of std::init flow events extracted
// LOCALS32: 155 std-init-flow - Number of std::init flow-state leaves joined cell by cell
// LOCALS32: 194 std-init-flow - Number of std::init flow-state nodes allocated
// LOCALS64: 320 std-init-flow - Number of std::init flow events extracted
// LOCALS64: 378 std-init-flow - Number of std::init flow-state leaves joined cell by cell
// LOCALS64: 386 std-init-flow - Number of std::init flow-state nodes allocated
#ifdef LOCALS
#define LOCAL(n) Out o##n; o##n.i0.m0 = n; r += use(o##n.i0.m0);
int locals() {
  int r = 0;
  REPEAT16(LOCAL, 1)
  REPEAT16(LOCAL, 2)
#if LOCALS > 2
  REPEAT16(LOCAL, 3)
  REPEAT16(LOCAL, 4)
#endif
  return r;
}
#endif

// Marked pointers, each copied once and reseated to a fresh allocation
// POINTERS64: 328 std-init-flow - Number of std::init flow-state nodes allocated
// POINTERS64: 256 std-init-flow - Number of std::init pointers checked for a replaced referent
// POINTERS128: 652 std-init-flow - Number of std::init flow-state nodes allocated
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
