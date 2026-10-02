// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/local.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/referent.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -isystem %t/sys -std=c++23 %t/system.cpp
// RUN: %clang_cc1 -fsyntax-only -verify=expected,strict -fprofiles -fprofiles-verify-deferred-checks -fno-profiles-exempt-system-headers -isystem %t/sys -std=c++23 %t/system.cpp
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -std=c++23 %t/now_init_body.cpp
//
// The lifecycle attributes are injected only under -fprofiles (enforcement
// not required), and appear on both the pattern and its instantiations.
// RUN: %clang_cc1 -ast-dump -fprofiles -std=c++23 %t/dump.cpp | FileCheck %s --check-prefix=INJECT
// RUN: %clang_cc1 -ast-dump -std=c++23 %t/dump.cpp | FileCheck %s --check-prefix=PLAIN

// Clang attaches the std::init lifecycle markers to std::construct_at
// (RefToUninit on the first parameter + NowInit) and std::destroy_at
// (NowUninit) itself, keyed on a pointer-typed first parameter, the
// parameter marker alone to std::now_init (one pointer parameter, pointer
// return), RefToUninit on the output range plus NowInit to the
// std::uninitialized_* family (by name and arity; the default_construct
// forms get the marker alone), and NowUninit to std::destroy and
// std::destroy_n, so the real library functions are usable under
// enforcement.

// INJECT: FunctionDecl {{.*}} construct_at 'T *(T *, A &&...)'
// INJECT: RefToUninitAttr {{.*}} Implicit
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} construct_at 'int *(int *, int &&)' implicit_instantiation
// INJECT: RefToUninitAttr {{.*}} Implicit
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} destroy_at 'void (T *)'
// INJECT: NowUninitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} now_init 'T *(T *)'
// INJECT: RefToUninitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} now_init 'int *(int *)' implicit_instantiation
// INJECT: RefToUninitAttr {{.*}} Implicit
// INJECT-NOT: NowInitAttr
// INJECT: FunctionDecl {{.*}} uninitialized_fill 'void (It, It, const T &)'
// INJECT-NEXT: ParmVarDecl {{.*}} first 'It'
// INJECT-NEXT: RefToUninitAttr {{.*}} Implicit
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} uninitialized_fill 'void (int *, int *, const int &)' implicit_instantiation
// INJECT: ParmVarDecl {{.*}} first 'int *'
// INJECT-NEXT: RefToUninitAttr {{.*}} Implicit
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} uninitialized_fill 'void (Iter, Iter, const int &)' implicit_instantiation
// INJECT-NOT: RefToUninitAttr
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} uninitialized_copy 'Out (In, In, Out)'
// INJECT-NOT: RefToUninitAttr
// INJECT: ParmVarDecl {{.*}} result 'Out'
// INJECT-NEXT: RefToUninitAttr {{.*}} Implicit
// INJECT: NowInitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} destroy 'void (It, It)'
// INJECT-NOT: RefToUninitAttr
// INJECT: NowUninitAttr {{.*}} Implicit
// INJECT: FunctionDecl {{.*}} destroy_n 'It (It, Size)'
// INJECT-NOT: RefToUninitAttr
// INJECT: NowUninitAttr {{.*}} Implicit

// PLAIN-NOT: RefToUninitAttr
// PLAIN-NOT: NowInitAttr
// PLAIN-NOT: NowUninitAttr

//--- dump.cpp
namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
template <class T> T *now_init(T *p);
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);
template <class In, class Out> Out uninitialized_copy(In first, In last, Out result);
template <class It> void destroy(It first, It last);
template <class It, class Size> It destroy_n(It first, Size n);
} // namespace std

struct Iter { int *p; };

void instantiate(int *p) {
  std::construct_at(p, 5);
  std::destroy_at(p);
  std::now_init(p);
  std::uninitialized_fill(p, p + 1, 0);
  std::uninitialized_copy(p, p + 1, p);
  std::uninitialized_fill(Iter{p}, Iter{p}, 0);
}

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
template <class T, __SIZE_TYPE__ N> T *begin(T (&a)[N]);
template <class T, __SIZE_TYPE__ N> T *end(T (&a)[N]);
template <class T> struct allocator { T *allocate(__SIZE_TYPE__ n); };
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

// default_construct and its _n form take an uninitialized range and credit nothing
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

// A range from an unannotated allocator is initialized memory
void allocator_range(std::allocator<int> &alloc) {
  int *b = alloc.allocate(3);
  std::uninitialized_fill_n(b, 3, 0); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
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

//--- sys/init_mem.h
namespace std {
template <class T, class... A> T *construct_at(T *p, A &&...args);
template <class T> void destroy_at(T *p);
template <class T> T *now_init(T *p) {
  return p; // strict-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template <class It, class T> void uninitialized_fill(It first, It last, const T &x);
template <class In, class Out> Out uninitialized_copy(In first, In last, Out result);
} // namespace std

//--- system.cpp
// Declarations in a system header are annotated the same way, and the checks
// still fire: the violation is located at the user's call site, outside the
// system-header exemption. The library's now_init definition violates the
// binding rule inside the header, which only
// -fno-profiles-exempt-system-headers diagnoses.
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

// The now_init definition's violation lies in the system header; the call
// site's marker check is the user's.
void now_init_user() {
  int u [[uninit]];
  int *s = std::now_init(&u); // OK
  // strict-note@-1 {{in instantiation of function template specialization 'std::now_init<int>' requested here}}
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
template <class T> T *now_init(T *p) {
  return p; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
} // namespace std

void user() {
  int u [[uninit]];
  int *s = std::now_init(&u); // expected-note {{in instantiation of function template specialization 'std::now_init<int>' requested here}}
  (void)s;
}
