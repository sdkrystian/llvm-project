// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/local.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/referent.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -isystem %t/sys -std=c++23 %t/system.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected,strict -fprofiles -fprofiles-verify-deferred-checks -fno-profiles-exempt-system-headers -isystem %t/sys -std=c++23 %t/system.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/now_init_body.cpp
// RUN: %clang_cc1 -triple x86_64-linux-gnu -fsyntax-only -verify=expected,array -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/va.cpp
// RUN: %clang_cc1 -triple aarch64-linux-gnu -fsyntax-only -verify=expected,record -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/va.cpp
// RUN: %clang_cc1 -triple i386-linux-gnu -fsyntax-only -verify=expected,pointer -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/va.cpp

// Clang knows the std::init roles of std::construct_at (its pointer first
// parameter must refer to uninitialized memory and is initialized by the
// call) and std::destroy_at (the call destroys it), std::now_init's pointer
// parameter (uninitialized, keeps its state), the std::uninitialized_*
// output range (by name and arity), and std::destroy's and
// std::destroy_n's first iterator, and looks them up at each call, so the
// real library functions are usable under enforcement.

//--- local.cpp
[[profiles::enforce(std::init)]];

namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
// A non-pointer first parameter is outside the form key: not annotated.
template <class T> void construct_at(T &r, int);
template <class T> T *now_init(T *p);
// Outside now_init's form key: a second parameter, a reference parameter.
template <class T> T *now_init(T *p, int);
template <class T> T *now_init(T &r);
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);
template <class It, class Size, class T> It uninitialized_fill_n(It first, Size n, const T &x);
template <class In, class Out> Out uninitialized_copy(In first, In last, Out result);
template <class In, class Size, class Out> Out uninitialized_copy_n(In first, Size n, Out result);
template <class In, class Out> Out uninitialized_move(In first, In last, Out result);
template <class In, class Size, class Out> void uninitialized_move_n(In first, Size n, Out result);
template <class It> void uninitialized_default_construct(It first, It last);
template <class It, class Size> It uninitialized_default_construct_n(It first, Size n);
template <class It> void uninitialized_value_construct(It first, It last);
template <class It, class Size> It uninitialized_value_construct_n(It first, Size n);
template <class P, class It, class T> void uninitialized_fill(P &&policy, It first, It last, const T &x);
template <class It> void destroy(It first, It last);
template <class It, class Size> It destroy_n(It first, Size n);
template <class P, class It> void destroy(P &&policy, It first, It last);
template <class P, class It, class Size> It destroy_n(P &&policy, It first, Size n);
template <class T, __SIZE_TYPE__ N> T *begin(T (&a)[N]);
template <class T, __SIZE_TYPE__ N> T *end(T (&a)[N]);
template <class T> struct allocator {
  T *allocate(__SIZE_TYPE__ n);
  void deallocate(T *p, __SIZE_TYPE__ n);
};
template <class A> struct allocator_traits {
  template <class U, class... Args>
  static void construct(A &a, U *p, Args &&...args);
  template <class U> static void destroy(A &a, U *p);
};
template <class T> struct allocator_traits<allocator<T>> {
  static T *allocate(allocator<T> &a, __SIZE_TYPE__ n);
  static void deallocate(allocator<T> &a, T *p, __SIZE_TYPE__ n);
  template <class U, class... Args>
  static void construct(allocator<T> &a, U *p, Args &&...args);
  template <class U> static void destroy(allocator<T> &a, U *p);
};
template <class T> T *addressof(T &r) noexcept;
namespace ranges {
inline constexpr struct {
  template <class R, class T> void operator()(R &&r, const T &x) const;
} uninitialized_fill;
} // namespace ranges
} // namespace std

// A global-namespace construct_at or now_init is not annotated either.
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> T *now_init(T *p);
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);

// The paper's central lifecycle idiom compiles clean.
void lifecycle() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  int v = u; // OK: the callee initialized u
  std::destroy_at(&u);
  (void)v;
}

// std::addressof hands the lifecycle calls the object itself
void lifecycle_addressof() {
  int u [[uninit]];
  std::construct_at(std::addressof(u), 5);
  int v = u; // OK
  std::destroy_at(std::addressof(u));
  std::destroy_at(std::addressof(u)); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
  (void)v;
}

