// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -std=c++23 %s

[[profiles::enforce(std::init)]];

extern "C" void *malloc(__SIZE_TYPE__);

__attribute__((objc_root_class))
@interface A
- (void)take:(int *)p;
- (void)fill:(int *) [[ref_to_uninit]] p;
- (void)vararg:(int *)p, ...;
@property int *prop;
@end

// A message argument binds the method's unmarked parameter
void test_unmarked_param(A *a) {
  int u [[uninit]];
  [a take:&u]; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  u = 1;
  [a take:&u]; // OK
}

// A marked local pointer is judged by its referent's flow state
void test_marked_local(A *a) {
  int *p [[ref_to_uninit]] = (int *)malloc(4);
  [a take:p]; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  *p = 1;
  [a take:p]; // OK
}

// A marked method parameter accepts uninitialized storage and rejects initialized storage
void test_marked_param(A *a) {
  int u [[uninit]];
  [a fill:&u]; // OK
  int v [[uninit]];
  v = 1;
  [a fill:&v]; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A variadic method's extra arguments bind nothing
void test_variadic(A *a) {
  int u [[uninit]];
  [a vararg:nullptr, &u]; // OK
}

// A property assignment's value is not judged
void test_property_assignment(A *a) {
  int u [[uninit]];
  a.prop = &u; // OK
}

// A message inside a method body is judged by the method's own flow
@implementation A
- (void)take:(int *)p {}
- (void)fill:(int *)p {}
- (void)vararg:(int *)p, ... {}
- (void)selfSend {
  int u [[uninit]];
  [self take:&u]; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  u = 1;
  [self take:&u]; // OK
}
@end
