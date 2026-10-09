// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// The LEADING_ERROR run adds a leading unrelated error so every later function
// is analyzed through the post-error path.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -DLEADING_ERROR -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp
// -Wunreachable-code makes the main path build a fully linearized CFG.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-verify-deferred-checks -std=c++23 -Wunreachable-code -fexceptions -fcxx-exceptions -isystem %t/sys %t/test.cpp

// std::init: each row spells one step into tracked storage -- a member, base,
// element, dereference, address-of, identity call, derivation, handle
// accessor, std::array member, conditional, pointer to data member, range-for
// element, lambda capture, or constructor member -- and runs a read, a store, an
// unmarked pointer binding, std::construct_at, and std::destroy_at through it,
// each in a function of its own, before and after the storage is filled.  A
// walker that designates other storage than its siblings changes a verdict
// here.

//--- sys/std_matrix.h
// The shapes of libstdc++'s <span>, <memory>, <array>, <iterator> and
// <utility> the matrix spells its steps with.
namespace std {
using size_t = decltype(sizeof 0);
using ptrdiff_t = decltype((int *)0 - (int *)0);
inline constexpr size_t dynamic_extent = static_cast<size_t>(-1);
template <class T> struct remove_cv { using type = T; };
template <class T> struct remove_cv<const T> { using type = T; };
template <class T> using remove_cv_t = typename remove_cv<T>::type;
template <class T> struct remove_reference { using type = T; };
template <class T> struct remove_reference<T &> { using type = T; };
template <class T> typename remove_reference<T>::type &&move(T &&t) noexcept;
template <class T> const T &as_const(T &t) noexcept;
template <class T> T *addressof(T &r) noexcept;
template <class T> struct iterator_traits;
template <class T> struct iterator_traits<T *> {
  using value_type = remove_cv_t<T>;
  using difference_type = ptrdiff_t;
  using pointer = T *;
  using reference = T &;
};
template <class T, class... A> constexpr T *construct_at(T *p, A &&...args);
template <class T> constexpr void destroy_at(T *p);
template <class FwdIt, class T>
void uninitialized_fill(FwdIt first, FwdIt last, const T &x);
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
  explicit constexpr __normal_iterator(const It &i) noexcept : _M_current(i) {}
  constexpr reference operator*() const noexcept { return *_M_current; }
  constexpr pointer operator->() const noexcept { return _M_current; }
  constexpr __normal_iterator &operator++() noexcept;
  constexpr reference operator[](difference_type n) const noexcept;
  constexpr __normal_iterator operator+(difference_type n) const noexcept;
};
template <class It, class C>
constexpr bool operator==(const __normal_iterator<It, C> &,
                          const __normal_iterator<It, C> &) noexcept;
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
  constexpr span(const span &) noexcept = default;
  constexpr span &operator=(const span &) noexcept = default;
  constexpr reference front() const noexcept;
  constexpr reference operator[](size_type idx) const noexcept;
  constexpr pointer data() const noexcept;
  constexpr iterator begin() const noexcept;
  constexpr iterator end() const noexcept;
};

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
  constexpr unique_ptr() noexcept;
  explicit unique_ptr(pointer p) noexcept;
  unique_ptr(unique_ptr &&) = default;
  ~unique_ptr() noexcept;
  T &operator*() const noexcept;
  pointer get() const noexcept;
  unique_ptr(const unique_ptr &) = delete;
  unique_ptr &operator=(const unique_ptr &) = delete;
};

template <class T, size_t N> struct array {
  T _M_elems[N];
  using value_type = T;
  using iterator = T *;
  constexpr iterator begin() noexcept;
  constexpr iterator end() noexcept;
  constexpr T *data() noexcept;
  constexpr T &operator[](size_t n) noexcept;
};

template <class C> constexpr auto begin(C &c) -> decltype(c.begin());
template <class T, size_t N> constexpr T *begin(T (&a)[N]) noexcept;
template <class C> constexpr auto data(C &c) -> decltype(c.data());
template <class T, size_t N> constexpr T *data(T (&a)[N]) noexcept;
template <class It> constexpr It next(It it, ptrdiff_t n = 1);
template <class T> constexpr T *to_address(T *p) noexcept;
namespace ranges {
struct __begin_fn {
  template <class R> constexpr auto operator()(R &&r) const -> decltype(r.begin());
  template <class T, size_t N> constexpr T *operator()(T (&a)[N]) const noexcept;
};
inline constexpr __begin_fn begin{};
} // namespace ranges
} // namespace std

//--- test.cpp
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];
#include <std_matrix.h>

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

