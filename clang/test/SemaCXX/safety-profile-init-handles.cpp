// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// The LEADING_ERROR run adds a leading unrelated error so every later function
// is analyzed through the post-error path.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// -Wunreachable-code makes the main path build a fully linearized CFG.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp

// std::init: a handle -- a smart pointer, a span, an iterator -- marked
// [[ref_to_uninit]] refers to uninitialized memory (P4222R2 §4.6, §9.3).

//--- sys/std_handles.h
// The shapes of libstdc++'s <span> and <memory>.
namespace std {
using size_t = decltype(sizeof 0);
using ptrdiff_t = decltype((int *)0 - (int *)0);
using nullptr_t = decltype(nullptr);
inline constexpr size_t dynamic_extent = static_cast<size_t>(-1);
template <class T> struct type_identity { using type = T; };
template <class T> using type_identity_t = typename type_identity<T>::type;
template <class T> struct remove_cv { using type = T; };
template <class T> struct remove_cv<const T> { using type = T; };
template <class T> using remove_cv_t = typename remove_cv<T>::type;
template <class T> struct iterator_traits;
template <class T> struct iterator_traits<T *> {
  using value_type = remove_cv_t<T>;
  using difference_type = ptrdiff_t;
  using pointer = T *;
  using reference = T &;
};
template <class T, class... A> constexpr T *construct_at(T *p, A &&...args);
template <class T> constexpr void destroy_at(T *p);
} // namespace std

namespace __gnu_cxx {
template <class It, class Container> class __normal_iterator {
  It _M_current;
  using traits = std::iterator_traits<It>;

public:
  using value_type = typename traits::value_type;
  using difference_type = typename traits::difference_type;
  using pointer = typename traits::pointer;
  using reference = typename traits::reference;
  constexpr __normal_iterator() noexcept : _M_current() {}
  explicit constexpr __normal_iterator(const It &i) noexcept : _M_current(i) {}
  constexpr reference operator*() const noexcept { return *_M_current; }
  constexpr pointer operator->() const noexcept { return _M_current; }
  constexpr __normal_iterator &operator++() noexcept;
  constexpr __normal_iterator operator++(int) noexcept;
  constexpr reference operator[](difference_type n) const noexcept;
  constexpr __normal_iterator &operator+=(difference_type n) noexcept;
  constexpr __normal_iterator operator+(difference_type n) const noexcept;
  constexpr const It &base() const noexcept { return _M_current; }
};
template <class It, class C>
constexpr bool operator==(const __normal_iterator<It, C> &,
                          const __normal_iterator<It, C> &) noexcept;
template <class It, class C>
constexpr __normal_iterator<It, C>
operator+(typename __normal_iterator<It, C>::difference_type n,
          const __normal_iterator<It, C> &i) noexcept;
} // namespace __gnu_cxx

namespace std {
template <class T, size_t Extent = dynamic_extent> class span {
  struct __iter_tag;
  T *_M_ptr;
  size_t _M_extent;

public:
  using element_type = T;
  using value_type = remove_cv_t<T>;
  using size_type = size_t;
  using pointer = T *;
  using reference = T &;
  using iterator = __gnu_cxx::__normal_iterator<pointer, __iter_tag>;
  constexpr span() noexcept;
  template <class It> constexpr span(It first, size_type count) noexcept;
  template <class It, class End> constexpr span(It first, End last) noexcept;
  template <size_t ArrayExtent>
  constexpr span(type_identity_t<element_type> (&arr)[ArrayExtent]) noexcept;
  constexpr span(const span &) noexcept = default;
  template <class OType, size_t OExtent>
  constexpr span(const span<OType, OExtent> &s) noexcept;
  constexpr span &operator=(const span &) noexcept = default;
  constexpr size_type size() const noexcept;
  constexpr reference front() const noexcept;
  constexpr reference back() const noexcept;
  constexpr reference operator[](size_type idx) const noexcept;
  constexpr pointer data() const noexcept;
  constexpr iterator begin() const noexcept;
  constexpr iterator end() const noexcept;
  constexpr span<element_type> subspan(size_type offset,
                                       size_type count = dynamic_extent) const
      noexcept;
};
template <class T, size_t N> span(T (&)[N]) -> span<T, N>;
template <class T, class End> span(T *, End) -> span<T>;

template <class T> struct default_delete {
  constexpr default_delete() noexcept = default;
  void operator()(T *p) const;
};
template <class T, class D = default_delete<T>> class unique_ptr {
  T *_M_p;
  D _M_d;

public:
  using pointer = T *;
  using element_type = T;
  using deleter_type = D;
  constexpr unique_ptr() noexcept;
  explicit unique_ptr(pointer p) noexcept;
  unique_ptr(pointer p, const deleter_type &d) noexcept;
  constexpr unique_ptr(nullptr_t) noexcept;
  unique_ptr(unique_ptr &&) = default;
  ~unique_ptr() noexcept;
  unique_ptr &operator=(unique_ptr &&) = default;
  unique_ptr &operator=(nullptr_t) noexcept;
  T &operator*() const noexcept;
  pointer operator->() const noexcept;
  pointer get() const noexcept;
  deleter_type &get_deleter() noexcept;
  explicit operator bool() const noexcept;
  pointer release() noexcept;
  void reset(pointer p = pointer()) noexcept;
  unique_ptr(const unique_ptr &) = delete;
  unique_ptr &operator=(const unique_ptr &) = delete;
};
} // namespace std

