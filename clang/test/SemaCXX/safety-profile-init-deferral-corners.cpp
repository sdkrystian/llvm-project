// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -isystem %t/sys %t/corners.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected,omp -fprofiles -fprofiles-verify-deferred-checks -fopenmp -std=c++23 -isystem %t/sys %t/corners.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected,blocks -fprofiles -fprofiles-verify-deferred-checks -fblocks -std=c++23 -isystem %t/sys %t/corners.cpp

// The shapes the verification of std::init's deferred checks depends on
// (ProfilesFrameworkInternals.rst, "Flow-Tracked Storage (std::init)"): under
// -fprofiles-verify-deferred-checks a deferred check the flow pass leaves
// unjudged is an error, so each case pins its verdict and the record's keying
// or visibility.

//--- sys/sys_body.h
#pragma clang system_header
void sys_sink(int *);
inline void sys_body() {
  int u [[uninit]];
  sys_sink(&u);
}

//--- corners.cpp
[[profiles::enforce(std::init)]];
#include <sys_body.h>

void sink(int *);
bool check(int *);
int use(int);
int *gp;

// An OpenMP region's bindings are judged by the enclosing function's flow
void omp_region() {
  int u [[uninit]];
#pragma omp parallel
  {
    int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    (void)q;
  }
  int v [[uninit]];
  v = 1;
#pragma omp parallel
  {
    int *q = &v; // OK
    (void)q;
  }
}

// An OpenMP region's own tracked locals are tracked like any local
void omp_region_local() {
#pragma omp parallel
  {
    int w [[uninit]];
    sink(&w); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    int v [[uninit]];
    v = 1;
    sink(&v); // OK
  }
}

// A constructor's OpenMP region is part of its body
struct OmpCtor {
  int m [[uninit]]; // expected-note {{member 'm' declared here}}
  OmpCtor() {
    int u [[uninit]];
#pragma omp parallel
    {
      sink(&u); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
      use(m); // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
    }
    m = 1;
  }
};

// A constructor's OpenMP clause is part of its body
struct OmpCtorClause {
  int m [[uninit]]; // omp-note {{member 'm' declared here}}
  OmpCtorClause() {
#pragma omp parallel if(m) // omp-error {{member 'm' is read before initialization under profile 'std::init'}}
    {
    }
    m = 1;
  }
};

// A local of a statement expression in a mem-initializer is tracked
struct StmtExprInit {
  int k;
  StmtExprInit() : k(({ int t [[uninit]]; t = 1; sink(&t); 1; })) {} // OK
};

// A debug captured region's statements are not flow sites
void debug_captured() {
  int u [[uninit]];
#pragma clang __debug captured
  {
    sink(&u);
  }
#pragma clang __debug captured
  {
    u = 1;
  }
  u = 1;
}

// An init-capture is judged by the enclosing function's flow
void init_capture() {
  int u [[uninit]];
  auto l = [q = &u] { (void)q; }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  u = 1;
  auto m = [q = &u] { (void)q; }; // OK
  (void)l; (void)m;
}

// A by-reference capture is judged by the enclosing function's flow
void byref_capture() {
  int u [[uninit]];
  auto l = [&u] { (void)&u; }; // expected-error {{capturing 'u' by reference binds a reference to uninitialized memory under profile 'std::init'}}
  (void)l;
}

// A lambda body is its own analysis owner
void lambda_body() {
  auto l = [] {
    int w [[uninit]];
    sink(&w); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  (void)l;
}

#ifdef __BLOCKS__
// A block body is its own analysis owner
void block_body() {
  void (^b)() = ^{
    int w [[uninit]];
    sink(&w); // blocks-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  (void)b;
}
#endif

// Unevaluated operands and assumptions are not flow sites
void unevaluated() {
  int u [[uninit]];
  (void)sizeof(sink(&u), 0);
  (void)noexcept(sink(&u));
  decltype(sink(&u)) *vp = nullptr;
  static_assert(!noexcept(sink(&u)));
  [[assume(check(&u))]]; // expected-warning {{assumption is ignored because it contains (potential) side-effects}}
  __builtin_assume(check(&u)); // expected-warning {{assumption is ignored because it contains (potential) side-effects}}
  (void)__builtin_constant_p(check(&u));
  (void)vp;
  u = 1;
}

// An if consteval branch is judged by flow
void consteval_branch() {
  int u [[uninit]];
  if consteval {
    sink(&u); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  }
  u = 1;
}

// A discarded if constexpr branch is not judged
void discarded_branch() {
  int u [[uninit]];
  if constexpr (false) {
    sink(&u); // OK
  }
  u = 1;
}

// A generic lambda's body is analyzed per specialization
void generic_lambda() {
  auto g = [](auto x) {
    int w [[uninit]];
    sink(&w); // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    return x;
  };
  g(1);   // expected-note {{in instantiation of function template specialization}}
  g(2.0); // expected-note {{in instantiation of function template specialization}}
}

// A builtin the flow engine does not enumerate leaves its arguments unjudged
void unenumerated_builtins() {
  int u [[uninit]];
  (void)__builtin_launder(&u);
  (void)__builtin_align_up(&u, 16);
  int *pu [[ref_to_uninit]] = &u;
  __atomic_store_n(&gp, pu, 0);
  u = 1;
}

// A builtin the flow engine does not enumerate leaves its arguments unjudged in an OpenMP region
void unenumerated_builtins_omp() {
  int u [[uninit]];
#pragma omp parallel
  {
    (void)__builtin_launder(&u);
  }
  u = 1;
}

// An unselected generic association or choice arm is not a flow site
void unselected_arms() {
  int u [[uninit]];
  (void)_Generic(0, int: 1, default: (sink(&u), 2));
  __builtin_choose_expr(1, (void)0, sink(&u));
  u = 1;
}

// An OpenMP clause expression is not a flow site
void omp_clause() {
  int u [[uninit]];
  u = 8;
#pragma omp parallel num_threads(u)
  {
  }
}

// A coroutine's parameter copies and promise construction are not flow sites
namespace std {
template <class R, class... A> struct coroutine_traits {
  using promise_type = typename R::promise_type;
};
template <class P = void> struct coroutine_handle;
template <> struct coroutine_handle<void> {
  static coroutine_handle from_address(void *);
};
template <class P> struct coroutine_handle {
  static coroutine_handle from_address(void *);
  operator coroutine_handle<>() const;
};
struct suspend_never {
  bool await_ready() noexcept;
  void await_suspend(coroutine_handle<>) noexcept;
  void await_resume() noexcept;
};
} // namespace std
struct CoTask {
  struct promise_type {
    promise_type(int *, int *);
    CoTask get_return_object();
    std::suspend_never initial_suspend();
    std::suspend_never final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
  };
};
CoTask coroutine_marked_params(int *q [[ref_to_uninit]], int *r) {
  sink(q); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  co_return;
}

// A system-header body is not analyzed
void call_sys_body() { sys_body(); }
