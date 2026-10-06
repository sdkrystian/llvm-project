// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// The LEADING_ERROR run adds a leading unrelated error so every later function
// is analyzed through the post-error path.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// -Wunreachable-code makes the main path build a fully linearized CFG.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions %t/libcxx.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions %t/libcxx.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions %t/libcxx.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/libcxx.cpp

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

template <class T> class weak_ptr;
template <class T> class __shared_ptr {
  T *_M_ptr;
  void *_M_refcount;

public:
  T *get() const noexcept;
  void reset() noexcept;
  template <class Y> void reset(Y *p);
};
template <class T> class shared_ptr : public __shared_ptr<T> {
public:
  using element_type = T;
  constexpr shared_ptr() noexcept;
  shared_ptr(const shared_ptr &) noexcept = default;
  template <class Y> explicit shared_ptr(Y *p);
  template <class Y, class D> shared_ptr(Y *p, D d);
  template <class D> shared_ptr(nullptr_t p, D d);
  template <class Y>
  shared_ptr(const shared_ptr<Y> &r, element_type *p) noexcept;
  template <class Y> shared_ptr(const shared_ptr<Y> &r) noexcept;
  shared_ptr(shared_ptr &&r) noexcept;
  template <class Y> explicit shared_ptr(const weak_ptr<Y> &r);
  template <class Y, class D> shared_ptr(unique_ptr<Y, D> &&r);
  constexpr shared_ptr(nullptr_t) noexcept;
  ~shared_ptr();
  shared_ptr &operator=(const shared_ptr &) noexcept = default;
  T &operator*() const noexcept;
  T *operator->() const noexcept;
};
template <class T> class weak_ptr {
  T *_M_ptr;

public:
  weak_ptr(const shared_ptr<T> &r) noexcept;
};

template <class It> class reverse_iterator {
protected:
  It current;

public:
  using iterator_type = It;
  using reference = typename iterator_traits<It>::reference;
  constexpr reverse_iterator() noexcept;
  explicit constexpr reverse_iterator(iterator_type x) noexcept;
  constexpr reverse_iterator(const reverse_iterator &x) noexcept;
  template <class U> constexpr reverse_iterator(const reverse_iterator<U> &x);
  constexpr reference operator*() const;
  constexpr reverse_iterator &operator++();
};

namespace ranges {
template <class It, class Sent = It> class subrange {
  It _M_begin;
  Sent _M_end;

public:
  subrange() = default;
  template <class I> constexpr subrange(I i, Sent s);
  constexpr It begin() const;
  constexpr Sent end() const;
};
template <class It, class Sent> subrange(It, Sent) -> subrange<It, Sent>;
} // namespace ranges

template <class C> class basic_string {
  C *_M_p;
  size_t _M_n;

public:
  basic_string(const C *s);
  basic_string(const basic_string &);
  ~basic_string();
};
using string = basic_string<char>;
} // namespace std
extern "C" void *memset(void *, int, std::size_t);

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
  std::span<int> &r = s; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
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

void sink(std::span<int> s);
void sinkr(const std::span<int> &s);
void fill(std::span<int> s [[ref_to_uninit]]);

// A handle constructed from uninitialized storage must be marked
void span_from_array() {
  int arr [[uninit]] [4];
  std::span<int> s(arr); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::span<int> m [[ref_to_uninit]](arr);
  (void)s, (void)m;
}

// Storage initialized on every path makes an unmarked handle
int span_after_memset() {
  int arr [[uninit]] [4];
  memset(arr, 0, sizeof arr);
  std::span<int> s(arr);
  return s[0];
}

// A handle argument is judged where it binds the parameter
void span_arguments() {
  int arr [[uninit]] [4];
  sink(arr); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  sink(std::span<int>(arr, 4)); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  fill(std::span(arr));
}