//--- test.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <std_handles.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

struct X {
  int v;
  X(int x) : v(x) {}
};
struct S {
  int v;
  void m();
};

// The paper's span example (P4222R2 §4.6): an element of a marked span is
// uninitialized memory
void init(std::span<X> s [[ref_to_uninit]], int i1, int i2) {
  int *q = &s.front().v; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&s[2], 10);
  X y = s[i2]; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&s[i1], 10);
  (void)q, (void)y;
}

// A read through a marked smart pointer
int read_through(std::unique_ptr<int> p [[ref_to_uninit]]) {
  return *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// get() of a marked smart pointer refers to uninitialized memory
void get_unmarked(std::unique_ptr<int> p [[ref_to_uninit]]) {
  int *q = p.get(); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q;
}
void get_marked(std::unique_ptr<int> p [[ref_to_uninit]]) {
  int *q [[ref_to_uninit]] = p.get();
  (void)q;
}

// A store through a marked smart pointer is its referent's store
void store_through(std::unique_ptr<int> p [[ref_to_uninit]]) { *p = 5; }

// A member store or member call through a marked smart pointer
void member_store(std::unique_ptr<S> p [[ref_to_uninit]]) {
  p->v = 1; // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
}
void member_call(std::unique_ptr<S> p [[ref_to_uninit]]) {
  p->m(); // expected-error {{calling member function 'm' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}

// Iterators and element accessors of a marked span; its size is the span's own
void span_accessors(std::span<int> s [[ref_to_uninit]]) {
  int &a = *s.begin(); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int &b = s.front(); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int &c [[ref_to_uninit]] = s.back();
  std::size_t n = s.size();
  (void)a, (void)b, (void)c, (void)n;
}

// An iterator or subspan derived from a marked span inherits its state
int offset_iterator(std::span<int> s [[ref_to_uninit]]) {
  return *(s.begin() + 1); // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}
int subspan_element(std::span<int> s [[ref_to_uninit]]) {
  return s.subspan(1)[0]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}
int incremented_iterator(std::span<int> s [[ref_to_uninit]]) {
  auto it [[ref_to_uninit]] = s.begin();
  return *++it; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// An unmarked span refers to initialized memory
int unmarked_span(std::span<int> s) { return s[0]; }

// A conditional mixing a marked and an unmarked span (P4222R2 §4.9)
int mixed_spans(bool c, std::span<int> sm [[ref_to_uninit]],
                std::span<int> su) {
  return (c ? sm : su)[0]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// Every reference a member of a marked handle returns is presumed
// uninitialized, the deleter's included
void deleter(std::unique_ptr<int> up [[ref_to_uninit]]) {
  auto &d = up.get_deleter(); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)d;
}

// A range-for over a marked span binds its elements
int range_for(std::span<int> s [[ref_to_uninit]]) {
  int k = 0;
  for (int &x [[ref_to_uninit]] : s)
    (void)x;
  for (int x : s) // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
    k += x;
  return k;
}

// A copy of a marked span and a local reference to one refer to the same
// memory
int copied_span(std::span<int> s [[ref_to_uninit]]) {
  std::span<int> &r = s;
  int a = r[0]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  return a + std::span<int>(s)[1]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// An iterator a non-member operator+ offsets inherits its state
int offset_iterator_left(std::span<int> s [[ref_to_uninit]]) {
  return *(1 + s.begin()); // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A class a member returns by value inherits the handle's state only as a
// handle to the same memory
struct Name {
  const char *data() const;
};
struct Arena {
  using value_type = char;
  Name name() const;
  std::span<char> bytes() const;
};
void arena_results(Arena a [[ref_to_uninit]]) {
  const char *n = a.name().data();
  char *b = a.bytes().data(); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)n, (void)b;
}

// A call operator's result is an ordinary call result
struct Gen {
  int *operator()() const;
};
int *call_operator(Gen g [[ref_to_uninit]]) { return g(); }

// A marked handle member, and a function returning a marked handle
struct Holder {
  std::span<int> s [[ref_to_uninit]];
  int first() const {
    return s[0]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  }
};
std::span<int> uninit_span [[ref_to_uninit]] ();
int from_call() {
  return uninit_span()[0]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}
