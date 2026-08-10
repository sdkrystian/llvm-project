// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c++20 -DCORO %s
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c++20 -fopenmp -DOMP %s

// std::init / uninit_decl skips synthesized (implicit) variables: the user
// can neither initialize them nor mark them [[uninit]]. A coroutine's
// __promise and OpenMP's private/reduction/linear copies are the reachable
// shapes; user-written declarations in the same bodies stay checked.

[[profiles::enforce(std::init)]];

#ifdef CORO
#include "Inputs/std-coroutine.h"

struct Task {
  struct promise_type {
    // An indeterminate scalar member and no user-provided default
    // constructor: default-initializing the implicit __promise leaves
    // 'state' uninitialized, but the variable is synthesized -- no
    // uninit_decl at the coroutine.
    int state;
    Task get_return_object();
    std::suspend_never initial_suspend();
    std::suspend_never final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
  };
};

Task test_promise_not_flagged() { // OK: __promise is implicit
  co_return;
}

Task test_body_still_checked() {
  int x; // expected-error {{variable 'x' must be initialized or marked '[[uninit]]' under profile 'std::init'}}
  co_return;
}
#endif

#ifdef OMP
void test_private_copy_not_flagged() {
  int x = 0;
#pragma omp parallel private(x) // OK: the synthesized private copy is implicit
  { x = 1; }
}

void test_reduction_and_linear_copies_not_flagged() {
  int r = 0;
  int x = 0;
#pragma omp parallel for reduction(+ : r) linear(x) // OK: synthesized copies are implicit
  for (int i = 0; i < 10; ++i)
    r += i;
}

void test_omp_body_still_checked() {
#pragma omp parallel
  {
    int y; // expected-error {{variable 'y' must be initialized or marked '[[uninit]]' under profile 'std::init'}}
    (void)y;
  }
}
#endif