// A smart pointer owning a default-initialized object (P4222R2 §4.7)
void unique_from_new() {
  std::unique_ptr<int> p(new int); // expected-error {{handle of type 'std::unique_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::unique_ptr<int> q(new int(5));
  std::unique_ptr<int> m [[ref_to_uninit]](new int);
  std::unique_ptr<int> i [[ref_to_uninit]](new int(5)); // expected-error {{handle of type 'std::unique_ptr<int>' marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// An owner takes the pointer, never the deleter, and the aliasing pointer;
// a null or default construction refers to nothing
struct Del {
  void operator()(int *) const;
};
void shared_constructions(std::shared_ptr<int> other) {
  std::shared_ptr<int> a(new int, Del{}); // expected-error {{handle of type 'std::shared_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::shared_ptr<int> b(new int(1), Del{});
  int u [[uninit]];
  std::shared_ptr<int> c(other, &u); // expected-error {{handle of type 'std::shared_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::shared_ptr<int> d(nullptr);
  std::shared_ptr<int> e [[ref_to_uninit]](nullptr);
  std::shared_ptr<int> f [[ref_to_uninit]];
  std::shared_ptr<int> g(nullptr, Del{});
}

// Every binding of a handle is judged
struct SpanMember {
  std::span<int> f;
  SpanMember(std::span<int> s [[ref_to_uninit]])
      : f(s) {} // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};
struct SpanAggregate {
  std::span<int> f;
};
std::span<int> site_kinds(std::span<int> s [[ref_to_uninit]], bool c) {
  std::span<int> t = s; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::span<int> m = static_cast<std::span<int> &&>(s); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::span<const int> cs = s; // expected-error {{handle of type 'std::span<const int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  SpanAggregate a{s}; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::span<int> e[1] = {s}; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  auto *n = new std::span<int>(s); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  auto l = [s] { return s.size(); }; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  if (c)
    throw s; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)t, (void)m, (void)cs, (void)a, (void)e, (void)n, (void)l;
  return s; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
std::span<int> marked_return [[ref_to_uninit]] (std::span<int> s [[ref_to_uninit]]) {
  return s;
}

// A conditional of a marked and an unmarked handle is Mixed (P4222R2 §4.9)
void mixed_handles(bool c, std::span<int> sm [[ref_to_uninit]],
                   std::span<int> su) {
  std::span<int> x = c ? sm : su; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::span<int> y [[ref_to_uninit]] = c ? sm : su; // expected-error {{handle of type 'std::span<int>' marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)x, (void)y;
}

// A class outside the table takes no raw storage: its constructor's pointer
// parameter is an ordinary binding, and the object is initialized
void string_from_buffer(char *buf [[ref_to_uninit]]) {
  std::string t [[ref_to_uninit]](buf); // expected-error {{handle of type 'std::string' (aka 'basic_string<char>') marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}} \
                                        // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A discarded temporary handle is not judged; a handle bound to an unmarked
// reference to a handle is judged as the handle
void reference_bound_handles(std::span<int> s [[ref_to_uninit]]) {
  int arr [[uninit]] [4];
  std::span<int>(arr, 4);
  sinkr(s); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A handle a temporary builds over raw storage lands in a reference to a
// handle, a local reference, or a by-reference capture as an unmarked handle
void rref_sink(std::span<int> &&s);
void reference_landings(int *raw [[ref_to_uninit]],
                        std::span<int> m [[ref_to_uninit]], std::span<int> u) {
  int arr [[uninit]] [4];
  sinkr(arr); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  sinkr(std::span<int>(raw, 4)); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  rref_sink({raw, 4}); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  const std::span<int> &r = m; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  auto l = [&m] { return m.size(); }; // expected-error {{capturing 'm' by reference binds a reference to uninitialized memory under profile 'std::init'}}
  const std::span<int> &ok = u;
  sinkr(u);
  (void)r, (void)l, (void)ok;
}

// A base subobject cannot carry the marker
struct SpanBase : std::span<int> {
  SpanBase(int *p [[ref_to_uninit]])
      : std::span<int>(p, 4) {} // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};
struct OwnerBase : std::unique_ptr<int> {
  OwnerBase() : std::unique_ptr<int>(new int) {} // expected-error {{handle of type 'std::unique_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};
struct SpanAggBase : std::span<int> {
  int k;
};
void aggregate_base(int *raw [[ref_to_uninit]]) {
  SpanAggBase a{std::span<int>(raw, 4), 1}; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)a;
}

// The last iterator of a (first, last) construction bounds the range
void first_last(int *p, int *e [[ref_to_uninit]]) {
  std::span<int> s(p, e);
  std::ranges::subrange sr(p, e);
  (void)s, (void)sr;
}

// An author-marked constructor parameter makes its object a handle to what
// the parameter takes
struct Buf {
  Buf(int *p [[ref_to_uninit]]);
};
void author_marked() {
  int u [[uninit]];
  Buf b(&u); // expected-error {{handle of type 'Buf' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int v [[uninit]];
  Buf m [[ref_to_uninit]](&v);
  (void)b, (void)m;
}

// A member of a std-table handle returning another of its kind (a
// subrange's iterator) inherits its state
int subrange_iterator(std::span<int> s [[ref_to_uninit]]) {
  std::ranges::subrange<std::span<int>::iterator> sr [[ref_to_uninit]](
      s.begin(), s.end());
  return *sr.begin(); // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// The iterator adaptors and ranges::subrange are in the table
void adaptors(int *p [[ref_to_uninit]]) {
  std::reverse_iterator<int *> r(p); // expected-error {{handle of type 'std::reverse_iterator<int *>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::reverse_iterator<int *> m [[ref_to_uninit]](p);
  std::ranges::subrange sr(p, p + 4); // expected-error {{handle of type 'std::ranges::subrange<int *, int *>' (aka 'subrange<int *, int *>') to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::ranges::subrange sm [[ref_to_uninit]](p, p + 4);
  (void)r, (void)m, (void)sr, (void)sm;
}

// Assigning a handle is judged against the assigned-to handle's marking
void span_assignment(std::span<int> sm [[ref_to_uninit]],
                     std::span<int> sm2 [[ref_to_uninit]], std::span<int> su) {
  su = sm; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  sm = su; // expected-error {{handle of type 'std::span<int>' marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  sm = static_cast<std::span<int> &&>(sm2);
}
void owner_assignment(std::unique_ptr<int> up [[ref_to_uninit]],
                      std::unique_ptr<int> up2 [[ref_to_uninit]]) {
  up = nullptr;
  up = static_cast<std::unique_ptr<int> &&>(up2);
}

// An owner's reset judges its pointer against the owner's marking
void owner_reset(std::unique_ptr<int> m [[ref_to_uninit]],
                 std::unique_ptr<int> u, std::shared_ptr<int> s) {
  m.reset(new int);
  m.reset(new int(1)); // expected-error {{handle of type 'std::unique_ptr<int>' marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  u.reset(new int); // expected-error {{handle of type 'std::unique_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  u.reset(new int(1));
  u.reset();
  s.reset(new int); // expected-error {{handle of type 'std::shared_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Storage tracked by flow counts at the assignment
void assign_after_fill() {
  int arr [[uninit]] [4];
  std::span<int> s;
  s = std::span<int>(arr); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  memset(arr, 0, sizeof arr);
  s = std::span<int>(arr);
}

// A handle assigned or reset through a reference, a pointer, or an element
// cannot carry the marker
void alias_assignment(std::span<int> sm [[ref_to_uninit]], std::span<int> &r,
                      std::unique_ptr<int> *pu, std::span<int> *arr) {
  r = sm; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  pu->reset(new int); // expected-error {{handle of type 'std::unique_ptr<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  pu->reset(new int(1));
  arr[0] = sm; // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// An assignment called by name reseats like the operator; a handle an
// assignment takes by value binds where it is assigned
struct SwapBuf {
  SwapBuf(int *p [[ref_to_uninit]]);
  SwapBuf(const SwapBuf &);
  SwapBuf &operator=(SwapBuf o);
};
void named_and_by_value(std::span<int> sm [[ref_to_uninit]], std::span<int> su,
                        int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) {
  su.operator=(sm); // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  SwapBuf a [[ref_to_uninit]](p), b [[ref_to_uninit]](q);
  a = b;
}

//--- libcxx.cpp
// libc++'s pointer wrapper, std::__1::__wrap_iter, is in the table.
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif
namespace std {
inline namespace __1 {
template <class Iter> class __wrap_iter {
  Iter __i_;

public:
  __wrap_iter() noexcept : __i_() {}
  explicit __wrap_iter(Iter x) noexcept : __i_(x) {}
  template <class Other> __wrap_iter(const __wrap_iter<Other> &u) noexcept;
  decltype(*__i_) operator*() const noexcept { return *__i_; }
};
} // namespace __1
} // namespace std

int wrap(int *p [[ref_to_uninit]]) {
  std::__wrap_iter<int *> w(p); // expected-error {{handle of type 'std::__wrap_iter<int *>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::__wrap_iter<int *> m [[ref_to_uninit]](p);
  std::__wrap_iter<const int *> c = m; // expected-error {{handle of type 'std::__wrap_iter<const int *>' to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)w, (void)c;
  return *m; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}
