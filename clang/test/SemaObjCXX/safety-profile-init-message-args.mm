// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-verify-deferred-checks -fobjc-exceptions -std=c++23 %s

[[profiles::enforce(std::init)]];

extern "C" void *malloc(__SIZE_TYPE__);

__attribute__((objc_root_class))
@interface A
- (void)take:(int *)p;
- (void)fill:(int *) [[ref_to_uninit]] p;
- (void)vararg:(int *)p, ...;
@property int *prop;
@property(nonatomic) int *mprop;
- (void)setMprop:(int *) [[ref_to_uninit]] p;
@property int val;
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

// A property assignment binds its value to the setter's parameter
void test_property_assignment(A *a) {
  int u [[uninit]];
  a.prop = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  a.mprop = &u; // OK
  u = 1;
  a.prop = &u;  // OK
  a.mprop = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A property assignment in a lambda body names the enclosing variable
void test_property_assignment_in_lambda(A *a) {
  int u [[uninit]];
  u = 1;
  auto l = [&] { a.prop = &u; }; // OK
  (void)l;
}

// A property assignment reads its value
void test_property_value_read(A *a, int *p [[ref_to_uninit]]) {
  a.val = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  *p = 1;
  a.val = *p; // OK
}

__attribute__((objc_root_class))
@interface NSString
+ (id)stringWithUTF8String:(const char *)s;
@end

// A boxed expression's operand is not judged
void test_boxed() {
  char buf [[uninit]] [4];
  id s = @(buf); // OK
  (void)s;
}

// A body whose CFG cannot be built is not judged by flow
void test_finally(A *a) {
  int u [[uninit]];
  u = 1;
  [a take:&u]; // OK
  @try {
  } @finally {
  }
}

// A message inside a method body is judged by the method's own flow
@implementation A
- (void)take:(int *)p {}
- (void)fill:(int *)p {}
- (void)vararg:(int *)p, ... {}
- (void)setMprop:(int *)p {}
- (void)selfSend {
  int u [[uninit]];
  [self take:&u]; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  u = 1;
  [self take:&u]; // OK
}
@end
