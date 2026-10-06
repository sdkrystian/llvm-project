// The std handle table is looked up at the use, so a module that provides
// the handle classes reads the same whether or not it was built with
// -fprofiles.

// RUN: rm -rf %t
// RUN: mkdir -p %t
// RUN: split-file %s %t

// RUN: %clang_cc1 -std=c++23 -emit-module-interface %t/lib.cppm -o %t/lib.pcm
// RUN: %clang_cc1 -std=c++23 -fprofiles -emit-module-interface %t/lib.cppm -o %t/lib-profiles.pcm
// RUN: %clang_cc1 -std=c++23 -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -fsyntax-only %t/use.cpp -fmodule-file=lib=%t/lib.pcm -verify
// RUN: %clang_cc1 -std=c++23 -fprofiles-enforce=std::init -fprofiles-verify-deferred-checks -fsyntax-only %t/use.cpp -fmodule-file=lib=%t/lib-profiles.pcm -verify

//--- std.h
namespace std {
using size_t = decltype(sizeof 0);
template <class T> class span {
  T *_M_ptr;
  size_t _M_extent;

public:
  template <class It> constexpr span(It first, size_t count) noexcept;
  template <size_t N> constexpr span(T (&arr)[N]) noexcept;
  constexpr span(const span &) noexcept = default;
  constexpr T &operator[](size_t idx) const noexcept;
  constexpr size_t size() const noexcept;
};
template <class T> class unique_ptr {
  T *_M_p;

public:
  explicit unique_ptr(T *p) noexcept;
  unique_ptr(unique_ptr &&) = default;
  ~unique_ptr();
  T &operator*() const noexcept;
  T *get() const noexcept;
};
} // namespace std

//--- lib.cppm
module;
#include "std.h"
export module lib;
export namespace std {
using std::span;
using std::unique_ptr;
} // namespace std

//--- use.cpp
import lib;

// A span over an uninitialized array must be marked
void span_from_array() {
  int arr [[uninit]] [4];
  std::span<int> s(arr); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::span<int> m [[ref_to_uninit]](arr);
  (void)s, (void)m;
}

// An element of a marked span is uninitialized memory
int element(std::span<int> s [[ref_to_uninit]]) {
  return s[0]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A smart pointer owning a default-initialized object must be marked
void owner() {
  std::unique_ptr<int> p(new int); // expected-error {{handle of type 'std::unique_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::unique_ptr<int> m [[ref_to_uninit]](new int);
  int *q = m.get(); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q;
}
