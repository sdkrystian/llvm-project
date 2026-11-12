// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// The LEADING_ERROR run adds a leading unrelated error so every later function
// is analyzed through the post-error path.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// -Wunreachable-code makes the main path build a fully linearized CFG.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp

// std::init: a range-for whose body begins by initializing its element --
// a built-in scalar store to the loop variable, or a call that initializes
// the storage the variable refers to -- initializes its range when the loop
// completes (ProfilesFramework.rst, "Binding Pointers and References").

//--- sys/std_range.h
// The shapes of libstdc++'s <array> and <memory> the tests use.
namespace std {
using size_t = decltype(sizeof 0);
template <class T, class... A> constexpr T *construct_at(T *p, A &&...args);
template <class T> constexpr void destroy_at(T *p);
template <class FwdIt, class T>
void uninitialized_fill(FwdIt first, FwdIt last, const T &x);
template <class T, size_t N> struct array {
  T _M_elems[N];
  using value_type = T;
  constexpr T *begin() noexcept;
  constexpr T *end() noexcept;
  constexpr T &operator[](size_t n) noexcept;
};
} // namespace std

//--- test.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

#include <std_range.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

void use(int);
int next_value();
struct Point {
  int v;
};

// The range-for of P4222R2 §1.4, then a read
int store_then_read() {
  int aar6 [[uninit]] [10];
  for (int &x : aar6)
    x = 0b1010101010101010;
  return aar6[9];
}

// The loop variable may be auto &, a named reference, or auto &&
int auto_reference() {
  int a [[uninit]] [4];
  for (auto &x : a)
    x = 0;
  return a[3];
}
int forwarding_reference() {
  int a [[uninit]] [4];
  for (auto &&x : a)
    x = 0;
  return a[3];
}

// A std::array is initialized like an array
int std_array() {
  std::array<int, 4> a [[uninit]];
  for (int &x : a)
    x = 0;
  return a[3];
}

// A marked loop variable may construct its element
int marked_construct() {
  int a [[uninit]] [4];
  for (int &x [[ref_to_uninit]] : a)
    std::construct_at(&x, 1);
  return a[3];
}
int marked_construct_class() {
  Point a [[uninit]] [4];
  for (Point &x [[ref_to_uninit]] : a)
    std::construct_at(&x, 1);
  return a[3].v;
}

// A constructor initializes an [[uninit]] array member
struct Member {
  int buf [[uninit]] [8];
  Member() {
    for (int &x : buf)
      x = 0;
    use(buf[7]);
  }
};

// A marked std::array reference parameter
int marked_array_parameter(std::array<int, 4> &out [[ref_to_uninit]]) {
  for (auto &x : out)
    x = 0;
  return out[3];
}

// A function template is judged per specialization
template <class T> T template_store() {
  T a [[uninit]] [4];
  for (T &x : a)
    x = T();
  return a[3];
}
template int template_store<int>();

// The loop's construction counts against a later construction
void construct_after_loop() {
  int a [[uninit]] [4];
  for (int &x [[ref_to_uninit]] : a)
    std::construct_at(&x, 1);
  std::uninitialized_fill(a, a + 4, 0); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A break after the store leaves the loop off its exit: not credited
int break_after_store(bool c) {
  int a [[uninit]] [4];
  for (int &x : a) {
    x = 0;
    if (c)
      break;
  }
  return a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A throwing store leaves the loop by its exception edge: not credited
int throwing_store() {
  int a [[uninit]] [4];
  try {
    for (int &x : a)
      x = next_value();
  } catch (...) {
    return a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  }
  return a[1];
}

// A continue before the store is not recognized
int continue_before_store(bool c) {
  int a [[uninit]] [4];
  for (int &x : a) { // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    if (c)
      continue;
    x = 0;
  }
  return a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A store reading the element is not recognized
void store_reads_element() {
  int a [[uninit]] [4];
  for (int &x : a) // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    x = x + 1;
}

// A body naming the range is not recognized
void body_names_range() {
  int a [[uninit]] [4];
  for (int &x : a) { // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    x = 0;
    use(sizeof a);
    std::destroy_at(&a[0]); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  }
}

// A copy of the element, a const reference, and an unmarked construction are not recognized
void element_copy() {
  int a [[uninit]] [4];
  for (int x : a) // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    use(x);
}
void const_reference() {
  int a [[uninit]] [4];
  for (const int &x : a) // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    use(x);
}
void unmarked_construct() {
  int a [[uninit]] [4];
  for (int &x : a) // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    std::construct_at(&x, 1); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
