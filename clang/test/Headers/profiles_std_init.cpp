// RUN: rm -rf %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -fno-profiles-exempt-system-headers -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -std=c++23 -DLIBCXX %s
// RUN: %clang_cc1 -fsyntax-only -verify=expected,modules -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -std=c++23 -fmodules -fimplicit-module-maps -fmodules-cache-path=%t -Rmodule-import %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++98 %s

// std::now_init comes from the resource header <profiles_std_init.h>,
// recognized by name like a standard library's own

// no-profiles-no-diagnostics

#include <profiles_std_init.h> // modules-remark {{importing module 'profiles_std_init'}}

#if __cplusplus >= 201103L
#ifdef LIBCXX
namespace std {
inline namespace __1 {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class It, class Size, class T>
It uninitialized_fill_n(It first, Size n, const T &x);
} // namespace __1
} // namespace std
#else
namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class It, class Size, class T>
It uninitialized_fill_n(It first, Size n, const T &x);
} // namespace std
#endif

// The returned pointer reads the storage the argument refers to
int after_fill(int *p [[ref_to_uninit]], int n) {
  std::uninitialized_fill_n(p, n, 0);
  int *q = std::now_init(p);
  return q[n - 1];
}

// The argument is judged by its marking alone
int after_store() {
  int v [[uninit]];
  v = 1;
  return *std::now_init(&v);
}
int after_construct(int *p [[ref_to_uninit]]) {
  std::construct_at(p);
  return *std::now_init(p);
}

// An unmarked argument is initialized memory
void unmarked(int *p) {
  int x = 1;
  std::now_init(&x); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  std::now_init(p);  // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// The function is constexpr
constexpr int constant() {
  int v [[uninit]];
  v = 2;
  return *std::now_init(&v);
}
static_assert(constant() == 2);
#endif
