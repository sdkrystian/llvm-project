// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 %s

static_assert(__has_cpp_attribute(must_init) == 202602);

int g;
struct Handle { int *p; };

// A pointer, reference, or handle parameter carries the marker
void fill(int *p [[must_init]]);
void fill_ref(int &r [[must_init]]);
void fill_prefix([[must_init]] int *p);
void fill_void(void *p [[must_init]]);
void fill_handle(Handle h [[must_init]]);
void fill_const_pointer(int *const p [[must_init]]);
void fill_many(int n, int *p [[must_init]], int *q [[ref_to_uninit]]);
void fill_both(int *p [[must_init]] [[ref_to_uninit]]);
void fill_both_reversed(int *p [[ref_to_uninit]] [[must_init]]);

// A member function, a static member function, and a lambda carry it
struct S {
  void fill(int *p [[must_init]]);
  static S make(int *p [[must_init]]);
  void fill_out_of_line(int *p [[must_init]]);
};
void S::fill_out_of_line(int *p) {}
auto fill_lambda = [](int *p [[must_init]]) {};

// A later declaration inherits the marker
void fill_redecl(int *p [[must_init]]);
void fill_redecl(int *p);
void fill_redecl_added(int *p);
void fill_redecl_added(int *p [[must_init]]);

// A function template's parameter carries it
template <typename T> void fill_template(T *p [[must_init]]);

// The marker satisfies the [[now_init]] vacuity rule
[[now_init]] void fill_now_init(int *p [[must_init]]);

// Only a parameter can carry the marker
int *bad_var [[must_init]]; // expected-error {{'must_init' attribute only applies to parameters}} \
                            // no-profiles-error {{'must_init' attribute only applies to parameters}}
struct BadField {
  int *m [[must_init]]; // expected-error {{'must_init' attribute only applies to parameters}} \
                        // no-profiles-error {{'must_init' attribute only applies to parameters}}
};
[[must_init]] int *bad_function(int *p); // expected-error {{'must_init' attribute only applies to parameters}} \
                                         // no-profiles-error {{'must_init' attribute only applies to parameters}}
void bad_local() {
  int *p [[must_init]] = &g; // expected-error {{'must_init' attribute only applies to parameters}} \
                             // no-profiles-error {{'must_init' attribute only applies to parameters}}
}

// A parameter of another type is rejected
void bad_scalar(int v [[must_init]]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                      // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}
void bad_fn_ptr(void (*f [[must_init]])()); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                            // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}
union Union { int *p; int i; };
void bad_union(Union u [[must_init]]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                       // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}

// A pointer or reference to const is rejected
void bad_const(const int *p [[must_init]]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                            // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}
void bad_const_ref(const int &r [[must_init]]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                                // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}
void bad_const_void(const void *p [[must_init]]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                                  // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}
void bad_const_array(const int (*p [[must_init]])[2]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                                      // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}
void bad_const_inner(int *const *p [[must_init]]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                                   // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}

// A pointer or reference to const may still carry [[ref_to_uninit]]
void const_ref_to_uninit(const int *p [[ref_to_uninit]]);
void bad_const_both(const int *p [[ref_to_uninit]] [[must_init]]); // expected-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                                                   // no-profiles-error {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}

// A dependent parameter is validated per instantiation of its class
template <typename T> struct Box {
  void fill(T p [[must_init]]); // expected-error 2 {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}} \
                                // no-profiles-error 2 {{'must_init' attribute only applies to pointers and references to non-const objects, and non-union classes}}
};
template struct Box<int *>;
template struct Box<Handle>;
template struct Box<int>; // expected-note {{in instantiation of template class 'Box<int>' requested here}} \
                          // no-profiles-note {{in instantiation of template class 'Box<int>' requested here}}
template struct Box<const int *>; // expected-note {{in instantiation of template class 'Box<const int *>' requested here}} \
                                  // no-profiles-note {{in instantiation of template class 'Box<const int *>' requested here}}

// A dependent function template parameter's invalid marker is dropped silently
template <typename T> void fill_dependent(T p [[must_init]]) {}
template void fill_dependent<int *>(int *);
template void fill_dependent<int>(int);
template void fill_dependent<const int *>(const int *);

// A constructor parameter cannot carry the marker
struct Filler {
  Filler(int *p [[must_init]]); // expected-error {{'must_init' attribute cannot be applied to a constructor}} \
                                // no-profiles-error {{'must_init' attribute cannot be applied to a constructor}}
  Filler(int n, int *p [[ref_to_uninit]]);
  template <typename T>
  Filler(T *p [[must_init]], T *q); // expected-error {{'must_init' attribute cannot be applied to a constructor}} \
                                    // no-profiles-error {{'must_init' attribute cannot be applied to a constructor}}
  static Filler make(int *p [[must_init]]);
};
Filler::Filler(int n, int *p [[must_init]]) {} // expected-error {{'must_init' attribute cannot be applied to a constructor}} \
                                               // no-profiles-error {{'must_init' attribute cannot be applied to a constructor}}

// Inert without an enforced profile
void use() {
  int x = 0;
  fill(&x);
  fill_ref(x);
}