struct Agg { int x; int y; };
struct Base { int b; };
struct Der : Base { int e; };
struct Mid { int n [[uninit]]; }; // #mid-n
struct Base2 { int b [[uninit]]; }; // #base2-b
struct Der2 : Base2 { int d [[uninit]]; }; // #der2-d
// m1, member: g.x
int m1_read_before() { Agg g [[uninit]]; return g.x; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void m1_store_before() { Agg g [[uninit]]; g.x = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void m1_bind_before() { Agg g [[uninit]]; int *q_ = &(g.x); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void m1_credit_before() { Agg g [[uninit]]; std::construct_at(&(g.x), 1); }
void m1_destroy_before() { Agg g [[uninit]]; std::destroy_at(&(g.x)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int m1_read_after() { Agg g [[uninit]]; std::construct_at(&g); return g.x; }
void m1_store_after() { Agg g [[uninit]]; std::construct_at(&g); g.x = 1; }
void m1_bind_after() { Agg g [[uninit]]; std::construct_at(&g); int *q_ = &(g.x); (void)q_; }
void m1_credit_after() { Agg g [[uninit]]; std::construct_at(&g); std::construct_at(&(g.x), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void m1_destroy_after() { Agg g [[uninit]]; std::construct_at(&g); std::destroy_at(&(g.x)); }
// m2, member: (&g)->x
int m2_read_before() { Agg g [[uninit]]; return (&g)->x; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void m2_store_before() { Agg g [[uninit]]; (&g)->x = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void m2_bind_before() { Agg g [[uninit]]; int *q_ = &((&g)->x); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void m2_credit_before() { Agg g [[uninit]]; std::construct_at(&((&g)->x), 1); }
void m2_destroy_before() { Agg g [[uninit]]; std::destroy_at(&((&g)->x)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int m2_read_after() { Agg g [[uninit]]; std::construct_at(&g); return (&g)->x; }
void m2_store_after() { Agg g [[uninit]]; std::construct_at(&g); (&g)->x = 1; }
void m2_bind_after() { Agg g [[uninit]]; std::construct_at(&g); int *q_ = &((&g)->x); (void)q_; }
void m2_credit_after() { Agg g [[uninit]]; std::construct_at(&g); std::construct_at(&((&g)->x), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void m2_destroy_after() { Agg g [[uninit]]; std::construct_at(&g); std::destroy_at(&((&g)->x)); }
// m3, member: pg->x
int m3_read_before(Agg *pg [[ref_to_uninit]]) { return pg->x; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void m3_store_before(Agg *pg [[ref_to_uninit]]) { pg->x = 1; } // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it}}
void m3_bind_before(Agg *pg [[ref_to_uninit]]) { int *q_ = &(pg->x); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void m3_credit_before(Agg *pg [[ref_to_uninit]]) { std::construct_at(&(pg->x), 1); }
void m3_destroy_before(Agg *pg [[ref_to_uninit]]) { std::destroy_at(&(pg->x)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int m3_read_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); return pg->x; }
void m3_store_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); pg->x = 1; }
void m3_bind_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); int *q_ = &(pg->x); (void)q_; }
void m3_credit_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); std::construct_at(&(pg->x), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void m3_destroy_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); std::destroy_at(&(pg->x)); }
// m4, member: (*pg).x
int m4_read_before(Agg *pg [[ref_to_uninit]]) { return (*pg).x; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void m4_store_before(Agg *pg [[ref_to_uninit]]) { (*pg).x = 1; } // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it}}
void m4_bind_before(Agg *pg [[ref_to_uninit]]) { int *q_ = &((*pg).x); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void m4_credit_before(Agg *pg [[ref_to_uninit]]) { std::construct_at(&((*pg).x), 1); }
void m4_destroy_before(Agg *pg [[ref_to_uninit]]) { std::destroy_at(&((*pg).x)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int m4_read_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); return (*pg).x; }
void m4_store_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); (*pg).x = 1; }
void m4_bind_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); int *q_ = &((*pg).x); (void)q_; }
void m4_credit_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); std::construct_at(&((*pg).x), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void m4_destroy_after(Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); std::destroy_at(&((*pg).x)); }
// b1, base: d.b
int b1_read_before() { Der d [[uninit]]; return d.b; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void b1_store_before() { Der d [[uninit]]; d.b = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void b1_bind_before() { Der d [[uninit]]; int *q_ = &(d.b); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void b1_credit_before() { Der d [[uninit]]; std::construct_at(&(d.b), 1); }
void b1_destroy_before() { Der d [[uninit]]; std::destroy_at(&(d.b)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int b1_read_after() { Der d [[uninit]]; std::construct_at(&d); return d.b; }
void b1_store_after() { Der d [[uninit]]; std::construct_at(&d); d.b = 1; }
void b1_bind_after() { Der d [[uninit]]; std::construct_at(&d); int *q_ = &(d.b); (void)q_; }
void b1_credit_after() { Der d [[uninit]]; std::construct_at(&d); std::construct_at(&(d.b), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void b1_destroy_after() { Der d [[uninit]]; std::construct_at(&d); std::destroy_at(&(d.b)); }
// b2, base: static_cast<Base &>(d).b
int b2_read_before() { Der d [[uninit]]; return static_cast<Base &>(d).b; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void b2_store_before() { Der d [[uninit]]; static_cast<Base &>(d).b = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void b2_bind_before() { Der d [[uninit]]; int *q_ = &(static_cast<Base &>(d).b); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void b2_credit_before() { Der d [[uninit]]; std::construct_at(&(static_cast<Base &>(d).b), 1); }
void b2_destroy_before() { Der d [[uninit]]; std::destroy_at(&(static_cast<Base &>(d).b)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int b2_read_after() { Der d [[uninit]]; std::construct_at(&d); return static_cast<Base &>(d).b; }
void b2_store_after() { Der d [[uninit]]; std::construct_at(&d); static_cast<Base &>(d).b = 1; }
void b2_bind_after() { Der d [[uninit]]; std::construct_at(&d); int *q_ = &(static_cast<Base &>(d).b); (void)q_; }
void b2_credit_after() { Der d [[uninit]]; std::construct_at(&d); std::construct_at(&(static_cast<Base &>(d).b), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void b2_destroy_after() { Der d [[uninit]]; std::construct_at(&d); std::destroy_at(&(static_cast<Base &>(d).b)); }
// b3, base: ((Base *)&d)->b
int b3_read_before() { Der d [[uninit]]; return ((Base *)&d)->b; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void b3_store_before() { Der d [[uninit]]; ((Base *)&d)->b = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void b3_bind_before() { Der d [[uninit]]; int *q_ = &(((Base *)&d)->b); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void b3_credit_before() { Der d [[uninit]]; std::construct_at(&(((Base *)&d)->b), 1); }
void b3_destroy_before() { Der d [[uninit]]; std::destroy_at(&(((Base *)&d)->b)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int b3_read_after() { Der d [[uninit]]; std::construct_at(&d); return ((Base *)&d)->b; }
void b3_store_after() { Der d [[uninit]]; std::construct_at(&d); ((Base *)&d)->b = 1; }
void b3_bind_after() { Der d [[uninit]]; std::construct_at(&d); int *q_ = &(((Base *)&d)->b); (void)q_; }
void b3_credit_after() { Der d [[uninit]]; std::construct_at(&d); std::construct_at(&(((Base *)&d)->b), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void b3_destroy_after() { Der d [[uninit]]; std::construct_at(&d); std::destroy_at(&(((Base *)&d)->b)); }
// b4, base: d.Base::b
int b4_read_before() { Der d [[uninit]]; return d.Base::b; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void b4_store_before() { Der d [[uninit]]; d.Base::b = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void b4_bind_before() { Der d [[uninit]]; int *q_ = &(d.Base::b); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void b4_credit_before() { Der d [[uninit]]; std::construct_at(&(d.Base::b), 1); }
void b4_destroy_before() { Der d [[uninit]]; std::destroy_at(&(d.Base::b)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int b4_read_after() { Der d [[uninit]]; std::construct_at(&d); return d.Base::b; }
void b4_store_after() { Der d [[uninit]]; std::construct_at(&d); d.Base::b = 1; }
void b4_bind_after() { Der d [[uninit]]; std::construct_at(&d); int *q_ = &(d.Base::b); (void)q_; }
void b4_credit_after() { Der d [[uninit]]; std::construct_at(&d); std::construct_at(&(d.Base::b), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void b4_destroy_after() { Der d [[uninit]]; std::construct_at(&d); std::destroy_at(&(d.Base::b)); }
// e1, element: a[1]
int e1_read_before() { int a [[uninit]] [4]; return a[1]; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void e1_store_before() { int a [[uninit]] [4]; a[1] = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void e1_bind_before() { int a [[uninit]] [4]; int *q_ = &(a[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void e1_credit_before() { int a [[uninit]] [4]; std::construct_at(&(a[1]), 1); }
void e1_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(a[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int e1_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return a[1]; }
void e1_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); a[1] = 1; }
void e1_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(a[1]); (void)q_; }
void e1_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(a[1]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void e1_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(a[1])); }
// e2, element: 1[a]
int e2_read_before() { int a [[uninit]] [4]; return 1[a]; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void e2_store_before() { int a [[uninit]] [4]; 1[a] = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void e2_bind_before() { int a [[uninit]] [4]; int *q_ = &(1[a]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void e2_credit_before() { int a [[uninit]] [4]; std::construct_at(&(1[a]), 1); }
void e2_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(1[a])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int e2_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return 1[a]; }
void e2_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); 1[a] = 1; }
void e2_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(1[a]); (void)q_; }
void e2_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(1[a]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void e2_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(1[a])); }
// e3, element: p[1]
int e3_read_before(int *p [[ref_to_uninit]]) { return p[1]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void e3_store_before(int *p [[ref_to_uninit]]) { p[1] = 1; }
void e3_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(p[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void e3_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(p[1]), 1); }
void e3_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(p[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int e3_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return p[1]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void e3_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); p[1] = 1; }
void e3_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(p[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void e3_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(p[1]), 1); }
void e3_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(p[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// e4, element: p[0]
int e4_read_before(int *p [[ref_to_uninit]]) { return p[0]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void e4_store_before(int *p [[ref_to_uninit]]) { p[0] = 1; }
void e4_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(p[0]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void e4_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(p[0]), 1); }
void e4_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(p[0])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int e4_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return p[0]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void e4_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); p[0] = 1; }
void e4_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(p[0]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void e4_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(p[0]), 1); }
void e4_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(p[0])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// e5, element: *(a + 1)
int e5_read_before() { int a [[uninit]] [4]; return *(a + 1); }
void e5_store_before() { int a [[uninit]] [4]; *(a + 1) = 1; }
void e5_bind_before() { int a [[uninit]] [4]; int *q_ = &(*(a + 1)); (void)q_; }
void e5_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*(a + 1)), 1); }
void e5_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*(a + 1))); }
int e5_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *(a + 1); }
void e5_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *(a + 1) = 1; }
void e5_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*(a + 1)); (void)q_; }
void e5_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*(a + 1)), 1); }
void e5_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*(a + 1))); }
// e6, element: *(1 + a)
int e6_read_before() { int a [[uninit]] [4]; return *(1 + a); }
void e6_store_before() { int a [[uninit]] [4]; *(1 + a) = 1; }
void e6_bind_before() { int a [[uninit]] [4]; int *q_ = &(*(1 + a)); (void)q_; }
void e6_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*(1 + a)), 1); }
void e6_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*(1 + a))); }
int e6_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *(1 + a); }
void e6_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *(1 + a) = 1; }
void e6_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*(1 + a)); (void)q_; }
void e6_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*(1 + a)), 1); }
void e6_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*(1 + a))); }
// e7, element: *(p + 1)
int e7_read_before(int *p [[ref_to_uninit]]) { return *(p + 1); }
void e7_store_before(int *p [[ref_to_uninit]]) { *(p + 1) = 1; }
void e7_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(*(p + 1)); (void)q_; }
void e7_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(*(p + 1)), 1); }
void e7_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(*(p + 1))); }
int e7_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return *(p + 1); }
void e7_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); *(p + 1) = 1; }
void e7_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(*(p + 1)); (void)q_; }
void e7_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(*(p + 1)), 1); }
void e7_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(*(p + 1))); }
// e8, element: *(2 + p - 1)
int e8_read_before(int *p [[ref_to_uninit]]) { return *(2 + p - 1); }
void e8_store_before(int *p [[ref_to_uninit]]) { *(2 + p - 1) = 1; }
void e8_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(*(2 + p - 1)); (void)q_; }
void e8_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(*(2 + p - 1)), 1); }
void e8_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(*(2 + p - 1))); }
int e8_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return *(2 + p - 1); }
void e8_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); *(2 + p - 1) = 1; }
void e8_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(*(2 + p - 1)); (void)q_; }
void e8_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(*(2 + p - 1)), 1); }
void e8_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(*(2 + p - 1))); }
// d1, deref: *p
int d1_read_before(int *p [[ref_to_uninit]]) { return *p; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void d1_store_before(int *p [[ref_to_uninit]]) { *p = 1; }
void d1_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(*p); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void d1_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(*p), 1); }
void d1_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(*p)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int d1_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return *p; }
void d1_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); *p = 1; }
void d1_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(*p); (void)q_; }
void d1_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(*p), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void d1_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(*p)); }
// d2, deref: *a
int d2_read_before() { int a [[uninit]] [4]; return *a; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void d2_store_before() { int a [[uninit]] [4]; *a = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void d2_bind_before() { int a [[uninit]] [4]; int *q_ = &(*a); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void d2_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*a), 1); }
void d2_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*a)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int d2_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *a; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void d2_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *a = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void d2_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*a); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void d2_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*a), 1); }
void d2_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*a)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// d3, deref: *u
int d3_read_before(std::unique_ptr<int> u [[ref_to_uninit]]) { return *u; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void d3_store_before(std::unique_ptr<int> u [[ref_to_uninit]]) { *u = 1; }
void d3_bind_before(std::unique_ptr<int> u [[ref_to_uninit]]) { int *q_ = &(*u); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void d3_credit_before(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(&(*u), 1); }
void d3_destroy_before(std::unique_ptr<int> u [[ref_to_uninit]]) { std::destroy_at(&(*u)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}} expected-error {{storage already destroyed by its scope-exit destructor is destroyed again}} expected-note {{'u' is destroyed at the end of its scope}}
int d3_read_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); return *u; }
void d3_store_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); *u = 1; }
void d3_bind_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); int *q_ = &(*u); (void)q_; }
void d3_credit_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); std::construct_at(&(*u), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void d3_destroy_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); std::destroy_at(&(*u)); } // expected-error {{storage already destroyed by its scope-exit destructor is destroyed again}} expected-note {{'u' is destroyed at the end of its scope}}
// a1, address-of: *&a[1]
int a1_read_before() { int a [[uninit]] [4]; return *&a[1]; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void a1_store_before() { int a [[uninit]] [4]; *&a[1] = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void a1_bind_before() { int a [[uninit]] [4]; int *q_ = &(*&a[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void a1_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*&a[1]), 1); }
void a1_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*&a[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int a1_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *&a[1]; }
void a1_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *&a[1] = 1; }
void a1_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*&a[1]); (void)q_; }
void a1_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*&a[1]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void a1_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*&a[1])); }
// a2, address-of: *&g.x
int a2_read_before() { Agg g [[uninit]]; return *&g.x; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void a2_store_before() { Agg g [[uninit]]; *&g.x = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void a2_bind_before() { Agg g [[uninit]]; int *q_ = &(*&g.x); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void a2_credit_before() { Agg g [[uninit]]; std::construct_at(&(*&g.x), 1); }
void a2_destroy_before() { Agg g [[uninit]]; std::destroy_at(&(*&g.x)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int a2_read_after() { Agg g [[uninit]]; std::construct_at(&g); return *&g.x; }
void a2_store_after() { Agg g [[uninit]]; std::construct_at(&g); *&g.x = 1; }
void a2_bind_after() { Agg g [[uninit]]; std::construct_at(&g); int *q_ = &(*&g.x); (void)q_; }
void a2_credit_after() { Agg g [[uninit]]; std::construct_at(&g); std::construct_at(&(*&g.x), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void a2_destroy_after() { Agg g [[uninit]]; std::construct_at(&g); std::destroy_at(&(*&g.x)); }
// a3, address-of: *&*p
int a3_read_before(int *p [[ref_to_uninit]]) { return *&*p; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void a3_store_before(int *p [[ref_to_uninit]]) { *&*p = 1; }
void a3_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(*&*p); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void a3_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(*&*p), 1); }
void a3_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(*&*p)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int a3_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return *&*p; }
void a3_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); *&*p = 1; }
void a3_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(*&*p); (void)q_; }
void a3_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(*&*p), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void a3_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(*&*p)); }
// i1, identity call: *std::addressof(g.x)
int i1_read_before() { Agg g [[uninit]]; return *std::addressof(g.x); } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void i1_store_before() { Agg g [[uninit]]; *std::addressof(g.x) = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void i1_bind_before() { Agg g [[uninit]]; int *q_ = &(*std::addressof(g.x)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void i1_credit_before() { Agg g [[uninit]]; std::construct_at(&(*std::addressof(g.x)), 1); }
void i1_destroy_before() { Agg g [[uninit]]; std::destroy_at(&(*std::addressof(g.x))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int i1_read_after() { Agg g [[uninit]]; std::construct_at(&g); return *std::addressof(g.x); }
void i1_store_after() { Agg g [[uninit]]; std::construct_at(&g); *std::addressof(g.x) = 1; }
void i1_bind_after() { Agg g [[uninit]]; std::construct_at(&g); int *q_ = &(*std::addressof(g.x)); (void)q_; }
void i1_credit_after() { Agg g [[uninit]]; std::construct_at(&g); std::construct_at(&(*std::addressof(g.x)), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void i1_destroy_after() { Agg g [[uninit]]; std::construct_at(&g); std::destroy_at(&(*std::addressof(g.x))); }
// i2, identity call: *std::addressof(a[1])
int i2_read_before() { int a [[uninit]] [4]; return *std::addressof(a[1]); } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void i2_store_before() { int a [[uninit]] [4]; *std::addressof(a[1]) = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void i2_bind_before() { int a [[uninit]] [4]; int *q_ = &(*std::addressof(a[1])); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void i2_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*std::addressof(a[1])), 1); }
void i2_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*std::addressof(a[1]))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int i2_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *std::addressof(a[1]); }
void i2_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *std::addressof(a[1]) = 1; }
void i2_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*std::addressof(a[1])); (void)q_; }
void i2_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*std::addressof(a[1])), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void i2_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*std::addressof(a[1]))); }
// i3, identity call: std::as_const(g).x
int i3_read_before() { Agg g [[uninit]]; return std::as_const(g).x; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void i3_bind_before() { Agg g [[uninit]]; const int *q_ = &(std::as_const(g).x); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
int i3_read_after() { Agg g [[uninit]]; std::construct_at(&g); return std::as_const(g).x; }
void i3_bind_after() { Agg g [[uninit]]; std::construct_at(&g); const int *q_ = &(std::as_const(g).x); (void)q_; }
// i4, identity call: std::as_const(*p)
int i4_read_before(int *p [[ref_to_uninit]]) { return std::as_const(*p); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void i4_bind_before(int *p [[ref_to_uninit]]) { const int *q_ = &(std::as_const(*p)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
int i4_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return std::as_const(*p); }
void i4_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); const int *q_ = &(std::as_const(*p)); (void)q_; }
// v1, derive: *std::begin(a)
int v1_read_before() { int a [[uninit]] [4]; return *std::begin(a); } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void v1_store_before() { int a [[uninit]] [4]; *std::begin(a) = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void v1_bind_before() { int a [[uninit]] [4]; int *q_ = &(*std::begin(a)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v1_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*std::begin(a)), 1); }
void v1_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*std::begin(a))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v1_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *std::begin(a); }
void v1_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *std::begin(a) = 1; }
void v1_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*std::begin(a)); (void)q_; }
void v1_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*std::begin(a)), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void v1_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*std::begin(a))); }
// v2, derive: std::begin(a)[1]
int v2_read_before() { int a [[uninit]] [4]; return std::begin(a)[1]; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void v2_store_before() { int a [[uninit]] [4]; std::begin(a)[1] = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void v2_bind_before() { int a [[uninit]] [4]; int *q_ = &(std::begin(a)[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v2_credit_before() { int a [[uninit]] [4]; std::construct_at(&(std::begin(a)[1]), 1); }
void v2_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(std::begin(a)[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v2_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return std::begin(a)[1]; }
void v2_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::begin(a)[1] = 1; }
void v2_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(std::begin(a)[1]); (void)q_; }
void v2_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(std::begin(a)[1]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void v2_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(std::begin(a)[1])); }
// v3, derive: *std::data(a)
int v3_read_before() { int a [[uninit]] [4]; return *std::data(a); } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void v3_store_before() { int a [[uninit]] [4]; *std::data(a) = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void v3_bind_before() { int a [[uninit]] [4]; int *q_ = &(*std::data(a)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v3_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*std::data(a)), 1); }
void v3_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*std::data(a))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v3_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *std::data(a); }
void v3_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *std::data(a) = 1; }
void v3_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*std::data(a)); (void)q_; }
void v3_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*std::data(a)), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void v3_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*std::data(a))); }
// v4, derive: *std::next(std::begin(a))
int v4_read_before() { int a [[uninit]] [4]; return *std::next(std::begin(a)); } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void v4_store_before() { int a [[uninit]] [4]; *std::next(std::begin(a)) = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void v4_bind_before() { int a [[uninit]] [4]; int *q_ = &(*std::next(std::begin(a))); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v4_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*std::next(std::begin(a))), 1); }
void v4_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*std::next(std::begin(a)))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v4_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *std::next(std::begin(a)); }
void v4_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *std::next(std::begin(a)) = 1; }
void v4_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*std::next(std::begin(a))); (void)q_; }
void v4_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*std::next(std::begin(a))), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void v4_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*std::next(std::begin(a)))); }
// v5, derive: *(std::begin(a) + 1)
int v5_read_before() { int a [[uninit]] [4]; return *(std::begin(a) + 1); }
void v5_store_before() { int a [[uninit]] [4]; *(std::begin(a) + 1) = 1; }
void v5_bind_before() { int a [[uninit]] [4]; int *q_ = &(*(std::begin(a) + 1)); (void)q_; }
void v5_credit_before() { int a [[uninit]] [4]; std::construct_at(&(*(std::begin(a) + 1)), 1); }
void v5_destroy_before() { int a [[uninit]] [4]; std::destroy_at(&(*(std::begin(a) + 1))); }
int v5_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return *(std::begin(a) + 1); }
void v5_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); *(std::begin(a) + 1) = 1; }
void v5_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int *q_ = &(*(std::begin(a) + 1)); (void)q_; }
void v5_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::construct_at(&(*(std::begin(a) + 1)), 1); }
void v5_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::destroy_at(&(*(std::begin(a) + 1))); }
// v6, derive: *std::to_address(p)
int v6_read_before(int *p [[ref_to_uninit]]) { return *std::to_address(p); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void v6_store_before(int *p [[ref_to_uninit]]) { *std::to_address(p) = 1; }
void v6_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(*std::to_address(p)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v6_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(*std::to_address(p)), 1); }
void v6_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(*std::to_address(p))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v6_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return *std::to_address(p); }
void v6_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); *std::to_address(p) = 1; }
void v6_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(*std::to_address(p)); (void)q_; }
void v6_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(*std::to_address(p)), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void v6_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(*std::to_address(p))); }
// v7, derive: *std::next(p)
int v7_read_before(int *p [[ref_to_uninit]]) { return *std::next(p); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void v7_store_before(int *p [[ref_to_uninit]]) { *std::next(p) = 1; }
void v7_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(*std::next(p)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v7_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(*std::next(p)), 1); }
void v7_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(*std::next(p))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v7_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return *std::next(p); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void v7_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); *std::next(p) = 1; }
void v7_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(*std::next(p)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v7_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(*std::next(p)), 1); }
void v7_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(*std::next(p))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// v8, derive: *std::begin(s)
int v8_read_before(std::span<int> s [[ref_to_uninit]]) { return *std::begin(s); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void v8_store_before(std::span<int> s [[ref_to_uninit]]) { *std::begin(s) = 1; }
void v8_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(*std::begin(s)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v8_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(*std::begin(s)), 1); }
void v8_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(*std::begin(s))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v8_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return *std::begin(s); }
void v8_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); *std::begin(s) = 1; }
void v8_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(*std::begin(s)); (void)q_; }
void v8_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(*std::begin(s)), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void v8_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(*std::begin(s))); }
// v9, derive: *std::ranges::begin(s)
int v9_read_before(std::span<int> s [[ref_to_uninit]]) { return *std::ranges::begin(s); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void v9_store_before(std::span<int> s [[ref_to_uninit]]) { *std::ranges::begin(s) = 1; }
void v9_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(*std::ranges::begin(s)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void v9_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(*std::ranges::begin(s)), 1); }
void v9_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(*std::ranges::begin(s))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int v9_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return *std::ranges::begin(s); }
void v9_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); *std::ranges::begin(s) = 1; }
void v9_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(*std::ranges::begin(s)); (void)q_; }
void v9_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(*std::ranges::begin(s)), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void v9_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(*std::ranges::begin(s))); }
// h1, handle accessor: *s.begin()
int h1_read_before(std::span<int> s [[ref_to_uninit]]) { return *s.begin(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h1_store_before(std::span<int> s [[ref_to_uninit]]) { *s.begin() = 1; }
void h1_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(*s.begin()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h1_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(*s.begin()), 1); }
void h1_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(*s.begin())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int h1_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return *s.begin(); }
void h1_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); *s.begin() = 1; }
void h1_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(*s.begin()); (void)q_; }
void h1_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(*s.begin()), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void h1_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(*s.begin())); }
// h2, handle accessor: *s.data()
int h2_read_before(std::span<int> s [[ref_to_uninit]]) { return *s.data(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h2_store_before(std::span<int> s [[ref_to_uninit]]) { *s.data() = 1; }
void h2_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(*s.data()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h2_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(*s.data()), 1); }
void h2_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(*s.data())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int h2_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return *s.data(); }
void h2_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); *s.data() = 1; }
void h2_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(*s.data()); (void)q_; }
void h2_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(*s.data()), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void h2_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(*s.data())); }
// h3, handle accessor: s.front()
int h3_read_before(std::span<int> s [[ref_to_uninit]]) { return s.front(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h3_store_before(std::span<int> s [[ref_to_uninit]]) { s.front() = 1; }
void h3_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(s.front()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h3_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(s.front()), 1); }
void h3_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(s.front())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int h3_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return s.front(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h3_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); s.front() = 1; }
void h3_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(s.front()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h3_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(s.front()), 1); }
void h3_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(s.front())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// h4, handle accessor: s[1]
int h4_read_before(std::span<int> s [[ref_to_uninit]]) { return s[1]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h4_store_before(std::span<int> s [[ref_to_uninit]]) { s[1] = 1; }
void h4_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(s[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h4_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(s[1]), 1); }
void h4_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(s[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int h4_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return s[1]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h4_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); s[1] = 1; }
void h4_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(s[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h4_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(s[1]), 1); }
void h4_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(s[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// h5, handle accessor: *(s.begin() + 1)
int h5_read_before(std::span<int> s [[ref_to_uninit]]) { return *(s.begin() + 1); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h5_store_before(std::span<int> s [[ref_to_uninit]]) { *(s.begin() + 1) = 1; }
void h5_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(*(s.begin() + 1)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h5_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(*(s.begin() + 1)), 1); }
void h5_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(*(s.begin() + 1))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int h5_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return *(s.begin() + 1); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h5_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); *(s.begin() + 1) = 1; }
void h5_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(*(s.begin() + 1)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h5_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(*(s.begin() + 1)), 1); }
void h5_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(*(s.begin() + 1))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// h6, handle accessor: *(s.data() + 1)
int h6_read_before(std::span<int> s [[ref_to_uninit]]) { return *(s.data() + 1); }
void h6_store_before(std::span<int> s [[ref_to_uninit]]) { *(s.data() + 1) = 1; }
void h6_bind_before(std::span<int> s [[ref_to_uninit]]) { int *q_ = &(*(s.data() + 1)); (void)q_; }
void h6_credit_before(std::span<int> s [[ref_to_uninit]]) { std::construct_at(&(*(s.data() + 1)), 1); }
void h6_destroy_before(std::span<int> s [[ref_to_uninit]]) { std::destroy_at(&(*(s.data() + 1))); }
int h6_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return *(s.data() + 1); }
void h6_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); *(s.data() + 1) = 1; }
void h6_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int *q_ = &(*(s.data() + 1)); (void)q_; }
void h6_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::construct_at(&(*(s.data() + 1)), 1); }
void h6_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); std::destroy_at(&(*(s.data() + 1))); }
// h7, handle accessor: *u.get()
int h7_read_before(std::unique_ptr<int> u [[ref_to_uninit]]) { return *u.get(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void h7_store_before(std::unique_ptr<int> u [[ref_to_uninit]]) { *u.get() = 1; }
void h7_bind_before(std::unique_ptr<int> u [[ref_to_uninit]]) { int *q_ = &(*u.get()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void h7_credit_before(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(&(*u.get()), 1); }
void h7_destroy_before(std::unique_ptr<int> u [[ref_to_uninit]]) { std::destroy_at(&(*u.get())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}} expected-error {{storage already destroyed by its scope-exit destructor is destroyed again}} expected-note {{'u' is destroyed at the end of its scope}}
int h7_read_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); return *u.get(); }
void h7_store_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); *u.get() = 1; }
void h7_bind_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); int *q_ = &(*u.get()); (void)q_; }
void h7_credit_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); std::construct_at(&(*u.get()), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void h7_destroy_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); std::destroy_at(&(*u.get())); } // expected-error {{storage already destroyed by its scope-exit destructor is destroyed again}} expected-note {{'u' is destroyed at the end of its scope}}
// h8, handle accessor: *(u.get() + 1)
int h8_read_before(std::unique_ptr<int> u [[ref_to_uninit]]) { return *(u.get() + 1); }
void h8_store_before(std::unique_ptr<int> u [[ref_to_uninit]]) { *(u.get() + 1) = 1; }
void h8_bind_before(std::unique_ptr<int> u [[ref_to_uninit]]) { int *q_ = &(*(u.get() + 1)); (void)q_; }
void h8_credit_before(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(&(*(u.get() + 1)), 1); }
void h8_destroy_before(std::unique_ptr<int> u [[ref_to_uninit]]) { std::destroy_at(&(*(u.get() + 1))); }
int h8_read_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); return *(u.get() + 1); }
void h8_store_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); *(u.get() + 1) = 1; }
void h8_bind_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); int *q_ = &(*(u.get() + 1)); (void)q_; }
void h8_credit_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); std::construct_at(&(*(u.get() + 1)), 1); }
void h8_destroy_after(std::unique_ptr<int> u [[ref_to_uninit]]) { std::construct_at(u.get(), 0); std::destroy_at(&(*(u.get() + 1))); }
// t1, temporary handle: std::span<int>(p, 4)[1]
int t1_read_before(int *p [[ref_to_uninit]]) { return std::span<int>(p, 4)[1]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void t1_store_before(int *p [[ref_to_uninit]]) { std::span<int>(p, 4)[1] = 1; }
void t1_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(std::span<int>(p, 4)[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void t1_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(std::span<int>(p, 4)[1]), 1); }
void t1_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(std::span<int>(p, 4)[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int t1_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return std::span<int>(p, 4)[1]; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void t1_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::span<int>(p, 4)[1] = 1; }
void t1_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(std::span<int>(p, 4)[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void t1_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(std::span<int>(p, 4)[1]), 1); }
void t1_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(std::span<int>(p, 4)[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// t2, temporary handle: *std::span<int>(p, 4).data()
int t2_read_before(int *p [[ref_to_uninit]]) { return *std::span<int>(p, 4).data(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void t2_store_before(int *p [[ref_to_uninit]]) { *std::span<int>(p, 4).data() = 1; }
void t2_bind_before(int *p [[ref_to_uninit]]) { int *q_ = &(*std::span<int>(p, 4).data()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void t2_credit_before(int *p [[ref_to_uninit]]) { std::construct_at(&(*std::span<int>(p, 4).data()), 1); }
void t2_destroy_before(int *p [[ref_to_uninit]]) { std::destroy_at(&(*std::span<int>(p, 4).data())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int t2_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return *std::span<int>(p, 4).data(); }
void t2_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); *std::span<int>(p, 4).data() = 1; }
void t2_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int *q_ = &(*std::span<int>(p, 4).data()); (void)q_; }
void t2_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::construct_at(&(*std::span<int>(p, 4).data()), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void t2_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); std::destroy_at(&(*std::span<int>(p, 4).data())); }
// r1, std::array member: r[1]
int r1_read_before() { std::array<int, 4> r [[uninit]]; return r[1]; } // expected-error {{calling member function 'operator[]' binds its implicit object parameter to uninitialized memory}}
void r1_store_before() { std::array<int, 4> r [[uninit]]; r[1] = 1; } // expected-error {{calling member function 'operator[]' binds its implicit object parameter to uninitialized memory}}
void r1_bind_before() { std::array<int, 4> r [[uninit]]; int *q_ = &(r[1]); (void)q_; } // expected-error {{calling member function 'operator[]' binds its implicit object parameter to uninitialized memory}}
void r1_credit_before() { std::array<int, 4> r [[uninit]]; std::construct_at(&(r[1]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}} expected-error {{calling member function 'operator[]' binds its implicit object parameter to uninitialized memory}}
void r1_destroy_before() { std::array<int, 4> r [[uninit]]; std::destroy_at(&(r[1])); } // expected-error {{calling member function 'operator[]' binds its implicit object parameter to uninitialized memory}}
int r1_read_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); return r[1]; }
void r1_store_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); r[1] = 1; }
void r1_bind_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); int *q_ = &(r[1]); (void)q_; }
void r1_credit_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::construct_at(&(r[1]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void r1_destroy_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::destroy_at(&(r[1])); }
// r2, std::array member: *r.data()
int r2_read_before() { std::array<int, 4> r [[uninit]]; return *r.data(); } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void r2_store_before() { std::array<int, 4> r [[uninit]]; *r.data() = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void r2_bind_before() { std::array<int, 4> r [[uninit]]; int *q_ = &(*r.data()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void r2_credit_before() { std::array<int, 4> r [[uninit]]; std::construct_at(&(*r.data()), 1); }
void r2_destroy_before() { std::array<int, 4> r [[uninit]]; std::destroy_at(&(*r.data())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int r2_read_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); return *r.data(); }
void r2_store_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); *r.data() = 1; }
void r2_bind_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); int *q_ = &(*r.data()); (void)q_; }
void r2_credit_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::construct_at(&(*r.data()), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void r2_destroy_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::destroy_at(&(*r.data())); }
// r3, std::array member: *r.begin()
int r3_read_before() { std::array<int, 4> r [[uninit]]; return *r.begin(); } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void r3_store_before() { std::array<int, 4> r [[uninit]]; *r.begin() = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void r3_bind_before() { std::array<int, 4> r [[uninit]]; int *q_ = &(*r.begin()); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void r3_credit_before() { std::array<int, 4> r [[uninit]]; std::construct_at(&(*r.begin()), 1); }
void r3_destroy_before() { std::array<int, 4> r [[uninit]]; std::destroy_at(&(*r.begin())); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int r3_read_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); return *r.begin(); }
void r3_store_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); *r.begin() = 1; }
void r3_bind_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); int *q_ = &(*r.begin()); (void)q_; }
void r3_credit_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::construct_at(&(*r.begin()), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void r3_destroy_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::destroy_at(&(*r.begin())); }
// r4, std::array member: r.data()[1]
int r4_read_before() { std::array<int, 4> r [[uninit]]; return r.data()[1]; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void r4_store_before() { std::array<int, 4> r [[uninit]]; r.data()[1] = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void r4_bind_before() { std::array<int, 4> r [[uninit]]; int *q_ = &(r.data()[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void r4_credit_before() { std::array<int, 4> r [[uninit]]; std::construct_at(&(r.data()[1]), 1); }
void r4_destroy_before() { std::array<int, 4> r [[uninit]]; std::destroy_at(&(r.data()[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int r4_read_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); return r.data()[1]; }
void r4_store_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); r.data()[1] = 1; }
void r4_bind_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); int *q_ = &(r.data()[1]); (void)q_; }
void r4_credit_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::construct_at(&(r.data()[1]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void r4_destroy_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::destroy_at(&(r.data()[1])); }
// r5, std::array member: *(r.data() + 1)
int r5_read_before() { std::array<int, 4> r [[uninit]]; return *(r.data() + 1); }
void r5_store_before() { std::array<int, 4> r [[uninit]]; *(r.data() + 1) = 1; }
void r5_bind_before() { std::array<int, 4> r [[uninit]]; int *q_ = &(*(r.data() + 1)); (void)q_; }
void r5_credit_before() { std::array<int, 4> r [[uninit]]; std::construct_at(&(*(r.data() + 1)), 1); }
void r5_destroy_before() { std::array<int, 4> r [[uninit]]; std::destroy_at(&(*(r.data() + 1))); }
int r5_read_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); return *(r.data() + 1); }
void r5_store_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); *(r.data() + 1) = 1; }
void r5_bind_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); int *q_ = &(*(r.data() + 1)); (void)q_; }
void r5_credit_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::construct_at(&(*(r.data() + 1)), 1); }
void r5_destroy_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::destroy_at(&(*(r.data() + 1))); }
// r6, std::array member: *(r.begin() + 1)
int r6_read_before() { std::array<int, 4> r [[uninit]]; return *(r.begin() + 1); }
void r6_store_before() { std::array<int, 4> r [[uninit]]; *(r.begin() + 1) = 1; }
void r6_bind_before() { std::array<int, 4> r [[uninit]]; int *q_ = &(*(r.begin() + 1)); (void)q_; }
void r6_credit_before() { std::array<int, 4> r [[uninit]]; std::construct_at(&(*(r.begin() + 1)), 1); }
void r6_destroy_before() { std::array<int, 4> r [[uninit]]; std::destroy_at(&(*(r.begin() + 1))); }
int r6_read_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); return *(r.begin() + 1); }
void r6_store_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); *(r.begin() + 1) = 1; }
void r6_bind_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); int *q_ = &(*(r.begin() + 1)); (void)q_; }
void r6_credit_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::construct_at(&(*(r.begin() + 1)), 1); }
void r6_destroy_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); std::destroy_at(&(*(r.begin() + 1))); }
// c1, conditional: (c ? a : b)[1]
int c1_read_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; return (c ? a : b)[1]; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void c1_store_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; (c ? a : b)[1] = 1; } // expected-error {{writing an element of an '[[uninit]]' object does not initialize it}}
void c1_bind_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; int *q_ = &((c ? a : b)[1]); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void c1_credit_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::construct_at(&((c ? a : b)[1]), 1); }
void c1_destroy_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::destroy_at(&((c ? a : b)[1])); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int c1_read_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); return (c ? a : b)[1]; }
void c1_store_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); (c ? a : b)[1] = 1; }
void c1_bind_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); int *q_ = &((c ? a : b)[1]); (void)q_; }
void c1_credit_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); std::construct_at(&((c ? a : b)[1]), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void c1_destroy_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); std::destroy_at(&((c ? a : b)[1])); }
// c2, conditional: *(c ? p : q)
int c2_read_before(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { return *(c ? p : q); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void c2_store_before(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { *(c ? p : q) = 1; }
void c2_bind_before(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { int *q_ = &(*(c ? p : q)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void c2_credit_before(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { std::construct_at(&(*(c ? p : q)), 1); }
void c2_destroy_before(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { std::destroy_at(&(*(c ? p : q))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int c2_read_after(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { std::construct_at(p, 0); std::construct_at(q, 0); return *(c ? p : q); }
void c2_store_after(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { std::construct_at(p, 0); std::construct_at(q, 0); *(c ? p : q) = 1; }
void c2_bind_after(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { std::construct_at(p, 0); std::construct_at(q, 0); int *q_ = &(*(c ? p : q)); (void)q_; }
void c2_credit_after(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { std::construct_at(p, 0); std::construct_at(q, 0); std::construct_at(&(*(c ? p : q)), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void c2_destroy_after(bool c, int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) { std::construct_at(p, 0); std::construct_at(q, 0); std::destroy_at(&(*(c ? p : q))); }
// c3, conditional: (c ? g : h).x
int c3_read_before(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; return (c ? g : h).x; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void c3_store_before(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; (c ? g : h).x = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void c3_bind_before(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; int *q_ = &((c ? g : h).x); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void c3_credit_before(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; std::construct_at(&((c ? g : h).x), 1); }
void c3_destroy_before(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; std::destroy_at(&((c ? g : h).x)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int c3_read_after(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; std::construct_at(&g); std::construct_at(&h); return (c ? g : h).x; }
void c3_store_after(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; std::construct_at(&g); std::construct_at(&h); (c ? g : h).x = 1; }
void c3_bind_after(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; std::construct_at(&g); std::construct_at(&h); int *q_ = &((c ? g : h).x); (void)q_; }
void c3_credit_after(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; std::construct_at(&g); std::construct_at(&h); std::construct_at(&((c ? g : h).x), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void c3_destroy_after(bool c) { Agg g [[uninit]]; Agg h [[uninit]]; std::construct_at(&g); std::construct_at(&h); std::destroy_at(&((c ? g : h).x)); }
// c4, conditional: *(c ? a : b)
int c4_read_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; return *(c ? a : b); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void c4_store_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; *(c ? a : b) = 1; } // expected-error {{writing an element of uninitialized storage does not initialize it}}
void c4_bind_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; int *q_ = &(*(c ? a : b)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void c4_credit_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::construct_at(&(*(c ? a : b)), 1); }
void c4_destroy_before(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::destroy_at(&(*(c ? a : b))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int c4_read_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); return *(c ? a : b); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void c4_store_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); *(c ? a : b) = 1; } // expected-error {{writing an element of uninitialized storage does not initialize it}}
void c4_bind_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); int *q_ = &(*(c ? a : b)); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void c4_credit_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); std::construct_at(&(*(c ? a : b)), 1); }
void c4_destroy_after(bool c) { int a [[uninit]] [4]; int b [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); std::uninitialized_fill(b, b + 4, 0); std::destroy_at(&(*(c ? a : b))); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// q1, pointer to data member: g.*pm
int q1_read_before(int Agg::*pm) { Agg g [[uninit]]; return g.*pm; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void q1_store_before(int Agg::*pm) { Agg g [[uninit]]; g.*pm = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void q1_bind_before(int Agg::*pm) { Agg g [[uninit]]; int *q_ = &(g.*pm); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void q1_credit_before(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&(g.*pm), 1); }
void q1_destroy_before(int Agg::*pm) { Agg g [[uninit]]; std::destroy_at(&(g.*pm)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int q1_read_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); return g.*pm; }
void q1_store_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); g.*pm = 1; }
void q1_bind_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); int *q_ = &(g.*pm); (void)q_; }
void q1_credit_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); std::construct_at(&(g.*pm), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void q1_destroy_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); std::destroy_at(&(g.*pm)); }
// q2, pointer to data member: (&g)->*pm
int q2_read_before(int Agg::*pm) { Agg g [[uninit]]; return (&g)->*pm; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
void q2_store_before(int Agg::*pm) { Agg g [[uninit]]; (&g)->*pm = 1; } // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
void q2_bind_before(int Agg::*pm) { Agg g [[uninit]]; int *q_ = &((&g)->*pm); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void q2_credit_before(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&((&g)->*pm), 1); }
void q2_destroy_before(int Agg::*pm) { Agg g [[uninit]]; std::destroy_at(&((&g)->*pm)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int q2_read_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); return (&g)->*pm; }
void q2_store_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); (&g)->*pm = 1; }
void q2_bind_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); int *q_ = &((&g)->*pm); (void)q_; }
void q2_credit_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); std::construct_at(&((&g)->*pm), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void q2_destroy_after(int Agg::*pm) { Agg g [[uninit]]; std::construct_at(&g); std::destroy_at(&((&g)->*pm)); }
// q3, pointer to data member: pg->*pm
int q3_read_before(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { return pg->*pm; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void q3_store_before(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { pg->*pm = 1; } // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it}}
void q3_bind_before(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { int *q_ = &(pg->*pm); (void)q_; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void q3_credit_before(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { std::construct_at(&(pg->*pm), 1); }
void q3_destroy_before(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { std::destroy_at(&(pg->*pm)); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
int q3_read_after(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); return pg->*pm; }
void q3_store_after(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); pg->*pm = 1; }
void q3_bind_after(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); int *q_ = &(pg->*pm); (void)q_; }
void q3_credit_after(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); std::construct_at(&(pg->*pm), 1); } // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void q3_destroy_after(int Agg::*pm, Agg *pg [[ref_to_uninit]]) { std::construct_at(pg); std::destroy_at(&(pg->*pm)); }
// z1, conditional reseat: *s1.data()
int z1_read_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); (c ? s1 : s2) = std::span<int>(r, 4); return *s1.data(); }
void z1_store_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); (c ? s1 : s2) = std::span<int>(r, 4); *s1.data() = 1; }
void z1_bind_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); (c ? s1 : s2) = std::span<int>(r, 4); int *q_ = &(*s1.data()); (void)q_; }
void z1_credit_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); (c ? s1 : s2) = std::span<int>(r, 4); std::construct_at(&(*s1.data()), 1); }
void z1_destroy_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); (c ? s1 : s2) = std::span<int>(r, 4); std::destroy_at(&(*s1.data())); }
int z1_read_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); (c ? s1 : s2) = std::span<int>(r, 4); return *s1.data(); }
void z1_store_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); (c ? s1 : s2) = std::span<int>(r, 4); *s1.data() = 1; }
void z1_bind_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); (c ? s1 : s2) = std::span<int>(r, 4); int *q_ = &(*s1.data()); (void)q_; }
void z1_credit_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); (c ? s1 : s2) = std::span<int>(r, 4); std::construct_at(&(*s1.data()), 1); }
void z1_destroy_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::span<int> s1 [[ref_to_uninit]](r1, 4); std::span<int> s2 [[ref_to_uninit]](r2, 4); std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); (c ? s1 : s2) = std::span<int>(r, 4); std::destroy_at(&(*s1.data())); }
// z2, conditional reseat: *r1
int z2_read_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { (c ? r1 : r2) = r; return *r1; }
void z2_store_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { (c ? r1 : r2) = r; *r1 = 1; }
void z2_bind_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { (c ? r1 : r2) = r; int *q_ = &(*r1); (void)q_; }
void z2_credit_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { (c ? r1 : r2) = r; std::construct_at(&(*r1), 1); }
void z2_destroy_before(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { (c ? r1 : r2) = r; std::destroy_at(&(*r1)); }
int z2_read_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::construct_at(r1, 0); std::construct_at(r2, 0); (c ? r1 : r2) = r; return *r1; }
void z2_store_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::construct_at(r1, 0); std::construct_at(r2, 0); (c ? r1 : r2) = r; *r1 = 1; }
void z2_bind_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::construct_at(r1, 0); std::construct_at(r2, 0); (c ? r1 : r2) = r; int *q_ = &(*r1); (void)q_; }
void z2_credit_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::construct_at(r1, 0); std::construct_at(r2, 0); (c ? r1 : r2) = r; std::construct_at(&(*r1), 1); }
void z2_destroy_after(bool c, int *r [[ref_to_uninit]], int *r1 [[ref_to_uninit]], int *r2 [[ref_to_uninit]]) { std::construct_at(r1, 0); std::construct_at(r2, 0); (c ? r1 : r2) = r; std::destroy_at(&(*r1)); }
// f1, range-for: for (... : a)
int f1_read_before() { int a [[uninit]] [4]; int t = 0; for (int x : a) t += x; return t; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f1_store_before() { int a [[uninit]] [4]; for (int &x : a) x = 1; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f1_bind_before() { int a [[uninit]] [4]; for (int &x : a) (void)x; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f1_credit_before() { int a [[uninit]] [4]; for (int &x [[ref_to_uninit]] : a) std::construct_at(&x, 1); } // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory}} expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f1_destroy_before() { int a [[uninit]] [4]; for (int &x : a) std::destroy_at(&x); } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
int f1_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); int t = 0; for (int x : a) t += x; return t; }
void f1_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); for (int &x : a) x = 1; }
void f1_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); for (int &x : a) (void)x; }
void f1_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); for (int &x [[ref_to_uninit]] : a) std::construct_at(&x, 1); } // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void f1_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); for (int &x : a) std::destroy_at(&x); }
// f2, range-for: for (... : s)
int f2_read_before(std::span<int> s [[ref_to_uninit]]) { int t = 0; for (int x : s) t += x; return t; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void f2_store_before(std::span<int> s [[ref_to_uninit]]) { for (int &x : s) x = 1; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f2_bind_before(std::span<int> s [[ref_to_uninit]]) { for (int &x : s) (void)x; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f2_credit_before(std::span<int> s [[ref_to_uninit]]) { for (int &x [[ref_to_uninit]] : s) std::construct_at(&x, 1); }
void f2_destroy_before(std::span<int> s [[ref_to_uninit]]) { for (int &x : s) std::destroy_at(&x); } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
int f2_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); int t = 0; for (int x : s) t += x; return t; }
void f2_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); for (int &x : s) x = 1; }
void f2_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); for (int &x : s) (void)x; }
void f2_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); for (int &x [[ref_to_uninit]] : s) std::construct_at(&x, 1); } // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void f2_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); for (int &x : s) std::destroy_at(&x); }
// f3, range-for: for (... : std::span<int>(p, 4))
int f3_read_before(int *p [[ref_to_uninit]]) { int t = 0; for (int x : std::span<int>(p, 4)) t += x; return t; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void f3_store_before(int *p [[ref_to_uninit]]) { for (int &x : std::span<int>(p, 4)) x = 1; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f3_bind_before(int *p [[ref_to_uninit]]) { for (int &x : std::span<int>(p, 4)) (void)x; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f3_credit_before(int *p [[ref_to_uninit]]) { for (int &x [[ref_to_uninit]] : std::span<int>(p, 4)) std::construct_at(&x, 1); }
void f3_destroy_before(int *p [[ref_to_uninit]]) { for (int &x : std::span<int>(p, 4)) std::destroy_at(&x); } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
int f3_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); int t = 0; for (int x : std::span<int>(p, 4)) t += x; return t; }
void f3_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); for (int &x : std::span<int>(p, 4)) x = 1; }
void f3_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); for (int &x : std::span<int>(p, 4)) (void)x; }
void f3_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); for (int &x [[ref_to_uninit]] : std::span<int>(p, 4)) std::construct_at(&x, 1); } // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void f3_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); for (int &x : std::span<int>(p, 4)) std::destroy_at(&x); }
// f4, range-for: for (... : c ? s1 : s2)
int f4_read_before(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { int t = 0; for (int x : c ? s1 : s2) t += x; return t; } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void f4_store_before(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { for (int &x : c ? s1 : s2) x = 1; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f4_bind_before(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { for (int &x : c ? s1 : s2) (void)x; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f4_credit_before(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { for (int &x [[ref_to_uninit]] : c ? s1 : s2) std::construct_at(&x, 1); }
void f4_destroy_before(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { for (int &x : c ? s1 : s2) std::destroy_at(&x); } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
int f4_read_after(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); int t = 0; for (int x : c ? s1 : s2) t += x; return t; }
void f4_store_after(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); for (int &x : c ? s1 : s2) x = 1; }
void f4_bind_after(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); for (int &x : c ? s1 : s2) (void)x; }
void f4_credit_after(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); for (int &x [[ref_to_uninit]] : c ? s1 : s2) std::construct_at(&x, 1); } // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void f4_destroy_after(bool c, std::span<int> s1 [[ref_to_uninit]], std::span<int> s2 [[ref_to_uninit]]) { std::uninitialized_fill(s1.begin(), s1.end(), 0); std::uninitialized_fill(s2.begin(), s2.end(), 0); for (int &x : c ? s1 : s2) std::destroy_at(&x); }
// f5, range-for: for (... : r)
int f5_read_before() { std::array<int, 4> r [[uninit]]; int t = 0; for (int x : r) t += x; return t; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f5_store_before() { std::array<int, 4> r [[uninit]]; for (int &x : r) x = 1; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f5_bind_before() { std::array<int, 4> r [[uninit]]; for (int &x : r) (void)x; } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f5_credit_before() { std::array<int, 4> r [[uninit]]; for (int &x [[ref_to_uninit]] : r) std::construct_at(&x, 1); } // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory}} expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void f5_destroy_before() { std::array<int, 4> r [[uninit]]; for (int &x : r) std::destroy_at(&x); } // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]'}}
int f5_read_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); int t = 0; for (int x : r) t += x; return t; }
void f5_store_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); for (int &x : r) x = 1; }
void f5_bind_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); for (int &x : r) (void)x; }
void f5_credit_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); for (int &x [[ref_to_uninit]] : r) std::construct_at(&x, 1); } // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
void f5_destroy_after() { std::array<int, 4> r [[uninit]]; std::uninitialized_fill(r.begin(), r.end(), 0); for (int &x : r) std::destroy_at(&x); }
// l1, by-reference lambda capture: a[1]
int l1_read_before() { int a [[uninit]] [4]; return [&] { return a[1]; }(); } // expected-error {{capturing 'a' by reference binds a reference to uninitialized memory}}
void l1_store_before() { int a [[uninit]] [4]; [&] { a[1] = 1; }(); } // expected-error {{capturing 'a' by reference binds a reference to uninitialized memory}}
void l1_bind_before() { int a [[uninit]] [4]; [&] { int *q_ = &(a[1]); (void)q_; }(); } // expected-error {{capturing 'a' by reference binds a reference to uninitialized memory}}
void l1_credit_before() { int a [[uninit]] [4]; [&] { std::construct_at(&(a[1]), 1); }(); } // expected-error {{capturing 'a' by reference binds a reference to uninitialized memory}}
void l1_destroy_before() { int a [[uninit]] [4]; [&] { std::destroy_at(&(a[1])); }(); } // expected-error {{capturing 'a' by reference binds a reference to uninitialized memory}}
int l1_read_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); return [&] { return a[1]; }(); }
void l1_store_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); [&] { a[1] = 1; }(); }
void l1_bind_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); [&] { int *q_ = &(a[1]); (void)q_; }(); }
void l1_credit_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); [&] { std::construct_at(&(a[1]), 1); }(); }
void l1_destroy_after() { int a [[uninit]] [4]; std::uninitialized_fill(a, a + 4, 0); [&] { std::destroy_at(&(a[1])); }(); }
// l2, by-reference lambda capture: g.x
int l2_read_before() { Agg g [[uninit]]; return [&] { return g.x; }(); } // expected-error {{capturing 'g' by reference binds a reference to uninitialized memory}}
void l2_store_before() { Agg g [[uninit]]; [&] { g.x = 1; }(); } // expected-error {{capturing 'g' by reference binds a reference to uninitialized memory}}
void l2_bind_before() { Agg g [[uninit]]; [&] { int *q_ = &(g.x); (void)q_; }(); } // expected-error {{capturing 'g' by reference binds a reference to uninitialized memory}}
void l2_credit_before() { Agg g [[uninit]]; [&] { std::construct_at(&(g.x), 1); }(); } // expected-error {{capturing 'g' by reference binds a reference to uninitialized memory}}
void l2_destroy_before() { Agg g [[uninit]]; [&] { std::destroy_at(&(g.x)); }(); } // expected-error {{capturing 'g' by reference binds a reference to uninitialized memory}}
int l2_read_after() { Agg g [[uninit]]; std::construct_at(&g); return [&] { return g.x; }(); }
void l2_store_after() { Agg g [[uninit]]; std::construct_at(&g); [&] { g.x = 1; }(); }
void l2_bind_after() { Agg g [[uninit]]; std::construct_at(&g); [&] { int *q_ = &(g.x); (void)q_; }(); }
void l2_credit_after() { Agg g [[uninit]]; std::construct_at(&g); [&] { std::construct_at(&(g.x), 1); }(); }
void l2_destroy_after() { Agg g [[uninit]]; std::construct_at(&g); [&] { std::destroy_at(&(g.x)); }(); }
// l3, by-reference lambda capture: *p
int l3_read_before(int *p [[ref_to_uninit]]) { return [&] { return *p; }(); }
void l3_store_before(int *p [[ref_to_uninit]]) { [&] { *p = 1; }(); }
void l3_bind_before(int *p [[ref_to_uninit]]) { [&] { int *q_ = &(*p); (void)q_; }(); }
void l3_credit_before(int *p [[ref_to_uninit]]) { [&] { std::construct_at(&(*p), 1); }(); }
void l3_destroy_before(int *p [[ref_to_uninit]]) { [&] { std::destroy_at(&(*p)); }(); }
int l3_read_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); return [&] { return *p; }(); }
void l3_store_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); [&] { *p = 1; }(); }
void l3_bind_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); [&] { int *q_ = &(*p); (void)q_; }(); }
void l3_credit_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); [&] { std::construct_at(&(*p), 1); }(); }
void l3_destroy_after(int *p [[ref_to_uninit]]) { std::uninitialized_fill(p, p + 4, 0); [&] { std::destroy_at(&(*p)); }(); }
// l4, by-reference lambda capture: *s.data()
int l4_read_before(std::span<int> s [[ref_to_uninit]]) { return [&] { return *s.data(); }(); } // expected-error {{capturing 's' by reference binds a reference to uninitialized memory}}
void l4_store_before(std::span<int> s [[ref_to_uninit]]) { [&] { *s.data() = 1; }(); } // expected-error {{capturing 's' by reference binds a reference to uninitialized memory}}
void l4_bind_before(std::span<int> s [[ref_to_uninit]]) { [&] { int *q_ = &(*s.data()); (void)q_; }(); } // expected-error {{capturing 's' by reference binds a reference to uninitialized memory}}
void l4_credit_before(std::span<int> s [[ref_to_uninit]]) { [&] { std::construct_at(&(*s.data()), 1); }(); } // expected-error {{capturing 's' by reference binds a reference to uninitialized memory}}
void l4_destroy_before(std::span<int> s [[ref_to_uninit]]) { [&] { std::destroy_at(&(*s.data())); }(); } // expected-error {{capturing 's' by reference binds a reference to uninitialized memory}}
int l4_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); return [&] { return *s.data(); }(); }
void l4_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); [&] { *s.data() = 1; }(); }
void l4_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); [&] { int *q_ = &(*s.data()); (void)q_; }(); }
void l4_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); [&] { std::construct_at(&(*s.data()), 1); }(); }
void l4_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); [&] { std::destroy_at(&(*s.data())); }(); }
// l5, by-reference lambda capture: *cr.data()
int l5_read_before(std::span<int> s [[ref_to_uninit]]) { const auto &cr = s; return [&] { return *cr.data(); }(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}} expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]'}} expected-error {{capturing 'cr' by reference binds a reference to uninitialized memory}}
void l5_store_before(std::span<int> s [[ref_to_uninit]]) { const auto &cr = s; [&] { *cr.data() = 1; }(); } // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]'}} expected-error {{capturing 'cr' by reference binds a reference to uninitialized memory}}
void l5_bind_before(std::span<int> s [[ref_to_uninit]]) { const auto &cr = s; [&] { int *q_ = &(*cr.data()); (void)q_; }(); } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}} expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]'}} expected-error {{capturing 'cr' by reference binds a reference to uninitialized memory}}
void l5_credit_before(std::span<int> s [[ref_to_uninit]]) { const auto &cr = s; [&] { std::construct_at(&(*cr.data()), 1); }(); } // expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]'}} expected-error {{capturing 'cr' by reference binds a reference to uninitialized memory}}
void l5_destroy_before(std::span<int> s [[ref_to_uninit]]) { const auto &cr = s; [&] { std::destroy_at(&(*cr.data())); }(); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}} expected-error {{handle of type 'std::span<int>' to uninitialized memory must be marked '[[ref_to_uninit]]'}} expected-error {{capturing 'cr' by reference binds a reference to uninitialized memory}}
int l5_read_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); const auto &cr = s; return [&] { return *cr.data(); }(); } // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory}}
void l5_store_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); const auto &cr = s; [&] { *cr.data() = 1; }(); }
void l5_bind_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); const auto &cr = s; [&] { int *q_ = &(*cr.data()); (void)q_; }(); } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
void l5_credit_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); const auto &cr = s; [&] { std::construct_at(&(*cr.data()), 1); }(); }
void l5_destroy_after(std::span<int> s [[ref_to_uninit]]) { std::uninitialized_fill(s.begin(), s.end(), 0); const auto &cr = s; [&] { std::destroy_at(&(*cr.data())); }(); } // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// k1, constructor of the current object: x
struct K1_read_before { int x [[uninit]]; int k = 0; K1_read_before() { k = x; } }; // expected-error {{member 'x' is read before initialization}} expected-note {{member 'x' declared here}}
struct K1_store_before { int x [[uninit]]; int k = 0; K1_store_before() { x = 1; } };
struct K1_bind_before { int x [[uninit]]; int k = 0; K1_bind_before() { int *q_ = &(x); (void)q_; } }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
struct K1_credit_before { int x [[uninit]]; int k = 0; K1_credit_before() { std::construct_at(&(x), 1); } };
struct K1_destroy_before { int x [[uninit]]; int k = 0; K1_destroy_before() { std::destroy_at(&(x)); } }; // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
struct K1_read_after { int x [[uninit]]; int k = 0; K1_read_after() { std::construct_at(&x, 0); k = x; } };
struct K1_store_after { int x [[uninit]]; int k = 0; K1_store_after() { std::construct_at(&x, 0); x = 1; } };
struct K1_bind_after { int x [[uninit]]; int k = 0; K1_bind_after() { std::construct_at(&x, 0); int *q_ = &(x); (void)q_; } };
struct K1_credit_after { int x [[uninit]]; int k = 0; K1_credit_after() { std::construct_at(&x, 0); std::construct_at(&(x), 1); } }; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
struct K1_destroy_after { int x [[uninit]]; int k = 0; K1_destroy_after() { std::construct_at(&x, 0); std::destroy_at(&(x)); } };
// k2, constructor of the current object: m.n
struct K2_read_before { Mid m; int k = 0; K2_read_before() { k = m.n; } }; // expected-error {{member 'n' is read before initialization}} expected-note@#mid-n {{member 'n' declared here}}
struct K2_store_before { Mid m; int k = 0; K2_store_before() { m.n = 1; } };
struct K2_bind_before { Mid m; int k = 0; K2_bind_before() { int *q_ = &(m.n); (void)q_; } }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
struct K2_credit_before { Mid m; int k = 0; K2_credit_before() { std::construct_at(&(m.n), 1); } };
struct K2_destroy_before { Mid m; int k = 0; K2_destroy_before() { std::destroy_at(&(m.n)); } }; // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
struct K2_read_after { Mid m; int k = 0; K2_read_after() { std::construct_at(&m); k = m.n; } };
struct K2_store_after { Mid m; int k = 0; K2_store_after() { std::construct_at(&m); m.n = 1; } };
struct K2_bind_after { Mid m; int k = 0; K2_bind_after() { std::construct_at(&m); int *q_ = &(m.n); (void)q_; } };
struct K2_credit_after { Mid m; int k = 0; K2_credit_after() { std::construct_at(&m); std::construct_at(&(m.n), 1); } }; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
struct K2_destroy_after { Mid m; int k = 0; K2_destroy_after() { std::construct_at(&m); std::destroy_at(&(m.n)); } };
// k3, constructor of the current object: w.x
struct K3_read_before { Agg w [[uninit]]; int k = 0; K3_read_before() { k = w.x; } }; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory}}
struct K3_store_before { Agg w [[uninit]]; int k = 0; K3_store_before() { w.x = 1; } }; // expected-error {{writing a member of an '[[uninit]]' object does not initialize it}}
struct K3_bind_before { Agg w [[uninit]]; int k = 0; K3_bind_before() { int *q_ = &(w.x); (void)q_; } }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
struct K3_credit_before { Agg w [[uninit]]; int k = 0; K3_credit_before() { std::construct_at(&(w.x), 1); } };
struct K3_destroy_before { Agg w [[uninit]]; int k = 0; K3_destroy_before() { std::destroy_at(&(w.x)); } }; // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
struct K3_read_after { Agg w [[uninit]]; int k = 0; K3_read_after() { std::construct_at(&w); k = w.x; } };
struct K3_store_after { Agg w [[uninit]]; int k = 0; K3_store_after() { std::construct_at(&w); w.x = 1; } };
struct K3_bind_after { Agg w [[uninit]]; int k = 0; K3_bind_after() { std::construct_at(&w); int *q_ = &(w.x); (void)q_; } };
struct K3_credit_after { Agg w [[uninit]]; int k = 0; K3_credit_after() { std::construct_at(&w); std::construct_at(&(w.x), 1); } }; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory}}
struct K3_destroy_after { Agg w [[uninit]]; int k = 0; K3_destroy_after() { std::construct_at(&w); std::destroy_at(&(w.x)); } };
// k4, constructor of the current object: (this->*pm).d
struct K4_read_before { Der2 m; int k = 0; K4_read_before(Der2 K4_read_before::*pm) { k = (this->*pm).d; } }; // expected-error {{member 'd' is read before initialization}} expected-error {{member 'b' is read before initialization}} expected-note@#der2-d {{member 'd' declared here}} expected-note@#base2-b {{member 'b' declared here}}
struct K4_store_before { Der2 m; int k = 0; K4_store_before(Der2 K4_store_before::*pm) { (this->*pm).d = 1; } };
struct K4_bind_before { Der2 m; int k = 0; K4_bind_before(Der2 K4_bind_before::*pm) { int *q_ = &((this->*pm).d); (void)q_; } }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
struct K4_credit_before { Der2 m; int k = 0; K4_credit_before(Der2 K4_credit_before::*pm) { std::construct_at(&((this->*pm).d), 1); } };
struct K4_destroy_before { Der2 m; int k = 0; K4_destroy_before(Der2 K4_destroy_before::*pm) { std::destroy_at(&((this->*pm).d)); } }; // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
struct K4_read_after { Der2 m; int k = 0; K4_read_after(Der2 K4_read_after::*pm) { std::construct_at(&m); k = (this->*pm).d; } };
struct K4_store_after { Der2 m; int k = 0; K4_store_after(Der2 K4_store_after::*pm) { std::construct_at(&m); (this->*pm).d = 1; } };
struct K4_bind_after { Der2 m; int k = 0; K4_bind_after(Der2 K4_bind_after::*pm) { std::construct_at(&m); int *q_ = &((this->*pm).d); (void)q_; } }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
struct K4_credit_after { Der2 m; int k = 0; K4_credit_after(Der2 K4_credit_after::*pm) { std::construct_at(&m); std::construct_at(&((this->*pm).d), 1); } };
struct K4_destroy_after { Der2 m; int k = 0; K4_destroy_after(Der2 K4_destroy_after::*pm) { std::construct_at(&m); std::destroy_at(&((this->*pm).d)); } }; // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
// k5, constructor of the current object: static_cast<Base2 &>(this->*pm).b
struct K5_read_before { Der2 m; int k = 0; K5_read_before(Der2 K5_read_before::*pm) { k = static_cast<Base2 &>(this->*pm).b; } }; // expected-error {{member 'd' is read before initialization}} expected-error {{member 'b' is read before initialization}} expected-note@#der2-d {{member 'd' declared here}} expected-note@#base2-b {{member 'b' declared here}}
struct K5_store_before { Der2 m; int k = 0; K5_store_before(Der2 K5_store_before::*pm) { static_cast<Base2 &>(this->*pm).b = 1; } };
struct K5_bind_before { Der2 m; int k = 0; K5_bind_before(Der2 K5_bind_before::*pm) { int *q_ = &(static_cast<Base2 &>(this->*pm).b); (void)q_; } }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
struct K5_credit_before { Der2 m; int k = 0; K5_credit_before(Der2 K5_credit_before::*pm) { std::construct_at(&(static_cast<Base2 &>(this->*pm).b), 1); } };
struct K5_destroy_before { Der2 m; int k = 0; K5_destroy_before(Der2 K5_destroy_before::*pm) { std::destroy_at(&(static_cast<Base2 &>(this->*pm).b)); } }; // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
struct K5_read_after { Der2 m; int k = 0; K5_read_after(Der2 K5_read_after::*pm) { std::construct_at(&m); k = static_cast<Base2 &>(this->*pm).b; } };
struct K5_store_after { Der2 m; int k = 0; K5_store_after(Der2 K5_store_after::*pm) { std::construct_at(&m); static_cast<Base2 &>(this->*pm).b = 1; } };
struct K5_bind_after { Der2 m; int k = 0; K5_bind_after(Der2 K5_bind_after::*pm) { std::construct_at(&m); int *q_ = &(static_cast<Base2 &>(this->*pm).b); (void)q_; } }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]'}}
struct K5_credit_after { Der2 m; int k = 0; K5_credit_after(Der2 K5_credit_after::*pm) { std::construct_at(&m); std::construct_at(&(static_cast<Base2 &>(this->*pm).b), 1); } };
struct K5_destroy_after { Der2 m; int k = 0; K5_destroy_after(Der2 K5_destroy_after::*pm) { std::construct_at(&m); std::destroy_at(&(static_cast<Base2 &>(this->*pm).b)); } }; // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function}}
