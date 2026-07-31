// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 %s

// std::init: known allocator callees are classified without annotation
// (paper §4.3: malloc returns a pointer to uninitialized memory, calloc to
// zero-initialized memory, and such functions "must be known to an analyzer
// enforcing the initialization profile"). The knowledge keys on Clang's
// builtin recognition, so the library functions are declared with matching
// signatures here; -fno-builtin would fall back to the trusted-initialized
// default (a missed diagnostic, never a false positive).

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

typedef __SIZE_TYPE__ size_t;
extern "C" void *malloc(size_t);
extern "C" void *calloc(size_t, size_t);
extern "C" void *realloc(void *, size_t);
extern "C" void *aligned_alloc(size_t, size_t);
extern "C" void free(void *);

void take_uninit_ptr(int *p [[ref_to_uninit]]);
void take_ptr(int *p);

// malloc returns uninitialized memory: a marked target accepts it, and an
// unmarked one must not bind it (paper §4.3's `void* p2 = &x2` error).
void test_malloc() {
  int *p [[ref_to_uninit]] = (int *)malloc(4); // OK: the paper's canonical use
  void *v [[ref_to_uninit]] = malloc(4);       // OK: no cast needed for void*
  int *q = (int *)malloc(4); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_uninit_ptr((int *)malloc(4)); // OK
  take_ptr((int *)malloc(4)); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// The __builtin_ spellings need no declaration and classify identically.
void test_builtin_spellings() {
  int *p [[ref_to_uninit]] = (int *)__builtin_malloc(4); // OK
  int *q = (int *)__builtin_malloc(4); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *r = (int *)__builtin_calloc(1, 4); // OK: zero-initialized
}

// calloc zero-initializes (paper §4.3), so its result is initialized memory
// and the marked direction flips.
void test_calloc() {
  int *p = (int *)calloc(1, 4); // OK
  int *q [[ref_to_uninit]] = (int *)calloc(1, 4); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// realloc preserves a prefix and leaves the tail indeterminate:
// affirmatively neither initialized nor uninitialized, so neither binding
// direction diagnoses.
void test_realloc(void *v) {
  int *p = (int *)realloc(v, 8);                   // OK: unknown
  int *q [[ref_to_uninit]] = (int *)realloc(v, 8); // OK: unknown
}

void test_aligned_alloc() {
  int *p [[ref_to_uninit]] = (int *)aligned_alloc(16, 16); // OK
  int *q = (int *)aligned_alloc(16, 16); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// alloca's stack memory is as uninitialized as malloc's heap memory.
void test_alloca() {
  int *p [[ref_to_uninit]] = (int *)__builtin_alloca(4); // OK
  int *q = (int *)__builtin_alloca(4); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Allocator sources compose with the existing machinery: a write through the
// marked pointer is the pointee's initialization (with parse-order credit),
// and a read before it is the read-through violation.
void test_write_then_read() {
  int *p [[ref_to_uninit]] = (int *)malloc(4);
  *p = 5;     // OK: initializes the pointee
  int x = *p; // OK: credited
}

void test_read_through() {
  int *p [[ref_to_uninit]] = (int *)malloc(4);
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// A direct call to a replaceable global allocation function returns
// uninitialized memory exactly like malloc (a new-*expression* is recognized
// separately, from its initialization style).
void test_operator_new() {
  int *p [[ref_to_uninit]] = (int *)::operator new(4); // OK
  int *q = (int *)::operator new(4); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *r = (int *)::operator new[](8); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *s [[ref_to_uninit]] = (int *)__builtin_operator_new(4); // OK
}

// A class-specific operator new is not replaceable; its semantics belong to
// its class, so it stays a trusted unmarked callee.
struct PoolAllocated {
  static void *operator new(size_t);
};
void test_class_specific_operator_new() {
  int *p [[ref_to_uninit]] = (int *)PoolAllocated::operator new(4); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *q = (int *)PoolAllocated::operator new(4); // OK: trusted
}

// Suppression covers the binding like any other ref_to_uninit site.
void test_suppress() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]]
  int *q = (int *)malloc(4); // OK: suppressed
}

// A Decl-less binding of a non-dependent allocator call (a call argument) is
// checked at definition time even in a never-instantiated template, like the
// other all-non-dependent shapes (TreeTransform may reuse the node).
template <class T>
void template_malloc_arg() {
  take_ptr((int *)malloc(4)); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// ============================================================
// Storage-release callees (the deallocation side)
// ============================================================

// free, realloc's pointer argument, and replaceable global operator delete
// release the storage they are handed: the binding accepts a pointer in any
// state ([[now_uninit]]-equivalent), so deallocating never-written storage
// is legal -- the RAII buffer below is the motivating shape -- and the
// storage's credit is withdrawn, so a whole-`*p` read through a marked
// pointer after the release is the read-through violation again. Like the
// allocator side, free/realloc recognition keys on Clang's builtin
// knowledge (-fno-builtin loses the relaxation, never more).
struct RAIIBuf {
  int *p [[ref_to_uninit]];
  RAIIBuf() : p((int *)malloc(16)) {}
  ~RAIIBuf() { free(p); } // OK: releasing storage in any state
};

void test_free_uninit(int *p [[ref_to_uninit]]) {
  free(p); // OK: never-initialized storage may be released
}
void test_free_initialized(int *p [[ref_to_uninit]]) {
  *p = 5;
  free(p); // OK: initialized storage may be released
}
void test_builtin_free(int *p [[ref_to_uninit]]) {
  __builtin_free(p); // OK: same recognition by builtin ID
}
void test_operator_delete(int *p [[ref_to_uninit]],
                          int *q [[ref_to_uninit]]) {
  ::operator delete(p);   // OK: replaceable global deallocation
  ::operator delete[](q); // OK
}
void test_read_after_free(int *p [[ref_to_uninit]]) {
  *p = 5;
  int x = *p; // OK: credited
  free(p);
  int y = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x; (void)y;
}
void test_realloc_releases_pointer_argument(int *p [[ref_to_uninit]]) {
  *p = 5;
  int *r = (int *)realloc(p, 32); // OK both ways: unknown source, releasing sink
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)r; (void)x;
}

// Ending the object's lifetime and then releasing its storage are two
// different, correctly ordered operations: the release callees are exempt
// from the double-destroy check.
template <class T> [[now_uninit]] void destroy_at(T *p);
void test_destroy_then_free(int *p [[ref_to_uninit]]) {
  *p = 5;
  destroy_at(p);
  free(p); // OK: not a double destroy
}

// A class-specific operator delete is not replaceable; its semantics belong
// to its class, so its parameter stays the ordinary unmarked target.
struct PoolDeallocated {
  static void operator delete(void *);
};
void test_class_specific_operator_delete(int *p [[ref_to_uninit]]) {
  PoolDeallocated::operator delete(p); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