// construct_at initializes u whole: a second construct_at is rejected by
// the reverse-direction rule.
void double_construct() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  std::construct_at(&u, 6); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// destroy_at before any store destroys uninitialized storage.
void destroy_uninitialized() {
  int u [[uninit]];
  std::destroy_at(&u); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

// A second destroy_at is a double destruction.
void double_destroy() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  std::destroy_at(&u);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// A destroy_at after a destroy_at on one path is a double destruction
void double_destroy_one_path(bool c) {
  int u [[uninit]];
  std::construct_at(&u, 5);
  if (c)
    std::destroy_at(&u);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// A destroy_at after a destroy_at and a reconstruction on one path is accepted
void destroy_reconstructed_one_path(bool c) {
  int u [[uninit]];
  std::construct_at(&u, 5);
  if (c) {
    std::destroy_at(&u);
    std::construct_at(&u, 6);
  }
  std::destroy_at(&u); // OK
}

// A destroy_at after a conditional destroy_at that may name the storage is a
// double destruction
void double_destroy_after_conditional(bool c) {
  int a [[uninit]], b [[uninit]];
  std::construct_at(&a, 1);
  std::construct_at(&b, 2);
  std::destroy_at(c ? &a : &b);
  std::destroy_at(&a); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// A conditional destroy_at keeps an earlier destroy on every path
void double_destroy_across_conditional(bool c) {
  int a [[uninit]], b [[uninit]];
  std::construct_at(&a, 1);
  std::construct_at(&b, 2);
  std::destroy_at(&a);
  std::destroy_at(c ? &a : &b); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  std::destroy_at(&a); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// Binding the destroyed storage is the unmarked-direction violation.
void destroy_then_bind() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  std::destroy_at(&u);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q;
}

// The global-namespace overload earns nothing: its first parameter is
// unmarked.
void global_ns_not_annotated() {
  int u [[uninit]];
  construct_at(&u, 5); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// The non-pointer-first-parameter std overload is likewise not annotated.
void non_pointer_p0_not_annotated() {
  int u [[uninit]];
  std::construct_at(u, 5); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Calling through an explicit specialization initializes the same storage.
void explicit_specialization() {
  int u [[uninit]];
  std::construct_at<int>(&u, 5);
  int v = u; // OK
  (void)v;
}

struct Payload { int y; };
void sink(int *);

// now_init launders its argument through the return (P4222R2 §4.4): the
// returned pointer is trusted, the original name keeps its state (§6.1).
void now_init_return_only() {
  int u [[uninit]];
  int v = *std::now_init(&u); // OK: the unmarked return is trusted
  int *s = std::now_init(&u); // OK
  sink(&u); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::construct_at(&u, 5); // OK: u still refers to uninitialized memory
  (void)v; (void)s;
}

// A pointer to initialized storage cannot be passed to now_init (§4.2).
void now_init_initialized_source() {
  int x = 1;
  std::now_init(&x); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// The trusted return cannot initialize a marked pointer.
void now_init_marked_target() {
  int u [[uninit]];
  int *q [[ref_to_uninit]] = std::now_init(&u); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)q;
}

// Through a marked pointer the referent keeps its state.
void now_init_through_marked_pointer() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  int *s = std::now_init(p); // OK
  int r = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)s; (void)r;
}

// A decayed [[uninit]] array is a marked source (§6.1's now_init(arr10)).
void now_init_array() {
  Payload arr [[uninit]] [4];
  Payload *a = std::now_init(arr); // OK
  int z = a[2].y; // OK
  (void)z;
}

// The §4.6 slot idiom: arithmetic on a marked member pointer is
// unclassified, and the returned referent is trusted.
struct Slots { Payload *elem [[ref_to_uninit]]; int n; };
Payload &slot_at(Slots &s, int i) {
  return *std::now_init(s.elem + i); // OK
}

// Declarations outside the form key are not annotated.
void now_init_form_key() {
  int u [[uninit]];
  std::now_init(&u, 0); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  std::now_init(u); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  ::now_init(&u); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// The paper's §4.7 pair: an initialized range is rejected, an uninitialized one is initialized
void fill_pair() {
  int a1[] = {1, 2, 3};
  std::uninitialized_fill(a1, a1 + 3, 10); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int a2 [[uninit]] [3];
  std::uninitialized_fill(a2, a2 + 3, 10);
  int y = a2[0]; // OK
  (void)y;
}

// A read before the fill reads uninitialized memory
void read_before_fill() {
  int a2 [[uninit]] [3];
  int x = a2[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  std::uninitialized_fill(a2, a2 + 3, 10);
  (void)x;
}

// A second fill is a double initialization
void double_fill() {
  int a [[uninit]] [3];
  std::uninitialized_fill_n(a, 3, 1);
  std::uninitialized_fill_n(a, 3, 2); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// uninitialized_copy reads its source range
void copy_source() {
  int s [[uninit]] [3];
  int d [[uninit]] [3];
  std::uninitialized_copy(s, s + 3, d); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p [[ref_to_uninit]] = s;
  int e [[uninit]] [3];
  std::uninitialized_copy(p, p + 3, e); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// The output range of uninitialized_copy_n, move, and move_n is the third parameter
void copy_move_output() {
  int src[3] = {1, 2, 3};
  int d1 [[uninit]] [3], d2 [[uninit]] [3], d3 [[uninit]] [3], d4 [[uninit]] [3];
  std::uninitialized_copy(src, src + 3, d1);
  std::uninitialized_copy_n(src, 3, d2);
  std::uninitialized_move(src, src + 3, d3);
  std::uninitialized_move_n(src, 3, d4);
  int y = d1[0] + d2[0] + d3[0] + d4[0]; // OK
  (void)y;
}

// value_construct and its _n form initialize the range
void value_construct() {
  int a [[uninit]] [3], b [[uninit]] [3];
  std::uninitialized_value_construct(a, a + 3);
  std::uninitialized_value_construct_n(b, 3);
  int y = a[0] + b[0]; // OK
  (void)y;
}

// default_construct of int takes an uninitialized range and credits nothing
void default_construct() {
  int a [[uninit]] [3], b [[uninit]] [3];
  std::uninitialized_default_construct(a, a + 3);
  std::uninitialized_default_construct_n(b, 3);
  int y = a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  int z = b[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  int c[3] = {};
  std::uninitialized_default_construct(c, c + 3); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)y; (void)z;
}

// default_construct of a trivial class credits nothing
struct Trivial { int v; };
int default_construct_trivial(Trivial *p [[ref_to_uninit]]) {
  std::uninitialized_default_construct_n(p, 1);
  return p->v; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// default_construct initializes a range whose default-initialization leaves nothing indeterminate
struct UserCtor { UserCtor(); int v; };
struct AllNSDMI { int v = 0; };
int default_construct_determinate(UserCtor *p [[ref_to_uninit]],
                                  AllNSDMI *q [[ref_to_uninit]]) {
  std::uninitialized_default_construct(p, p + 1);
  std::uninitialized_default_construct_n(q, 1);
  std::uninitialized_default_construct_n(q, 1); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  return p->v + q->v; // OK
}

// A class-type iterator is outside the marker: the call is inert
struct Iter {
  int *p [[ref_to_uninit]];
  bool operator!=(Iter) const;
  Iter &operator++();
  int &operator*() const;
};
void class_iterator() {
  int arr [[uninit]] [4];
  Iter b{arr}, e{arr + 4};
  std::uninitialized_fill(b, e, 0); // OK
  int x = arr[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)x;
}

// The §6.1 arr9 shape
struct X { int v; };
void arr9_shape(const X &xval) {
  X arr9 [[uninit]] [20];
  std::uninitialized_fill(arr9, arr9 + 20, xval);
  int v = arr9[3].v; // OK
  (void)v;
}

// The parallel-policy overload's range is its second parameter
struct Policy {};
void policy_overload() {
  int a [[uninit]] [3];
  std::uninitialized_fill(Policy{}, a, a + 3, 1);
  int y = a[0]; // OK
  int b[3] = {};
  std::uninitialized_fill(Policy{}, b, b + 3, 1); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)y;
}

// A ranges CPO object is not annotated
void ranges_cpo() {
  int a2 [[uninit]] [3];
  std::ranges::uninitialized_fill(a2, 10); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// std::begin on an [[uninit]] array is rejected on both sides
void begin_end() {
  int a [[uninit]] [3];
  std::uninitialized_fill(std::begin(a), std::end(a), 0); // expected-error 2 {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}} \
                                                          // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A global-namespace uninitialized_fill is not annotated
void global_ns_fill() {
  int a [[uninit]] [3];
  ::uninitialized_fill(a, a + 3, 0); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A range from std::allocator is uninitialized memory
void allocator_range(std::allocator<int> &alloc) {
  int *b [[ref_to_uninit]] = alloc.allocate(3);
  std::uninitialized_fill_n(b, 3, 0);
  int x = *b; // OK
  std::destroy_n(b, 3);
  alloc.deallocate(b, 3);
  (void)x;
}

// An unmarked pointer cannot hold std::allocator's storage
void allocator_unmarked(std::allocator<int> &alloc) {
  int *b = alloc.allocate(3); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)b;
}

// allocator_traits allocates and deallocates the same way
void allocator_traits_range(std::allocator<int> &alloc) {
  using Traits = std::allocator_traits<std::allocator<int>>;
  int *b [[ref_to_uninit]] = Traits::allocate(alloc, 3);
  int *c = Traits::allocate(alloc, 3); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  Traits::deallocate(alloc, b, 3);
  (void)c;
}

// A read after deallocate reads released storage
int allocator_read_after_deallocate(std::allocator<int> &alloc) {
  int *b [[ref_to_uninit]] = alloc.allocate(1);
  *b = 1;
  alloc.deallocate(b, 1);
  return *b; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// destroy ends the lifetime of the range
void destroy_range() {
  int arr [[uninit]] [4];
  std::uninitialized_fill(arr, arr + 4, 0);
  std::destroy(arr, arr + 4);
  int x = arr[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  std::uninitialized_fill(arr, arr + 4, 1); // OK: uninitialized again
  (void)x;
}

// destroy of never-constructed storage
void destroy_never_constructed() {
  int arr [[uninit]] [4];
  std::destroy(arr, arr + 4); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

// A second destroy is a double destruction
void destroy_twice() {
  int arr [[uninit]] [4];
  std::uninitialized_fill(arr, arr + 4, 0);
  std::destroy(arr, arr + 4);
  std::destroy(arr, arr + 4); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// destroy_n through a marked pointer
void destroy_n_marked_pointer() {
  int arr [[uninit]] [4];
  int *p [[ref_to_uninit]] = arr;
  std::uninitialized_fill_n(p, 4, 0);
  int y = *p; // OK
  std::destroy_n(p, 4);
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x; (void)y;
}

// The end iterator's arithmetic spelling is unclassified
void destroy_end_iterator() {
  int arr [[uninit]] [4];
  std::uninitialized_fill(arr, arr + 4, 0);
  std::destroy(arr, arr + 4); // OK
}

// Only the first iterator's storage is destroyed
void destroy_first_iterator_only() {
  int a [[uninit]] [4], b [[uninit]] [4];
  std::uninitialized_fill(a, a + 4, 0);
  std::uninitialized_fill(b, b + 4, 0);
  std::destroy(a, b);
  int x = b[0]; // OK
  int y = a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)x; (void)y;
}

// The parallel-policy overloads of destroy and destroy_n destroy their first iterator
void destroy_policy_overload() {
  int a [[uninit]] [4];
  std::destroy(Policy{}, a, a + 4); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  std::uninitialized_fill(a, a + 4, 0);
  std::destroy_n(Policy{}, a, 4);
  int y = a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}

// A marked end iterator is accepted by uninitialized_fill
void fill_marked_end(int *b [[ref_to_uninit]], int *e [[ref_to_uninit]]) {
  std::uninitialized_fill(b, e, 0);
}

// A marked end iterator is accepted by uninitialized_value_construct
void value_construct_marked_end(int *b [[ref_to_uninit]],
                                int *e [[ref_to_uninit]]) {
  std::uninitialized_value_construct(b, e);
}

// A marked end iterator is accepted by destroy
void destroy_marked_end(int *b [[ref_to_uninit]], int *e [[ref_to_uninit]]) {
  std::uninitialized_fill(b, e, 0);
  std::destroy(b, e);
}

// A marked end iterator is accepted by the policy overload
void fill_policy_marked_end(int *b [[ref_to_uninit]],
                            int *e [[ref_to_uninit]]) {
  std::uninitialized_fill(Policy{}, b, e, 0);
}

// Marked member iterators are accepted
struct MarkedRange {
  int *b [[ref_to_uninit]];
  int *e [[ref_to_uninit]];
  void fill() { std::uninitialized_fill(b, e, 0); }
};

// A marked local end iterator is accepted
void fill_marked_local_end(int *b [[ref_to_uninit]], int n) {
  int *e [[ref_to_uninit]] = b + n;
  std::uninitialized_fill(b, e, 0);
}

// Only the first iterator's storage is initialized
void fill_first_iterator_only() {
  int a [[uninit]] [4], b [[uninit]] [4];
  std::uninitialized_fill(a, b, 0);
  int x = a[0]; // OK
  int y = b[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)x; (void)y;
}

struct TraitsPair { int x; int y; };
using PairTraits = std::allocator_traits<std::allocator<TraitsPair>>;

// allocator_traits::construct initializes and ::destroy destroys like construct_at and destroy_at
int traits_construct_destroy(std::allocator<TraitsPair> &a) {
  TraitsPair *p [[ref_to_uninit]] = PairTraits::allocate(a, 1);
  PairTraits::construct(a, p, 1, 2);
  int r = p->x; // OK
  PairTraits::destroy(a, p);
  PairTraits::deallocate(a, p, 1);
  return r;
}

// A second allocator_traits::construct is a double construction
void traits_double_construct(std::allocator<TraitsPair> &a) {
  TraitsPair *p [[ref_to_uninit]] = PairTraits::allocate(a, 1);
  PairTraits::construct(a, p, 1, 2);
  PairTraits::construct(a, p, 3, 4); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// allocator_traits::destroy of never-constructed storage destroys uninitialized storage
void traits_destroy_uninitialized(std::allocator<TraitsPair> &a) {
  TraitsPair *p [[ref_to_uninit]] = PairTraits::allocate(a, 1);
  PairTraits::destroy(a, p); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

struct OwnAlloc {
  template <class U> void construct(U *p);
  template <class U, class... Args> void construct(U *p, Args &&...args);
  template <class U> void destroy(U *p);
};
struct DerivedOwnAlloc : OwnAlloc {};
struct Determinate { int x = 0; };
using OwnTraits = std::allocator_traits<OwnAlloc>;

// An allocator's own construct with no arguments default-initializes
int traits_own_construct_default(OwnAlloc &a, int *p [[ref_to_uninit]]) {
  OwnTraits::construct(a, p);
  return *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// An allocator's own construct inherited from a base default-initializes
int traits_base_construct_default(DerivedOwnAlloc &a, int *p [[ref_to_uninit]]) {
  std::allocator_traits<DerivedOwnAlloc>::construct(a, p);
  return *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// An allocator's own construct with no arguments initializes a type default-initialization initializes
int traits_own_construct_determinate(OwnAlloc &a, Determinate *p [[ref_to_uninit]]) {
  std::allocator_traits<OwnAlloc>::construct(a, p);
  return p->x; // OK
}

// An allocator's own construct with arguments initializes
int traits_own_construct_arguments(OwnAlloc &a, int *p [[ref_to_uninit]]) {
  OwnTraits::construct(a, p, 1);
  return *p; // OK
}

// An allocator's own destroy is not a destroy
void traits_own_destroy(OwnAlloc &a, int *p [[ref_to_uninit]]) {
  OwnTraits::construct(a, p, 1);
  OwnTraits::destroy(a, p);
  OwnTraits::destroy(a, p); // OK
}

// An allocator's own destroy leaves the storage possibly initialized: a read
// is rejected, a reconstruction is accepted
int traits_own_destroy_reconstruct(OwnAlloc &a, int *p [[ref_to_uninit]]) {
  OwnTraits::construct(a, p, 1);
  OwnTraits::destroy(a, p);
  int r = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  OwnTraits::construct(a, p, 2);
  return r + *p; // OK
}

struct ScopedDtor { int x; ~ScopedDtor(); };
struct ScopedTrivial { int x; };

// The scope-exit destructor of a never-constructed [[uninit]] local destroys uninitialized storage
void scope_exit_never_constructed() {
  ScopedDtor w [[uninit]]; // expected-note {{'w' is destroyed at the end of its scope}}
} // expected-error {{uninitialized storage is destroyed by its scope-exit destructor under profile 'std::init'}}

// The scope-exit destructor after destroy_at destroys the local again
void scope_exit_after_destroy_at() {
  ScopedDtor w [[uninit]]; // expected-note {{'w' is destroyed at the end of its scope}}
  std::construct_at(&w);
  std::destroy_at(&w);
} // expected-error {{storage already destroyed by its scope-exit destructor is destroyed again under profile 'std::init'}}

// A construct_at on one path is enough for the scope-exit destructor
void scope_exit_one_path(bool c) {
  ScopedDtor w [[uninit]];
  if (c)
    std::construct_at(&w);
}

// The scope-exit destructor of a never-filled [[uninit]] array destroys uninitialized storage
void scope_exit_array() {
  [[uninit]] ScopedDtor arr[2]; // expected-note {{'arr' is destroyed at the end of its scope}}
} // expected-error {{uninitialized storage is destroyed by its scope-exit destructor under profile 'std::init'}}

// The scope-exit destructor after construction is accepted
void scope_exit_constructed() {
  ScopedDtor w [[uninit]];
  std::construct_at(&w);
}

// An early return runs the scope-exit destructor there
void scope_exit_early_return(bool c) {
  ScopedDtor w [[uninit]]; // expected-note {{'w' is destroyed at the end of its scope}}
  if (c)
    return; // expected-error {{uninitialized storage is destroyed by its scope-exit destructor under profile 'std::init'}}
  std::construct_at(&w);
}

// A trivially destructible [[uninit]] local is not judged at scope exit
void scope_exit_trivial() {
  ScopedTrivial w [[uninit]];
}

// The scope-exit destructor after a destroy_at on one path destroys the local again
void scope_exit_after_one_path_destroy(bool c) {
  ScopedDtor w [[uninit]]; // expected-note {{'w' is destroyed at the end of its scope}}
  std::construct_at(&w);
  if (c)
    std::destroy_at(&w);
} // expected-error {{storage already destroyed by its scope-exit destructor is destroyed again under profile 'std::init'}}

// The scope-exit destructor of an array constructed element by element is accepted
void scope_exit_array_elements() {
  [[uninit]] ScopedDtor arr[2];
  for (int i = 0; i != 2; ++i)
    std::construct_at(&arr[i]);
}

// Element-wise construction after a whole destroy leaves the array destroyed,
// so its scope-exit destructor destroys it again
void scope_exit_array_elements_after_destroy() {
  [[uninit]] ScopedDtor arr[2]; // expected-note {{'arr' is destroyed at the end of its scope}}
  std::uninitialized_value_construct(arr, arr + 2);
  std::destroy(arr, arr + 2);
  for (int i = 0; i != 2; ++i)
    std::construct_at(&arr[i]);
} // expected-error {{storage already destroyed by its scope-exit destructor is destroyed again under profile 'std::init'}}

// A whole-range construction after a whole destroy is accepted at scope exit
void scope_exit_array_reconstructed_after_destroy() {
  [[uninit]] ScopedDtor arr[2];
  std::uninitialized_value_construct(arr, arr + 2);
  std::destroy(arr, arr + 2);
  std::uninitialized_value_construct(arr, arr + 2);
}

// A default constructor defaulted out of line runs no code, so the scope exit is judged
struct ScopedOutOfLine {
  int x [[uninit]];
  ScopedOutOfLine();
  ~ScopedOutOfLine();
};
ScopedOutOfLine::ScopedOutOfLine() = default;
void scope_exit_out_of_line_default() {
  ScopedOutOfLine w [[uninit]]; // expected-note {{'w' is destroyed at the end of its scope}}
} // expected-error {{uninitialized storage is destroyed by its scope-exit destructor under profile 'std::init'}}
void scope_exit_out_of_line_default_destroyed() {
  ScopedOutOfLine w [[uninit]]; // expected-note {{'w' is destroyed at the end of its scope}}
  std::construct_at(&w);
  std::destroy_at(&w);
} // expected-error {{storage already destroyed by its scope-exit destructor is destroyed again under profile 'std::init'}}

// A marker rejected for its initializer is not judged at scope exit
void scope_exit_marker_with_initializer() {
  ScopedDtor w [[uninit]] = ScopedDtor{1}; // expected-error {{variable 'w' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
}

// A suppression on the declaration covers its scope-exit destructor
void scope_exit_suppressed() {
  [[profiles::suppress(std::init, rule: "destroy_uninit")]] ScopedDtor w [[uninit]];
}

// A declaration that runs a constructor is not judged at scope exit
struct ScopedCtor { ScopedCtor(); ~ScopedCtor(); };
void scope_exit_rejected_marker() {
  ScopedCtor w [[uninit]]; // expected-error {{variable 'w' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'ScopedCtor' does not leave it uninitialized}} \
                           // expected-note {{default-initialization of 'ScopedCtor' runs a constructor}}
}

//--- referent.cpp
// A marked local pointer or reference refers to the storage its latest
// binding names (ProfilesFramework.rst, "Binding Pointers and References"),
// so the lifecycle calls act on that storage under either name.
[[profiles::enforce(std::init)]];

namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
} // namespace std

struct Payload { int y; };

// construct_at through a marked pointer initializes the local it refers to.
void construct_through_pointer() {
  Payload x [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  std::construct_at(p, 5);
  int z = x.y; // OK: p refers to x
  (void)z;
}

// The same through a marked reference.
void construct_through_reference() {
  Payload x [[uninit]];
  Payload &r [[ref_to_uninit]] = x;
  std::construct_at(&r, 5);
  int z = x.y; // OK: r refers to x
  (void)z;
}

// construct_at on the local initializes what a marked pointer to it reads.
void construct_local_read_through_pointer() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  std::construct_at(&u, 5);
  int v = *p; // OK: u is initialized
  (void)v;
}

// A second construct_at under the other name is a double construction.
void double_construct_across_names() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  std::construct_at(p, 5);
  std::construct_at(&u, 6); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A second destroy_at under the other name is a double destruction.
void double_destroy_across_names() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  std::construct_at(&u, 5);
  std::destroy_at(p);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// A destroy_at through the pointer after one on one path is a double destruction
void double_destroy_one_path(bool c, int *p [[ref_to_uninit]]) {
  std::construct_at(p, 5);
  if (c)
    std::destroy_at(p);
  std::destroy_at(p); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// destroy_at through the pointer destroys the local: never-constructed
// storage is rejected, and a subobject read after a destroy reads destroyed
// storage.
void destroy_through_pointer() {
  Payload x [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  std::destroy_at(p); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  std::construct_at(&x, 1);
  std::destroy_at(p);
  int z = x.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)z;
}

// A marked pointer copied from another refers to the same storage.
void pointer_chain() {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = &u;
  int *q [[ref_to_uninit]] = p;
  std::construct_at(q, 5);
  int v = *p;                    // OK: q and p refer to u
  int *r [[ref_to_uninit]] = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)v; (void)r;
}

// Reseating the pointer to a second local credits that local from then on.
void reseat_to_second_local() {
  Payload x [[uninit]], y [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  std::construct_at(p, 1);
  p = &y;
  std::construct_at(p, 2);
  int a = x.y; // OK
  int b = y.y; // OK
  (void)a; (void)b;
}

// A conditional initializer with tracked arms leaves the referent
// unidentified: construct_at through the pointer credits neither local, and
// nothing fires through the pointer.
void conditional_initializer(bool c) {
  int u [[uninit]], v [[uninit]];
  int *p [[ref_to_uninit]] = c ? &u : &v;
  std::construct_at(p, 5);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int r = *p;  // OK: unknown referent
  (void)q; (void)r;
}

// A conditional initializer whose arms name one local refers to it
void conditional_initializer_one_local(bool c) {
  int u [[uninit]];
  int *p [[ref_to_uninit]] = c ? &u : &u;
  std::construct_at(p, 5);
  int *q = &u; // OK
  int r = *p;  // OK
  (void)q; (void)r;
}

// A pointer to a subobject has an anonymous referent: construct_at through
// it does not initialize the enclosing object.
void subobject_pointer() {
  Payload x [[uninit]];
  int *p [[ref_to_uninit]] = &x.y;
  std::construct_at(p, 5);
  int z = x.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)z;
}

// The wording follows the spelling of the read, not the storage it reaches.
void wording_by_spelling() {
  Payload x [[uninit]];
  Payload *p [[ref_to_uninit]] = &x;
  int a = p->y; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int b = x.y;  // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)a; (void)b;
}

// A marked binding asserts the local it names uninitialized, whatever a
// conditional construct_at left it.
void construct_conditional_then_bind(bool c) {
  Payload x [[uninit]];
  if (c)
    std::construct_at(&x, 1);
  Payload *p [[ref_to_uninit]] = &x; // OK: constructed on one path only
  int z = x.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  std::construct_at(p, 2);
  int z2 = x.y; // OK
  (void)z; (void)z2;
}

// A subobject read after a construct_at on one path only is rejected
int construct_one_path_subobject_read(bool c) {
  Payload x [[uninit]];
  if (c)
    std::construct_at(&x, 1);
  return x.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A subobject read after a construct_at on every path is accepted
int construct_every_path_subobject_read(bool c) {
  Payload x [[uninit]];
  if (c)
    std::construct_at(&x, 1);
  else
    std::construct_at(&x, 2);
  return x.y; // OK
}

// A read through a marker after a construct_at on one path only is rejected
int construct_one_path_read_through(int *p [[ref_to_uninit]], bool c) {
  if (c)
    std::construct_at(p, 1);
  return *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A read through a marker after a destroy_at on one path only is rejected
int destroy_one_path_read_through(int *p [[ref_to_uninit]], bool c) {
  std::construct_at(p, 1);
  if (c)
    std::destroy_at(p);
  return *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}


struct Base { int b; };
struct Derived : Base { int d; };
[[now_init]] void fill_base(Base *p [[ref_to_uninit]]);
[[now_init]] void fill_base_ref(Base &r [[ref_to_uninit]]);
[[now_init]] void fill_bytes(void *p [[ref_to_uninit]]);
[[now_init]] void fill_ints(int *p [[ref_to_uninit]]);
namespace std {
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);
} // namespace std
extern "C" void *memset(void *, int, __SIZE_TYPE__);
struct Pair { int x; int y; };

// construct_at of a base subobject leaves the derived member uninitialized
int construct_base_subobject() {
  Derived d [[uninit]];
  std::construct_at(static_cast<Base *>(&d), 1);
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// construct_at through the implicit conversion to the base
int construct_base_implicit() {
  Derived d [[uninit]];
  std::construct_at<Base>(&d, 1);
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A [[now_init]] call through a base-typed parameter initializes the base
int now_init_base_subobject() {
  Derived d [[uninit]];
  fill_base(&d);
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A [[now_init]] call through a base-typed reference parameter initializes the base
int now_init_base_reference() {
  Derived d [[uninit]];
  fill_base_ref(d);
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// construct_at through a marked base pointer to a derived local
int construct_through_base_pointer() {
  Derived d [[uninit]];
  Base *bp [[ref_to_uninit]] = &d;
  std::construct_at(bp, 1);
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// construct_at through a marked derived pointer's base conversion
int construct_base_through_marker(Derived *p [[ref_to_uninit]]) {
  std::construct_at<Base>(p, 1);
  return p->d; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A raw-bytes credit through a conversion to a base initializes only the base
int fill_bytes_base_subobject() {
  Derived d [[uninit]];
  fill_bytes(static_cast<Base *>(&d));
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A raw-bytes credit through a marked derived pointer's base conversion
int fill_bytes_base_through_marker(Derived *p [[ref_to_uninit]]) {
  fill_bytes(static_cast<Base *>(p));
  return p->d; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A raw-bytes credit through a marked derived reference's base conversion
int fill_bytes_base_through_reference(Derived &r [[ref_to_uninit]]) {
  fill_bytes(static_cast<Base *>(&r));
  return r.d; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A raw-bytes credit through a marked base pointer to a derived local
int fill_bytes_through_base_pointer() {
  Derived d [[uninit]];
  Base *bp [[ref_to_uninit]] = &d;
  fill_bytes(bp);
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// memset through a conversion to a base initializes only the base
int memset_base_subobject() {
  Derived d [[uninit]];
  memset(static_cast<Base *>(&d), 0, sizeof(Base));
  return d.d; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// construct_at of an int into a class object
int construct_int_into_object() {
  Pair a [[uninit]];
  std::construct_at((int *)&a, 5);
  return a.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// construct_at of a char into a class object
int construct_char_into_object() {
  Pair a [[uninit]];
  std::construct_at((char *)&a, 0);
  return a.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A [[now_init]] call through a void pointer parameter initializes the whole object
int now_init_raw_bytes() {
  Pair a [[uninit]];
  fill_bytes(&a);
  return a.y; // OK
}

// memset initializes the whole object
int memset_object() {
  Pair a [[uninit]];
  memset(&a, 0, sizeof a);
  return a.y; // OK
}

// A range fill of an array designates its element type
int fill_array_range() {
  int arr [[uninit]] [4];
  std::uninitialized_fill(arr, arr + 4, 0);
  return arr[3]; // OK
}

// construct_at of an array constructs its first element
int construct_array_first_element() {
  int arr [[uninit]] [4];
  std::construct_at(arr, 1);
  return arr[3]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A [[now_init]] call through a pointer to a smaller array initializes part of the array
[[now_init]] void fill_two(int (*p [[ref_to_uninit]])[2]);
int now_init_smaller_array() {
  int arr [[uninit]] [4];
  fill_two((int (*)[2])&arr);
  return arr[3]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A [[now_init]] call through a pointer to the whole array initializes it
[[now_init]] void fill_four(int (*p [[ref_to_uninit]])[4]);
int now_init_whole_array() {
  int arr [[uninit]] [4];
  fill_four(&arr);
  return arr[3]; // OK
}

// A [[now_init]] call with a decayed array designates its element type
int now_init_array() {
  int arr [[uninit]] [4];
  fill_ints(arr);
  return arr[3]; // OK
}

struct DtorPayload { int x; ~DtorPayload(); };

// A read after an explicit destructor call reads destroyed storage
int read_after_destructor_call(DtorPayload *p [[ref_to_uninit]]) {
  std::construct_at(p);
  p->~DtorPayload();
  return p->x; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A second explicit destructor call is a double destruction
void double_destructor_call(DtorPayload *p [[ref_to_uninit]]) {
  std::construct_at(p);
  p->~DtorPayload();
  p->~DtorPayload(); // expected-error {{storage already destroyed by a destructor call is destroyed again under profile 'std::init'}}
}

// An explicit destructor call on never-constructed storage destroys uninitialized storage
void destructor_call_uninitialized(DtorPayload *p [[ref_to_uninit]]) {
  p->~DtorPayload(); // expected-error {{uninitialized storage is destroyed by a destructor call under profile 'std::init'}}
}

// Reconstruction after an explicit destructor call is accepted
void reconstruct_after_destructor_call(DtorPayload *p [[ref_to_uninit]]) {
  std::construct_at(p);
  p->~DtorPayload();
  std::construct_at(p);
}

// A read of a local after its explicit destructor call reads destroyed storage
int read_local_after_destructor_call() {
  Payload s [[uninit]];
  std::construct_at(&s, 1);
  s.~Payload();
  return s.y; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

using Int = int;
// A second pseudo-destructor call is a double destruction
void double_pseudo_destructor_call(Int *p [[ref_to_uninit]]) {
  std::construct_at(p, 1);
  p->~Int();
  p->~Int(); // expected-error {{storage already destroyed by a destructor call is destroyed again under profile 'std::init'}}
}

// A delete-expression runs the destructor of never-constructed storage
void delete_uninitialized() {
  DtorPayload *p [[ref_to_uninit]] = new DtorPayload;
  delete p; // expected-error {{uninitialized storage is destroyed by a destructor call under profile 'std::init'}}
}

// A delete-expression after construction is accepted
void delete_constructed() {
  DtorPayload *p [[ref_to_uninit]] = new DtorPayload;
  std::construct_at(p);
  delete p;
}

// An explicit destructor call through a marked pointer member destroys its referent, which only the marker asserts uninitialized
struct DestroysMarkedMember {
  DtorPayload *p [[ref_to_uninit]];
  void reset() {
    p->~DtorPayload(); // OK
  }
  void reset_twice() {
    p->~DtorPayload();
    p->~DtorPayload(); // expected-error {{storage already destroyed by a destructor call is destroyed again under profile 'std::init'}}
  }
};

//--- sys/init_mem.h
inline void *operator new(__SIZE_TYPE__, void *p) noexcept { return p; }
namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args) {
  return ::new (static_cast<void *>(p)) T(static_cast<A &&>(args)...);
}
template <class T> void destroy_at(T *p);
template <class T> T *now_init(T *p) {
  return p;
}
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);
template <class In, class Out> Out uninitialized_copy(In first, In last, Out result);
} // namespace std

//--- system.cpp
// Calls to functions declared in a system header are checked the same way:
// the violation is located at the user's call site, outside the
// system-header exemption. The roles are the call's: the library's
// unmarked definitions are judged against their own declarations, so
// -fno-profiles-exempt-system-headers finds nothing in them.
[[profiles::enforce(std::init)]];
#include <init_mem.h>

void user() {
  int u [[uninit]];
  std::construct_at(&u, 5);
  int v = u; // OK: initialized
  std::destroy_at(&u);
  std::destroy_at(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
  (void)v;
}

// The call site's marker check is the user's
void now_init_user() {
  int u [[uninit]];
  int *s = std::now_init(&u); // OK
  int x = 1;
  std::now_init(&x); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)s;
}

// The uninitialized_* verdicts are located at the user's call
void uninitialized_user() {
  int a [[uninit]] [3];
  std::uninitialized_fill(a, a + 3, 1);
  int b [[uninit]] [3];
  std::uninitialized_copy(a, a + 3, b);
  int y = b[0]; // OK
  std::uninitialized_fill(a, a + 3, 2); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)y;
}

//--- now_init_body.cpp
// P4222R2 §4.4: the identity definition cannot compile with the profile
// enforced. Outside a system header its instantiation is diagnosed.
[[profiles::enforce(std::init)]];

namespace std {
template <class T> T *now_init(T *p [[ref_to_uninit]]) {
  return p; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
} // namespace std

void user() {
  int u [[uninit]];
  int *s = std::now_init(&u); // expected-note {{in instantiation of function template specialization 'std::now_init<int>' requested here}}
  (void)s;
}

//--- va.cpp
// va_start and va_copy initialize their va_list whatever its state; va_arg
// binds a class-type va_list (AArch64, ARM) to a reference. The [[uninit]]
// marker is accepted on a va_list of any representation, a pointer (i386)
// included.
[[profiles::enforce(std::init)]];

// The canonical variadic idiom
int sum(int n, ...) {
  __builtin_va_list ap [[uninit]];
  __builtin_va_start(ap, n);
  int r = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return r;
}

// va_copy initializes its destination and reads its source
int copy(int n, ...) {
  __builtin_va_list ap [[uninit]], aq [[uninit]];
  __builtin_va_copy(aq, ap); // array-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}} \
                             // record-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}} \
                             // pointer-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  __builtin_va_start(ap, n);
  __builtin_va_copy(aq, ap);
  int r = __builtin_va_arg(aq, int);
  __builtin_va_end(aq);
  __builtin_va_end(ap);
  return r;
}

// va_arg and va_end before va_start read an uninitialized va_list
int before_start(int n, ...) {
  __builtin_va_list ap [[uninit]];
  int r = __builtin_va_arg(ap, int); // record-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  __builtin_va_end(ap); // array-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}} \
                        // record-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}} \
                        // pointer-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  return r;
}

// A value-initialized va_list is accepted
int value_initialized(int n, ...) {
  __builtin_va_list ap = {};
  __builtin_va_start(ap, n);
  int r = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return r;
}

// A pointer is still rejected
int *pointer_marker() {
  char *p [[uninit]]; // expected-error {{'[[uninit]]' cannot be applied to a pointer under profile 'std::init'; initialize the pointer (for example to 'nullptr')}}
  return nullptr;
}

// An unmarked va_list without an initializer is diagnosed
int unmarked(int n, ...) {
  __builtin_va_list ap; // expected-error {{variable 'ap' must be initialized or marked '[[uninit]]' under profile 'std::init'}}
  __builtin_va_start(ap, n);
  __builtin_va_end(ap);
  return 0;
}
