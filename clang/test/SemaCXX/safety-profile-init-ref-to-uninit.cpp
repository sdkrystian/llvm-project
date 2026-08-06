// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fblocks -fcxx-exceptions -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -fblocks -fcxx-exceptions -std=c++23 %s

// std::init / ref_to_uninit (paper §5): a [[ref_to_uninit]] pointer or
// reference must be bound to uninitialized memory, and an unmarked pointer or
// reference must not. This file exercises the rule at variable initialization.

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

int g_init = 0;
int g_init2 = 0;
// Static fixtures supplying uninitialized memory for the pointer/reference
// tests below. A static [[uninit]] is rejected by static_marker (paper section
// 4.2), so suppress that rule here: the test deliberately creates uninitialized
// static storage, and suppression keeps the marker (the source stays
// "uninitialized memory" for the ref_to_uninit checks).
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "static_marker")]] [[uninit]] int g_uninit;
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "static_marker")]] [[uninit]] int g_uninit_arr[3];
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "static_marker")]] [[uninit]] int g_uninit2;
[[ref_to_uninit]] int *allocate(int n);
[[ref_to_uninit]] void *alloc_void();
[[ref_to_uninit]] int &get_uninit_ref();
void h();

void test_pointer_target() {
  int *p1 [[ref_to_uninit]] = &g_uninit; // OK
  int *p2 [[ref_to_uninit]] = &g_init;   // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *p3 = &g_uninit;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p4 = &g_init;                      // OK
  int *p5 = nullptr;                      // OK
  (void)p1; (void)p2; (void)p3; (void)p4; (void)p5;
}

void test_pointer_sources() {
  int *base [[ref_to_uninit]] = &g_uninit;
  int *from_ptr [[ref_to_uninit]] = base;           // OK: base is [[ref_to_uninit]]
  int *from_array [[ref_to_uninit]] = g_uninit_arr; // OK: array-to-pointer decay
  int *from_call [[ref_to_uninit]] = allocate(3);   // OK: [[ref_to_uninit]] return
  int *bad_from_array = g_uninit_arr;               // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *bad_from_call = allocate(3);                 // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)base; (void)from_ptr; (void)from_array; (void)from_call;
  (void)bad_from_array; (void)bad_from_call;
}

// An explicit pointer-to-pointer cast of a [[ref_to_uninit]] pointer is itself
// [[ref_to_uninit]] (paper §4.3); the cast does not launder the marking.
void test_pointer_casts() {
  void *vp [[ref_to_uninit]] = &g_uninit;
  int *c1 [[ref_to_uninit]] = (int *)vp; // OK
  int *c2 = (int *)vp;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  int *sc1 [[ref_to_uninit]] = static_cast<int *>(vp);      // OK
  int *sc2 = static_cast<int *>(vp);                         // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *rc1 [[ref_to_uninit]] = reinterpret_cast<int *>(vp); // OK
  int *rc2 = reinterpret_cast<int *>(vp);                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  // An initialized pointer round-tripped through a cast stays initialized.
  void *vi = &g_init;
  int *ci1 = (int *)vi;                   // OK
  int *ci2 [[ref_to_uninit]] = (int *)vi; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}

  // Casting a [[ref_to_uninit]]-returning call result propagates the marking.
  int *cc1 [[ref_to_uninit]] = (int *)alloc_void(); // OK
  int *cc2 = (int *)alloc_void();                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  // Trust model: a pointer manufactured from an integer is not propagated.
  int *ti = reinterpret_cast<int *>(0xdeadbeef); // OK

  // Deref of a cast routes back through the pointer recognizer.
  int &dr [[ref_to_uninit]] = *(int *)vp; // OK

  (void)c1; (void)c2; (void)sc1; (void)sc2; (void)rc1; (void)rc2;
  (void)vi; (void)ci1; (void)ci2; (void)cc1; (void)cc2; (void)ti; (void)dr;
}

void test_reference_target() {
  int &r1 [[ref_to_uninit]] = g_uninit; // OK
  int &r2 [[ref_to_uninit]] = g_init;   // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int &r3 = g_uninit;                   // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int &r4 = g_init;                     // OK
  int *p [[ref_to_uninit]] = &g_uninit;
  int &r5 [[ref_to_uninit]] = *p;       // OK: *p denotes uninitialized storage
  // A [[ref_to_uninit]] reference is itself a source of uninitialized storage,
  // symmetric to the [[ref_to_uninit]] pointer-copy case.
  int &r6 [[ref_to_uninit]] = r1;       // OK: r1 refers to uninitialized memory
  int &r7 = r1;                         // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)r1; (void)r2; (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)p;
}

// A reference cast (an explicit cast yielding a glvalue) denotes the same
// storage as its operand, and a [[ref_to_uninit]]-returning reference call
// denotes uninitialized storage. Symmetric to the pointer side.
void test_reference_casts() {
  int &cr1 [[ref_to_uninit]] = static_cast<int &>(g_uninit); // OK
  int &cr2 = static_cast<int &>(g_uninit);                    // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int &cr3 [[ref_to_uninit]] = (int &)g_init;                 // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}

  int &gr1 [[ref_to_uninit]] = get_uninit_ref(); // OK
  int &gr2 = get_uninit_ref();                    // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  // Address of a reference cast routes back through the pointer recognizer.
  int *p [[ref_to_uninit]] = &(int &)g_uninit; // OK

  (void)cr1; (void)cr2; (void)cr3; (void)gr1; (void)gr2; (void)p;
}

// A conversion-requiring reference binding materializes a *new* temporary
// initialized from the source's value: the reference binds initialized
// storage, so no ref_to_uninit binding diagnostic applies -- the value load
// from the [[uninit]] source is the flow-based read pass's (which fires
// below). A same-type binding builds no temporary and keeps its rejection.
void take_long(long l);
void test_temporary_binding_is_initialized() {
  int u [[uninit]]; // expected-note {{variable 'u' is declared here}}
  const long &r = u; // expected-error {{variable 'u' is read before initialization under profile 'std::init'}}
  (void)r;
}
void test_temporary_by_value_flavor() {
  int u [[uninit]]; // expected-note {{variable 'u' is declared here}}
  take_long(u); // expected-error {{variable 'u' is read before initialization under profile 'std::init'}}
}
void test_same_type_binding_still_fires() {
  int u [[uninit]];
  const int &r2 = u; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)r2;
}
// The marked direction is symmetric: a marked reference bound to a fresh
// temporary refers to *initialized* memory and is rejected.
struct TempAgg { int m; };
void test_marked_binding_to_temporary_rejected() {
  int u [[uninit]]; // expected-note {{variable 'u' is declared here}}
  long &&rr [[ref_to_uninit]] = u; // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}} \
                                   // expected-error {{variable 'u' is read before initialization under profile 'std::init'}}
  TempAgg &&sr [[ref_to_uninit]] = TempAgg(); // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)rr; (void)sr;
}
// A *pointer-typed* temporary is the deliberate exception: its value refers
// to the same storage the source did, so the qualification-conversion
// binding shapes classify by the pointee, exactly as without the temporary.
void test_pointer_temporary_binding(int *q) {
  const int *const &rp1 = q;                         // OK: unmarked value, unmarked target
  const int *const &rp2 [[ref_to_uninit]] = q;       // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  const int *const &rp3 = allocate(1);               // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  const int *const &rp4 [[ref_to_uninit]] = allocate(1); // OK: marked value, marked target
  (void)rp1; (void)rp2; (void)rp3; (void)rp4;
}

// Pass-through sources are transparent to their operand: a single-element
// braced initializer is looked through to its element, a conditional is
// uninitialized if either arm is, and a comma yields its right operand.
void test_braced_pointer() {
  int *b1 [[ref_to_uninit]] = {&g_uninit}; // OK
  int *b2 = {&g_uninit};                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *b3 [[ref_to_uninit]] = {&g_init};    // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *b4 = {&g_init};                       // OK
  // Empty {} value-initializes to nullptr, like = nullptr: a null source is
  // consistent with marked and unmarked targets alike (paper §8, §4.3; see
  // test_null_sources).
  int *b5 [[ref_to_uninit]] = {};            // OK
  int *b6 = {};                              // OK
  (void)b1; (void)b2; (void)b3; (void)b4; (void)b5; (void)b6;
}

void test_braced_reference() {
  int &r1 [[ref_to_uninit]] = {g_uninit}; // OK
  int &r2 = {g_uninit};                    // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int &r3 [[ref_to_uninit]] = {g_init};    // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int &r4 = {g_init};                       // OK
  (void)r1; (void)r2; (void)r3; (void)r4;
}

void test_conditional_pointer(bool c) {
  int *p1 = c ? &g_uninit : &g_init;                       // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p2 [[ref_to_uninit]] = c ? &g_uninit : &g_uninit2;  // OK: both arms uninitialized
  int *p3 [[ref_to_uninit]] = c ? &g_uninit : &g_init;     // OK: either arm may be uninitialized
  int *p4 [[ref_to_uninit]] = c ? &g_init : &g_init2;      // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *p5 = c ? &g_init : &g_init2;                        // OK
  (void)p1; (void)p2; (void)p3; (void)p4; (void)p5;
}

void test_conditional_reference(bool c) {
  int &r1 [[ref_to_uninit]] = c ? g_uninit : g_uninit2; // OK
  int &r2 = c ? g_uninit : g_init;                       // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int &r3 [[ref_to_uninit]] = c ? g_init : g_init2;      // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int &r4 = c ? g_init : g_init2;                        // OK
  (void)r1; (void)r2; (void)r3; (void)r4;
}

// The GNU `a ?: b` form classifies like the equivalent plain conditional:
// the common operand doubles as the true arm. Named pointers keep the
// conditions free of always-true address-of warnings; an unmarked pointer
// parameter is a trusted Initialized source, a marked one Uninitialized.
void test_gnu_conditional_pointer(int *q, int *q2, int *up [[ref_to_uninit]]) {
  int *p1 = up ?: q;                   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p2 [[ref_to_uninit]] = up ?: q; // OK: the common arm may be uninitialized
  int *p3 [[ref_to_uninit]] = q ?: q2; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *p4 = q ?: q2;                   // OK
  int *p5 = q ?: &g_uninit;            // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p1; (void)p2; (void)p3; (void)p4; (void)p5;
}

// A read through a GNU conditional over pointers fires like the plain form;
// a write through either conditional stays the top-level marker trust's
// (parity, both silent).
void test_gnu_conditional_read_write(int *q, int *p [[ref_to_uninit]], bool c) {
  int y1 = *(c ? q : p); // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y2 = *(q ?: p);    // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  *(c ? q : p) = 1;      // OK: top-level marker trust (write initializes the pointee)
  *(q ?: p) = 1;         // OK: same, GNU spelling
  (void)y1; (void)y2;
}

void test_comma_pointer() {
  int *p1 = (h(), &g_uninit);                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p2 [[ref_to_uninit]] = (h(), &g_uninit);  // OK
  int *p3 [[ref_to_uninit]] = (h(), &g_init);    // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *p4 = (h(), &g_init);                      // OK
  (void)p1; (void)p2; (void)p3; (void)p4;
}

void test_comma_reference() {
  int &r1 [[ref_to_uninit]] = (h(), g_uninit); // OK
  int &r2 = (h(), g_uninit);                    // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int &r3 [[ref_to_uninit]] = (h(), g_init);    // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int &r4 = (h(), g_init);                       // OK
  (void)r1; (void)r2; (void)r3; (void)r4;
}

void test_assignment() {
  int *p [[ref_to_uninit]] = &g_uninit;
  p = &g_uninit; // OK
  p = &g_init;   // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *q = &g_init;
  q = &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  q = &g_init;   // OK
  q = nullptr;   // OK
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { q = &g_uninit; } // OK: suppressed
  (void)p; (void)q;
}

// Assignment through indirection: the assigned-to pointer is reached by
// dereference or subscript, so it cannot carry a local [[ref_to_uninit]]
// marker. It is the default unmarked pointer and must not be bound to
// uninitialized memory, exactly as a directly-named pointer would be.
void test_assignment_indirect() {
  int *p = nullptr;
  int **pp = &p;
  *pp = &g_uninit;   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  *pp = &g_init;     // OK
  (*pp) = &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  *pp = new int;     // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  *pp = new int(0);  // OK

  int *arr[3] = {};
  arr[0] = &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  arr[1] = &g_init;   // OK
  (void)pp; (void)arr;
}

void test_suppress() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]] int *s = &g_uninit; // OK: suppressed
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] int *s2 [[ref_to_uninit]] = &g_init;       // OK: suppressed
  (void)s; (void)s2;
}

void take_uninit_ptr(int *p [[ref_to_uninit]]);
void take_uninit_ref(int &r [[ref_to_uninit]]);
void take_ptr(int *p);
void take_ref(const int &r);
void uninitialized_fill(int *r [[ref_to_uninit]], int val);

void test_call_arguments() {
  take_uninit_ptr(&g_uninit);    // OK
  take_uninit_ptr(&g_init);      // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  take_uninit_ptr(g_uninit_arr); // OK: array-to-pointer decay
  take_uninit_ref(g_uninit);     // OK
  take_uninit_ref(g_init);       // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}

  take_ptr(&g_uninit);           // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_ptr(&g_init);             // OK
  take_ref(g_uninit);            // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_ref(g_init);              // OK

  // A [[ref_to_uninit]] reference argument matches a [[ref_to_uninit]] reference
  // parameter, and is rejected for an unmarked one.
  int &ru [[ref_to_uninit]] = g_uninit;
  take_uninit_ref(ru);           // OK
  take_ref(ru);                  // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)ru;

  // The worked example from paper §5.
  int a1[] = {1, 2, 3};
  [[uninit]] int a2[3];
  uninitialized_fill(a1, 10); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  uninitialized_fill(a2, 10); // OK

  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { take_ptr(&g_uninit); } // OK: suppressed
}

// A null pointer refers to no object, so it is consistent with a marked
// target -- the marker means "zero or more uninitialized objects" (paper §8)
// -- and with an unmarked one (the paper §4.3 f1(p2) example): it classifies
// as unknown storage. This covers the literal forms and a named local whose
// declaration initializer is null.
void test_null_sources() {
  take_uninit_ptr(nullptr); // OK: null literal for a marked param
  take_uninit_ptr(0);       // OK: literal zero
  take_ptr(nullptr);        // OK

  int *p2 = nullptr;
  take_uninit_ptr(p2); // OK: the paper §4.3 example
  take_ptr(p2);        // OK

  int *q [[ref_to_uninit]] = p2; // OK: null is not affirmatively initialized
  int *q2 = p2;                  // OK
  (void)q; (void)q2;

  int *z = {};            // value-initializes to null, like = nullptr
  take_uninit_ptr(z);     // OK
  int *zb [[ref_to_uninit]] = {nullptr}; // OK: braced null recurses to the literal
  (void)zb;

  // The null-init classification is parse-order lenient: a reassignment after
  // the null declaration is not tracked, so passing the now-initialized
  // pointer to a marked parameter is an accepted missed diagnostic.
  int *r = nullptr;
  r = &g_init;
  take_uninit_ptr(r); // accepted: missed diagnostic (parse-order leniency)
}

// A zero-initialized *global* null pointer stays classified initialized:
// an extern pointer may be initialized elsewhere (another translation unit),
// and keeping globals initialized preserves the marked-direction
// diagnostics. Deliberate residual strictness.
int *g_null_ptr;
void test_null_global() {
  take_uninit_ptr(g_null_ptr); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A static local is excluded for the same reason as a global (not
// function-local state; hasLocalStorage is the gate).
void test_null_static_local() {
  static int *sp = nullptr;
  take_uninit_ptr(sp); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A *parameter* is excluded too: a ParmVarDecl's initializer is its default
// argument, which is not the parameter's value on most calls -- a
// defaulted-null parameter may be passed any caller pointer, so it must keep
// drawing the marked-target diagnostic.
void null_default_param(int *cp = nullptr) {
  take_uninit_ptr(cp); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// At the *call site* an omitted defaulted argument is the null literal
// itself: fine for a marked parameter.
void null_default_marked(int *p [[ref_to_uninit]] = nullptr);
void test_null_default_marked_param() {
  null_default_marked();        // OK: the null default argument
  null_default_marked(nullptr); // OK
}

// A *marked* pointer initialized to null keeps its marker classification as
// a source: the explicit marker is respected over the null initializer.
void test_null_init_marked_decl() {
  int *m [[ref_to_uninit]] = nullptr;
  int *m2 = m; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)m2;
}

// A defaulted pointer or reference argument is checked against the parameter's
// [[ref_to_uninit]] marking at the call site, like an explicit argument. The
// declarations themselves stay clean; the diagnostic fires at the call.
void def_uninit_ptr(int *p [[ref_to_uninit]] = &g_uninit);
void def_uninit_ptr_bad(int *p [[ref_to_uninit]] = &g_init);
void def_ptr(int *p = &g_init);
void def_ptr_bad(int *p = &g_uninit);
void def_uninit_ref(int &r [[ref_to_uninit]] = g_uninit);
void def_uninit_ref_bad(int &r [[ref_to_uninit]] = g_init);
void def_ref(int &r = g_init);
void def_ref_bad(int &r = g_uninit);

void test_default_arguments() {
  def_uninit_ptr();     // OK
  def_uninit_ptr_bad(); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  def_ptr();            // OK
  def_ptr_bad();        // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  def_uninit_ref();     // OK
  def_uninit_ref_bad(); // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  def_ref();            // OK
  def_ref_bad();        // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  // An explicit argument overrides the default and is checked on its own merits.
  def_ptr_bad(&g_init); // OK

  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { def_ptr_bad(); } // OK: suppressed
}

// Call arguments are checked at parameter copy-initialization, which also
// covers call forms that never reach GatherArgumentsForCall: calls to objects
// of class type (functors, lambdas) and overloaded operators (member and
// non-member).
struct MarkedFunctor {
  void operator()(int *p [[ref_to_uninit]]);
};
struct UnmarkedFunctor {
  void operator()(int *p);
};

void test_functor_arguments() {
  MarkedFunctor mf;
  mf(&g_uninit); // OK
  mf(&g_init);   // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  UnmarkedFunctor uf;
  uf(&g_init);   // OK
  uf(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { uf(&g_uninit); } // OK: suppressed
}

void test_lambda_arguments() {
  auto l = [](int *p [[ref_to_uninit]]) {};
  l(&g_uninit); // OK
  l(&g_init);   // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A call with no declared callee (through a function pointer) has no
// parameter declaration that could carry [[ref_to_uninit]], so its arguments
// are checked as unmarked targets (paper §7.2: passing uninitialized memory
// needs an appropriately declared callee). That holds even when the pointer
// happens to point at a function whose parameter is marked -- the marker is a
// declaration property, invisible through the pointer (paper §1.3, local
// analysis); suppress at the call if the flow is intended.
void test_fnptr_call_arguments(void (*fp)(int *), void (*fr)(int &)) {
  fp(&g_init);   // OK
  fp(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *rtu [[ref_to_uninit]] = &g_uninit;
  fp(rtu);       // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  fr(g_init);    // OK
  fr(g_uninit);  // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  void (*marked)(int *) = take_uninit_ptr;
  marked(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { fp(&g_uninit); } // OK: suppressed
  (void)rtu;
}

// Decl-less like the declared-callee argument site; the dependent callee type
// keeps the call unchecked on the pattern, so it fires once, at instantiation.
template <typename T>
void template_fnptr_call_arg(void (*fp)(T *)) {
  fp(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template void template_fnptr_call_arg<int>(void (*)(int *)); // expected-note {{in instantiation of function template specialization 'template_fnptr_call_arg<int>' requested here}}

// A variadic (...) argument never reaches parameter copy-initialization, and
// a ... parameter cannot carry [[ref_to_uninit]], so a pointer passed through
// it is checked as an unmarked target (paper §7.2). A *value* passed through
// ... is promoted with an ordinary lvalue-to-rvalue load, so its read is the
// read-through chokepoint's, not this site's.
void vf(int, ...);

void test_variadic_arguments() {
  vf(0, &g_init);      // OK
  vf(0, &g_uninit);    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *rtu [[ref_to_uninit]] = &g_uninit;
  vf(0, rtu);          // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  vf(0, g_uninit_arr); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  vf(0, *rtu);         // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}

  // Through a variadic function pointer: the named arguments are the
  // no-declared-callee site's, the ... arguments this one's -- exactly one
  // diagnostic either way.
  void (*vfp)(int, ...) = vf;
  vfp(0, &g_uninit);   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { vf(0, &g_uninit); } // OK: suppressed
  (void)rtu;
}

// Decl-less with a non-dependent argument: fires at definition time, and
// again when the call (always rebuilt) re-promotes the argument at
// instantiation -- the accepted repetition.
template <typename T>
void template_variadic_arg() {
  vf(0, &g_uninit); // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template void template_variadic_arg<int>(); // expected-note {{in instantiation of function template specialization 'template_variadic_arg<int>' requested here}}

// A call to an object of class type promotes its variadic arguments in its
// own loop (Sema::BuildCallToObjectOfClassType), distinct from
// GatherArgumentsForCall's; both are hooked, so variadic functors and
// variadic lambdas are covered too.
struct VariadicFunctor {
  void operator()(int, ...);
};

void test_variadic_functor_arguments() {
  VariadicFunctor f;
  f(0, &g_init);   // OK
  f(0, &g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  auto l = [](int, ...) {};
  l(0, &g_init);   // OK
  l(0, &g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A surrogate call converts to a function pointer and calls through it, so
// its named arguments are the no-declared-callee site's.
struct Surrogate {
  using FP = void (*)(int *);
  operator FP();
};

void test_surrogate_call_arguments() {
  Surrogate s;
  s(&g_init);   // OK
  s(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// An init-capture is a binding: a capture cannot carry [[ref_to_uninit]], so
// capturing a pointer or reference to uninitialized memory is always the
// unmarked-direction violation.
void test_init_captures() {
  auto c1 = [p = &g_init] { (void)p; };   // OK
  auto c2 = [p = &g_uninit] { (void)p; }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *rtu [[ref_to_uninit]] = &g_uninit;
  auto c3 = [&r = *rtu] { (void)r; }; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  auto c4 = [q = rtu] { (void)q; };   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  auto c5 = [v = g_init] { (void)v; }; // OK: a by-value int copy is not a binding
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] {
    auto c6 = [p = &g_uninit] { (void)p; }; // OK: suppressed
    (void)c6;
  }
  (void)c1; (void)c2; (void)c3; (void)c4; (void)c5;
}

// An init-capture inside a template body defers on the pattern and fires
// once, at instantiation.
template <typename T>
void template_init_capture_bad() {
  auto c = [p = &g_uninit] { (void)p; }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)c;
}
template void template_init_capture_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_init_capture_bad<int>' requested here}}

// A by-reference capture -- explicit or via a capture-default -- is the same
// binding as an init-capture: a capture cannot carry [[ref_to_uninit]], so
// capturing an [[uninit]] variable (or a [[ref_to_uninit]] reference) by
// reference is always the unmarked-direction violation. A copy capture is not
// a binding; it reads the variable in the enclosing function's CFG, which is
// the flow-based uninit_read pass's territory.
void test_ref_captures() {
  int x [[uninit]];
  int ok = 0;
  // The bodies must not assign x: a body store would credit it in parse
  // order and silence the capture check (see test_ref_capture_body_store).
  auto c1 = [&x] { (void)x; }; // expected-error {{capturing 'x' by reference binds a reference to uninitialized memory under profile 'std::init'}}
  auto c2 = [&] { (void)x; };  // expected-error {{capturing 'x' by reference binds a reference to uninitialized memory under profile 'std::init'}}
  auto c3 = [&ok] { ok = 1; }; // OK: initialized
  int *rtu [[ref_to_uninit]] = &g_uninit;
  auto c4 = [&rtu] { (void)rtu; }; // OK: the pointer object itself is initialized
  int &ur [[ref_to_uninit]] = *rtu;
  auto c5 = [&ur] { (void)ur; }; // expected-error {{capturing 'ur' by reference binds a reference to uninitialized memory under profile 'std::init'}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]] {
    auto c6 = [&x] { (void)x; }; // OK: suppressed
    (void)c6;
  }
  (void)c1; (void)c2; (void)c3; (void)c4; (void)c5;
}

// Parse-order store credit reaches the capture check both ways -- accepted
// leniencies of the scope-less credit map (false negatives only):
void test_ref_capture_after_store() {
  int u [[uninit]];
  u = 5;
  auto c = [&u] { (void)u; }; // OK: the store initialized u (symmetric
                              // with binding &u after the store)
  (void)c;
}

void test_ref_capture_body_store() {
  int u [[uninit]];
  auto L = [&] { u = 5; }; // OK: the body's own store credits u at parse
                           // order, silencing this capture check -- the
                           // deliberate leniency (no FunctionScopeInfo
                           // scoping), pinned here
  int *q = &u;             // OK: credited by the body store above
  (void)L; (void)q;
}

// A by-reference capture of a variable with a non-dependent type fires at
// definition time, and again when TreeTransform's unconditional lambda
// rebuild re-processes the capture at instantiation -- the accepted
// repetition. (The body must not assign x, as in test_ref_captures.)
template <typename T>
void template_ref_capture_bad() {
  int x [[uninit]];
  auto c = [&x] { (void)x; }; // expected-error 2 {{capturing 'x' by reference binds a reference to uninitialized memory under profile 'std::init'}}
  (void)c;
}
template void template_ref_capture_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_ref_capture_bad<int>' requested here}}

struct OpTag {};
OpTag operator+(OpTag, int *p [[ref_to_uninit]]);

struct Assignable {
  Assignable &operator=(int *p [[ref_to_uninit]]);
};

void test_operator_arguments() {
  OpTag t;
  (void)(t + &g_uninit); // OK
  (void)(t + &g_init);   // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}

  Assignable a;
  a = &g_uninit; // OK
  a = &g_init;   // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A non-dependent functor-call argument fires at definition time like every
// other Decl-less binding site, and repeats when the call is rebuilt at
// instantiation (the local functor forces the rebuild).
template <typename T>
void template_functor_bad() {
  MarkedFunctor mf;
  mf(&g_init); // expected-error 2 {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
template void template_functor_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_functor_bad<int>' requested here}}

// Pass-through sources reach the recognizer at the call-argument site too. A
// braced scalar pointer argument additionally warns (braces around scalar
// initializer), so the braced cases here use references; the pointer recognizer
// is exercised at this site by the conditional and comma forms.
void test_passthrough_call_arguments(bool c) {
  take_uninit_ref({g_uninit}); // OK
  take_uninit_ref({g_init});   // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  take_ref({g_uninit});        // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_ref({g_init});          // OK

  take_uninit_ptr(c ? &g_uninit : &g_init); // OK: either arm may be uninitialized
  take_ptr(c ? &g_uninit : &g_init);        // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_uninit_ref(c ? g_uninit : g_uninit2);// OK
  take_ref(c ? g_uninit : g_init);          // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

  take_uninit_ptr((h(), &g_uninit)); // OK
  take_ptr((h(), &g_uninit));        // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_uninit_ref((h(), g_init));    // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  take_ref((h(), g_uninit));         // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

struct Inner { int m; };

// Member access through a [[ref_to_uninit]] pointer denotes uninitialized
// storage. Arrow access (a->m, object *a) and explicit deref ((*a).m) must
// behave identically.
void test_member_through_pointer(Inner *ptr [[ref_to_uninit]]) {
  int *q1 = &ptr->m;                   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *q2 = &(*ptr).m;                 // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *q3 [[ref_to_uninit]] = &ptr->m; // OK
  (void)q1; (void)q2; (void)q3;
}

struct WithFields {
  int *p1 [[ref_to_uninit]] = &g_uninit; // OK
  int *p2 [[ref_to_uninit]] = &g_init;   // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *p3 = &g_uninit;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p4 = &g_init;                      // OK
  int *p5 = nullptr;                      // OK
  int &r1 [[ref_to_uninit]] = g_uninit;  // OK
  int &r2 = g_uninit;                     // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

// [[profiles::suppress]] on a data member must cover its initializer's
// finalization checks, not just the initializer's parsing.
struct WithSuppressedFields {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]] int *p1 = &g_uninit;        // OK: rule-targeted suppress
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] int *p2 = &g_uninit;                                // OK: whole-profile suppress
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] int *p3 [[ref_to_uninit]] = &g_init;                // OK: suppressed (marked target, initialized source)
};

// A suppress on the enclosing record covers its members' initializers.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
struct [[profiles::suppress(std::init)]] WithClassLevelSuppress {
  int *p = &g_uninit; // OK: suppressed by the class-level attribute
};

[[ref_to_uninit]] int *ret_uninit_ptr_ok() { return &g_uninit; }      // OK
[[ref_to_uninit]] int *ret_uninit_ptr_bad() {
  return &g_init; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
int *ret_ptr_bad() {
  return &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
int *ret_ptr_ok() { return &g_init; } // OK

[[ref_to_uninit]] int &ret_uninit_ref_ok() { return g_uninit; } // OK
int &ret_ref_bad() {
  return g_uninit; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Pass-through sources at the return site mirror the variable-init behavior. A
// braced scalar pointer return warns (braces around scalar initializer), so the
// braced returns use references; the pointer recognizer is reached here by the
// conditional and comma forms.
[[ref_to_uninit]] int &ret_braced_ref_ok() { return {g_uninit}; } // OK
int &ret_braced_ref_bad() {
  return {g_uninit}; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
[[ref_to_uninit]] int &ret_braced_ref_bad2() {
  return {g_init}; // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

[[ref_to_uninit]] int *ret_cond_ptr_ok(bool c) { return c ? &g_uninit : &g_init; } // OK
int *ret_cond_ptr_bad(bool c) {
  return c ? &g_uninit : &g_init; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

[[ref_to_uninit]] int *ret_comma_ptr_ok() { return (h(), &g_uninit); } // OK
int *ret_comma_ptr_bad() {
  return (h(), &g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

int *ret_suppressed() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] return &g_uninit; // OK: suppressed
}

// A return inside a lambda binds against the *lambda's own* call operator
// (paper §8.2: the function returning the value must itself be declared
// appropriately), whose [[ref_to_uninit]] marker is spelled in the C++23
// attribute position after the lambda-introducer. A deduced return type is
// resolved before the check runs, so `auto` lambdas are covered too.
// (co_return is not this hook's: it lowers to promise.return_value(e), whose
// argument funnels through the call-argument binding site.)
void test_lambda_return_unmarked() {
  auto explicit_ret = [](int *q [[ref_to_uninit]]) -> int * {
    return q; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  auto deduced_ret = [](int *q [[ref_to_uninit]]) {
    return q; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  auto ref_ret = []() -> int & {
    return g_uninit; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  auto ok = []() -> int * { return &g_init; }; // OK
  (void)explicit_ret; (void)deduced_ret; (void)ref_ret; (void)ok;
}

// The marked call operator is enforced in both directions, enabling the §4.4
// get_uninit() idiom in lambda form.
void test_lambda_return_marked() {
  auto marked = [] [[ref_to_uninit]] (int *q [[ref_to_uninit]]) -> int * {
    return q; // OK: marked operator returning uninitialized memory
  };
  auto marked_bad = [] [[ref_to_uninit]] () -> int * {
    return &g_init; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  };
  (void)marked; (void)marked_bad;
}

// The enclosing function's marker never leaks into a lambda's returns (and
// vice versa): each return is attributed to its own scope's declaration.
[[ref_to_uninit]] int *marked_fn_lambda_isolated() {
  auto inner = []() -> int * {
    return &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  (void)inner;
  return &g_uninit; // OK: the function's own marker
}
int *unmarked_fn_marked_lambda() {
  auto inner = [] [[ref_to_uninit]] () -> int * {
    return &g_uninit; // OK: the lambda's own marker
  };
  (void)inner;
  return &g_init; // OK: the function itself is unmarked
}

// Nested lambdas: the inner return is checked against the inner operator, the
// outer return against the outer one.
void test_nested_lambda_returns() {
  auto outer = [] [[ref_to_uninit]] () -> int * {
    auto inner = []() -> int * {
      return &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    };
    (void)inner;
    return &g_uninit; // OK: the outer operator is marked
  };
  (void)outer;
}

// Suppression covers a lambda return like any other site: the declaration
// statement's parse-time dominion spans the lambda body's tokens.
void test_lambda_return_suppress() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] auto l = []() -> int * {
    return &g_uninit; // OK: suppressed
  };
  auto m = []() -> int * {
    // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
    [[profiles::suppress(std::init)]] return &g_uninit; // OK: suppressed
  };
  (void)l; (void)m;
}

// Parse-order store credit reaches lambda returns like every binding: the
// body's own store precedes the return in parse order, so the credited entity
// is initialized memory and the unmarked return is accepted (the by-ref
// capture is accepted for the same reason, see test_ref_capture_body_store).
void test_lambda_return_after_store() {
  int u [[uninit]];
  auto l = [&]() -> int * {
    u = 5;
    return &u; // OK: credited by the store above
  };
  (void)l;
}

// A generic lambda's call operator is a template pattern: the Decl-carrying
// return check defers via isTemplated and fires when the call operator is
// instantiated -- mirroring the variable-init site (template_nondependent_bad)
// -- so a never-invoked generic lambda's return stays undiagnosed (deferred,
// never instantiated), and an invoked one fires per instantiation.
void test_generic_lambda_return() {
  auto never = [](auto) -> int * { return &g_uninit; }; // OK: never instantiated
  auto invoked = [](auto) -> int * {
    return &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  invoked(0); // expected-note {{in instantiation of function template specialization}}
  (void)never;
}

// A block's return is the same capturing-scope binding with no declaration to
// carry the marker, so it is checked as an unmarked target (like a variadic
// argument), through the same null-target path.
void test_block_return() {
  int *(^b1)() = ^int *() {
    return &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  };
  int *(^b2)() = ^int *() {
    return &g_init; // OK
  };
  (void)b1; (void)b2;
}

template <typename T>
void template_bad() {
  T *p = &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}
template void template_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_bad<int>' requested here}}

// A *non-dependent* pointer bound to uninitialized memory inside a template
// body is diagnosed once, at instantiation, not on the pattern (no double-fire).
template <typename T>
void template_nondependent_bad() {
  int *p = &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}
template void template_nondependent_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_nondependent_bad<int>' requested here}}

// A dependent [[ref_to_uninit]] parameter defers marker validation to
// instantiation (the pattern is accepted); the instantiated parameter carries
// the marker and drives this rule at the call.
template <typename T>
void dependent_marked_fill(T p [[ref_to_uninit]]) { (void)p; }
void test_dependent_marked_param() {
  dependent_marked_fill<int *>(&g_uninit); // OK: marked target, uninit source
  dependent_marked_fill<int *>(&g_init); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A default-initialized new-expression that leaves a scalar subobject
// indeterminate (e.g. new int, new int[n], paper §1.2 / §4.3) is a source of
// uninitialized free-store memory; new T(...), new T{...}, and a type with a
// user-provided default constructor are initialized.
namespace std { enum class byte : unsigned char {}; }

struct NewAgg { int x; };
struct NewWithCtor { NewWithCtor(); int x; };

void test_new_scalar() {
  int *n1 [[ref_to_uninit]] = new int;      // OK
  int *n2 = new int;                         // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *n3 = new int(5);                      // OK
  int *n4 = new int();                       // OK
  int *n5 = new int{};                       // OK
  int *n6 [[ref_to_uninit]] = new int(5);    // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *n7 [[ref_to_uninit]] = new int();     // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)n1; (void)n2; (void)n3; (void)n4; (void)n5; (void)n6; (void)n7;
}

void test_new_array(int n) {
  int *a1 [[ref_to_uninit]] = new int[10];   // OK
  int *a2 = new int[10];                      // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *a3 [[ref_to_uninit]] = new int[n];     // OK
  int *a4 = new int[n];                       // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)a1; (void)a2; (void)a3; (void)a4;
}

void test_new_class() {
  NewWithCtor *c1 = new NewWithCtor;                    // OK: user-provided default ctor trusted
  NewWithCtor *c2 [[ref_to_uninit]] = new NewWithCtor;  // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  NewAgg *a1 = new NewAgg;                               // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  NewAgg *a2 [[ref_to_uninit]] = new NewAgg;            // OK
  std::byte *b1 = new std::byte;                         // OK: std::byte exemption inherited
  std::byte *b2 [[ref_to_uninit]] = new std::byte;      // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)c1; (void)c2; (void)a1; (void)a2; (void)b1; (void)b2;
}

// A record whose only member is a vector is indeterminate after default-init
// (the walk counts vector members like scalar ones), so a raw new of it is a
// source of uninitialized memory.
typedef int nv4 __attribute__((vector_size(16)));
struct NewVecAgg { nv4 v; };
void test_new_vector_member() {
  NewVecAgg *q [[ref_to_uninit]] = new NewVecAgg; // OK
  NewVecAgg *r = new NewVecAgg;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q; (void)r;
}

struct NewInFields {
  int *p1 [[ref_to_uninit]] = new int; // OK
  int *p2 = new int;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p3 = new int(5);                 // OK
};

void test_new_assignment() {
  int *p [[ref_to_uninit]] = new int;
  p = new int;    // OK
  p = new int(5); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *q = new int(0);
  q = new int;    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  q = new int(0); // OK
  (void)p; (void)q;
}

// A written initializer for an *allocated pointer* is itself a binding: the
// heap pointer object cannot carry [[ref_to_uninit]], so it must not be bound
// to uninitialized memory. Both the parenthesized and the braced form are
// checked; copying the value of a marked pointer is the same violation, as at
// variable scope.
void test_new_pointer_init() {
  int **n1 = new (int *)(&g_init);   // OK
  int **n2 = new (int *)(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int **n3 = new (int *){&g_uninit}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *rtu [[ref_to_uninit]] = &g_uninit;
  int **n4 = new (int *)(rtu); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int **n5 [[ref_to_uninit]] = new (int *); // OK: no written initializer -- the
                                            // allocated pointer is indeterminate,
                                            // so the marked target accepts it
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] {
    int **s = new (int *)(&g_uninit); // OK: suppressed
    (void)s;
  }
  (void)n1; (void)n2; (void)n3; (void)n4; (void)n5;
}

// A dependent allocated type defers on the pattern and fires once, at
// instantiation.
template <typename T>
void template_new_pointer_bad() {
  T **p = new (T *)(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}
template void template_new_pointer_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_new_pointer_bad<int>' requested here}}

void test_new_call_arguments() {
  take_uninit_ptr(new int);    // OK
  take_uninit_ptr(new int(5)); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  take_ptr(new int);           // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_ptr(new int(5));        // OK
}

[[ref_to_uninit]] int *ret_new_uninit_ok() { return new int; } // OK
[[ref_to_uninit]] int *ret_new_uninit_bad() {
  return new int(5); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
int *ret_new_ptr_bad() {
  return new int; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
int *ret_new_ptr_ok() { return new int(0); } // OK

void test_new_suppress() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]] int *s = new int; // OK: suppressed
  (void)s;
}

template <typename T>
void template_new_bad() {
  T *p = new T; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}
template void template_new_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_new_bad<int>' requested here}}

// The call-argument, pointer-assignment, and return sites pass no Decl, so
// (unlike the variable-init site, template_nondependent_bad above) they defer
// only on an instantiation-dependent source. These sources are non-dependent,
// so each fires at definition time -- and each construct is rebuilt at
// instantiation anyway (the callee's implicit cast is stripped, forcing a
// call rebuild; the local p is remapped; a return statement always rebuilds),
// so the diagnostic repeats there. The repetition is accepted for now.
template <typename T>
void template_call_arg_unmarked() {
  take_ptr(&g_uninit); // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template void template_call_arg_unmarked<int>(); // expected-note {{in instantiation of function template specialization 'template_call_arg_unmarked<int>' requested here}}

template <typename T>
void template_call_arg_marked() {
  take_uninit_ptr(&g_init); // expected-error 2 {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
template void template_call_arg_marked<int>(); // expected-note {{in instantiation of function template specialization 'template_call_arg_marked<int>' requested here}}

template <typename T>
void template_assignment_bad() {
  int *p = nullptr;
  p = &g_uninit; // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}
template void template_assignment_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_assignment_bad<int>' requested here}}

template <typename T>
int *template_return_bad() {
  return &g_uninit; // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template int *template_return_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_return_bad<int>' requested here}}

// The definition-time fire repeats once per instantiation that rebuilds the
// construct: two explicit instantiations pin the exact counts (one pattern
// fire plus one per specialization).
template <typename T>
void template_assignment_repeats() {
  int *p = nullptr;
  p = &g_uninit; // expected-error 3 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}
template void template_assignment_repeats<int>();  // expected-note {{in instantiation of function template specialization 'template_assignment_repeats<int>' requested here}}
template void template_assignment_repeats<long>(); // expected-note {{in instantiation of function template specialization 'template_assignment_repeats<long>' requested here}}

template <typename T>
int *template_return_repeats() {
  return &g_uninit; // expected-error 3 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template int *template_return_repeats<int>();  // expected-note {{in instantiation of function template specialization 'template_return_repeats<int>' requested here}}
template int *template_return_repeats<long>(); // expected-note {{in instantiation of function template specialization 'template_return_repeats<long>' requested here}}

// An instantiation-dependent source is not checkable on the pattern: no
// definition-time fire. The construct is rebuilt at every instantiation, so
// each violating specialization diagnoses once, with its note chain.
template <typename T>
void template_dependent_per_spec() {
  T *p = nullptr;
  p = (T *)&g_uninit; // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}
template void template_dependent_per_spec<int>();  // expected-note {{in instantiation of function template specialization 'template_dependent_per_spec<int>' requested here}}
template void template_dependent_per_spec<long>(); // expected-note {{in instantiation of function template specialization 'template_dependent_per_spec<long>' requested here}}

// Fully non-dependent constructs whose operands transform to themselves are
// *reused* by TreeTransform at instantiation -- their Build* never re-runs.
// Deferring would silently lose the diagnostic (these all-global shapes were
// silent before), so they are checked at definition time: exactly one error,
// on the pattern, with no instantiation note.
int *g_ptr_sink = nullptr;
int **g_pp_sink = nullptr;

template <typename T>
void template_allglobal_bad() {
  g_ptr_sink = &g_uninit;             // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  throw &g_uninit;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  g_pp_sink = new (int *)(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template void template_allglobal_bad<int>();

// The same shapes diagnose in a never-instantiated template: definition-time
// checking deliberately trades strict "as-if after phase 7" purity for
// reuse-proof diagnostics.
template <typename T>
void template_allglobal_never_instantiated() {
  g_ptr_sink = &g_uninit;             // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  throw &g_uninit;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  g_pp_sink = new (int *)(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A never-instantiated template diagnoses its non-dependent violations at
// definition time; only instantiation-dependent constructs (the (T *) cast)
// stay silent without an instantiation.
template <typename T>
void template_never_instantiated() {
  take_ptr(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p = nullptr;
  p = &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  p = (T *)&g_uninit;
  (void)p;
}

// A value-dependent if-constexpr condition is not yet known discarded at the
// pattern, so the branch is live at parse and its non-dependent violations
// diagnose at definition time. The f<int> instantiation discards the branch
// (never rebuilding its statements), so nothing repeats.
template <typename T>
void template_discarded_branch() {
  if constexpr (sizeof(T) > 1000) {
    take_ptr(&g_uninit); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    int *p = nullptr;
    p = &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    (void)p;
  }
}
template void template_discarded_branch<int>();

// A generic lambda's body is a template pattern even in a non-template
// function: a non-dependent violation diagnoses at definition time whether or
// not the lambda is ever invoked, and an all-global shape is reused (not
// rebuilt) when the call operator is instantiated, so invoking does not
// repeat it.
void generic_lambda_never_invoked() {
  auto l = [](auto x) { g_ptr_sink = &g_uninit; }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)l;
}

void generic_lambda_invoked() {
  auto l = [](auto x) { g_ptr_sink = &g_uninit; }; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  l(1);
}

// A late-parsed inline member of a class template is a pattern too: a
// non-dependent violation diagnoses when its body is parsed. Suppression on
// the method or on the class covers the definition-time fire like any other.
template <typename T>
struct LateParsedMember {
  void m() { g_ptr_sink = &g_uninit; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

template <typename T>
struct LateParsedSuppressMethod {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] void m() { g_ptr_sink = &g_uninit; } // OK: suppressed
};

template <typename T>
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
struct [[profiles::suppress(std::init)]] LateParsedSuppressClass {
  void m() { g_ptr_sink = &g_uninit; } // OK: suppressed
};

// std::init / uninit_read (paper §4.5): a read *through* a [[ref_to_uninit]]
// pointer or reference yields an uninitialized value, diagnosed at the
// lvalue-to-rvalue conversion (Sema::DefaultLvalueConversion). These reads fire
// only under -fprofiles; the no-profiles run stays clean. A direct read of a
// named [[uninit]] object is left to the flow-based uninit_read pass and is not
// retested here.
void take_value(int v);

int test_read_through_pointer(int *p [[ref_to_uninit]], int i) {
  int y1 = *p;     // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y2 = p[i];   // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y3 = *p + 1; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  take_value(*p);  // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  if (*p)          // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
    h();
  (void)y1; (void)y2; (void)y3;
  return *p;       // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

void test_read_through_reference(int &r [[ref_to_uninit]], Inner *ptr [[ref_to_uninit]],
                                 void *vp [[ref_to_uninit]]) {
  int y1 = r;                // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y2 = ptr->m;           // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y3 = (*ptr).m;         // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y4 = *(int *)vp;       // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y5 = get_uninit_ref(); // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)y1; (void)y2; (void)y3; (void)y4; (void)y5;
}

// Paper §4.5: reading an uninitialized std::byte is permitted, so a read
// through a [[ref_to_uninit]] std::byte pointer/reference is not diagnosed.
void test_read_byte_exempt(std::byte *bp [[ref_to_uninit]], std::byte &br [[ref_to_uninit]]) {
  std::byte b1 = *bp; // OK
  std::byte b2 = br;  // OK
  (void)b1; (void)b2;
}

void test_read_suppress(int *p [[ref_to_uninit]]) {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { take_value(*p); }                      // OK: whole-profile suppress
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_read")]] { take_value(*p); } // OK: rule-targeted suppress
}

// The suppression dominion of a member declaration includes its initializer
// tokens (P3589R2 s2.4p3). The read-through check fires from
// ActOnFinishCXXInClassMemberInitializer during the late parse of an NSDMI,
// so a suppression on the field (or, via the lexical parent walk, on the
// class) must cover it; a sibling member's suppression must not leak.
int *nsdmi_rtu [[ref_to_uninit]] = &g_uninit;

struct NsdmiReadSuppress {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] int x = *nsdmi_rtu; // OK: field suppress
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_read")]] int y = *nsdmi_rtu; // OK: rule-targeted
  int z = *nsdmi_rtu; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
};

// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
struct [[profiles::suppress(std::init)]] NsdmiClassReadSuppress {
  int x = *nsdmi_rtu; // OK: class-level suppression via the lexical parent walk
};

// None of these is a read through the marker: a discarded-value expression and
// an address-of apply no lvalue-to-rvalue conversion, a write targets the
// glvalue without loading it, a reference binding is not a load, and copying
// the pointer value reads the (initialized) pointer object rather than through
// it.
void test_read_negatives(int *p [[ref_to_uninit]], int &r [[ref_to_uninit]],
                         Inner *ptr [[ref_to_uninit]], int *base [[ref_to_uninit]]) {
  (void)r;                         // OK: discarded value
  (void)*p;                        // OK: discarded value
  int *ap [[ref_to_uninit]] = &*p; // OK: address-of is not a read
  int &r2 [[ref_to_uninit]] = *p;  // OK: reference binding, not a read
  *p = 5;                          // OK: write, not a read (and it credits
                                   // p's pointee, so it stays after the
                                   // marked bindings above)
  // A subobject write is not a read either -- the error below is
  // uninit_write's (the piecemeal ban), not uninit_read's.
  ptr->m = 5;                      // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
  int *q [[ref_to_uninit]] = base; // OK: reads the pointer value, not through it
  (void)ap; (void)q; (void)r2;
}

// A subobject read of a named [[uninit]] object loads an uninitialized value,
// exactly like a read through a [[ref_to_uninit]] pointer: member-wise delayed
// initialization of an [[uninit]] object is banned (paper §5.4), so no
// assignment could have given the member a value. Only the *whole-object*
// direct read of a named [[uninit]] entity is left to the flow-based
// uninit_read pass (which credits assignments).
struct Pair { int x; int y; };
struct PairHolder { Pair p; };

void test_member_read_of_uninit_object() {
  Pair s [[uninit]];
  int y1 = s.x;    // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  take_value(s.y); // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  PairHolder o [[uninit]];
  int y2 = o.p.x;  // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  // The arrow spelling reaches the member through a pointer, so the
  // diagnostic's phrasing approximation picks the pointer wording; the read is
  // diagnosed all the same.
  int y3 = (&s)->x; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)y1; (void)y2; (void)y3;
}

// Discarded values and address-taking apply no lvalue-to-rvalue conversion
// and are not reads; taking the member's address is the binding checks'
// territory (unchanged behavior, retested as a regression guard). A write is
// not a read either, but a subobject store of an [[uninit]] object is itself
// banned as delayed initialization (uninit_write; full coverage in
// safety-profile-init-write.cpp).
void test_member_read_negatives() {
  Pair s [[uninit]];
  s.x = 1;                         // expected-error {{writing a member of an '[[uninit]]' object does not initialize it under profile 'std::init'; initialize the whole object}}
  (void)s.x;                       // OK: discarded value
  int *p = &s.x;                   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *q [[ref_to_uninit]] = &s.x; // OK: binding checked by ref_to_uninit
  (void)p; (void)q;
}

// A non-field member reached through an [[uninit]] object is not a subobject
// of it: a static data member is zero-initialized static storage, so its
// address is initialized memory regardless of the base object's state -- an
// unmarked binding is fine and a marked one is rejected, exactly as if the
// member were named via the class (`&S::sm` parity). A marked static
// *reference* member still classifies by its own marker (its referent is the
// uninitialized memory), and copying a marked static *pointer* member is an
// ordinary marked-pointer copy.
struct WithStaticMembers {
  static int sm;
  [[ref_to_uninit]] static int &sr;
  [[ref_to_uninit]] static int *sp;
  int x;
};
void test_static_member_address_of_uninit_object() {
  WithStaticMembers s [[uninit]];
  int *p1 = &s.sm;                               // OK: static storage, not a subobject
  int *p2 [[ref_to_uninit]] = &s.sm;             // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *p3 = &WithStaticMembers::sm;              // OK: same classification as &s.sm
  int *p4 [[ref_to_uninit]] = &WithStaticMembers::sm; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *p5 [[ref_to_uninit]] = &s.sr;             // OK: the marked reference's referent is uninitialized
  int *p6 = &s.sr;                               // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *p7 [[ref_to_uninit]] = s.sp;              // OK: copy of a marked pointer
  int *p8 = s.sp;                                // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p1; (void)p3; (void)p5; (void)p7;
}

void test_member_read_suppress() {
  Pair s [[uninit]];
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_read")]] { take_value(s.x); } // OK
}

// std::byte members stay exempt (paper §4.5).
struct WithByte { std::byte b; int i; };
void test_member_read_byte_exempt() {
  WithByte s [[uninit]];
  std::byte b = s.b; // OK
  (void)b;
}

// A whole-record copy from *pp reads the uninitialized pointee (paper
// abstract: an object marked [[ref_to_uninit]] cannot be read through), but
// class types never reach the lvalue-to-rvalue chokepoint. The copy is caught
// all the same, by the binding rule at the copy constructor's reference
// parameter -- so the diagnostic is the binding one, not the read-through
// one. This holds for copy-, direct-, braced-, argument-, and return-copies
// alike.
void take_pair(Pair v);

Pair test_record_copy_through(Pair *pp [[ref_to_uninit]]) {
  Pair v = *pp;   // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  Pair w(*pp);    // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  Pair b = {*pp}; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_pair(*pp); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)v; (void)w; (void)b;
  return *pp;     // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// The escape is the paper's own (§7.2): a copy constructor declared with a
// [[ref_to_uninit]] parameter accepts the uninitialized source -- and then,
// symmetrically, rejects an initialized one.
struct MarkedCopy {
  int x;
  MarkedCopy();
  MarkedCopy(const MarkedCopy &q [[ref_to_uninit]]);
};

void test_record_copy_marked_ctor(MarkedCopy *mp [[ref_to_uninit]],
                                  const MarkedCopy &init) {
  MarkedCopy v = *mp;  // OK: the marked parameter accepts the uninit pointee
  MarkedCopy w = init; // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)v; (void)w;
}

// A record *containing* std::byte members is not std::byte, so the §4.5 read
// exemption does not extend to the whole-record binding.
struct ByteBox { std::byte b; };
void test_record_copy_byte_member(ByteBox *bp [[ref_to_uninit]]) {
  ByteBox v = *bp; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)v;
}

// An element read of a named [[uninit]] array is a subobject read exactly like
// s.x: neither flow pass tracks array elements (and element-wise delayed
// initialization is banned, paper §5.5), so the marker counts below the
// element access even for a read. *a denotes the same element as a[0].
void test_element_read_of_uninit_array(int i) {
  [[uninit]] int a[2];
  int y1 = a[0];    // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  take_value(a[i]); // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  int y2 = *a;      // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  [[uninit]] int m[2][2];
  int y3 = m[1][0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y1; (void)y2; (void)y3;
}

// An [[uninit]] array *member*'s element read is flagged the same way: like
// the class-type member in HasAggMember below, an array member has no legal
// element-wise assignment path, so the marker counts even on the current
// object.
struct WithArrMember {
  [[uninit]] int a[2];
  int get() { return a[0]; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
};

// Discarded values and address-taking apply no lvalue-to-rvalue conversion
// and are not reads; bindings to the array or its elements stay the
// ref_to_uninit checks' territory (regression guards, unchanged behavior). An
// element store is not a read either, but is itself banned as delayed
// initialization (uninit_write; full coverage in
// safety-profile-init-write.cpp).
void test_element_read_negatives(int i) {
  [[uninit]] int a[2];
  a[0] = 1;                         // expected-error {{writing an element of an '[[uninit]]' object does not initialize it under profile 'std::init'; initialize the whole object}}
  (void)a[0];                       // OK: discarded value
  int *p [[ref_to_uninit]] = &a[0]; // OK: address-of is not a read; binding checked by ref_to_uninit
  int *q [[ref_to_uninit]] = a;     // OK: array decay is a binding, not a read
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_read")]] { take_value(a[i]); } // OK: rule-targeted suppress
  (void)p; (void)q;
}

// std::byte arrays stay exempt (paper §4.5).
void test_element_read_byte_exempt() {
  [[uninit]] std::byte b[2];
  std::byte v = b[0]; // OK
  (void)v;
}

// The dependent element type makes the read instantiation-dependent, so it
// defers on the pattern and fires once, at instantiation.
template <typename T>
void template_element_read_bad() {
  [[uninit]] T a[2];
  int y = a[0]; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}
template void template_element_read_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_element_read_bad<int>' requested here}}

// The §5.2 trust pattern is preserved: a scalar [[uninit]] *member* of the
// current object may be assigned in the constructor body (flow-checked by the
// ctor-body pass), so a member-function read of it is not flagged here.
struct BodyInit {
  int m [[uninit]];
  BodyInit() { m = 1; }
  int get() { return m; } // OK: trusted, assigned in the constructor body
};

// But a subobject of an [[uninit]] *class-type member* has no legal
// assignment path (member-wise delayed initialization is banned, and
// construct_at flow is uniformly unmodeled), so its read is flagged even on
// the current object.
struct HasAggMember {
  Pair agg [[uninit]];
  int get() { return agg.x; } // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
};

// Like every Decl-less check, the member read fires at definition time when
// its glvalue is non-dependent, and repeats when the read is rebuilt at
// instantiation (the local s is remapped) -- the accepted repetition.
template <typename T>
void template_member_read_bad() {
  Pair s [[uninit]];
  int y = s.x; // expected-error 2 {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}
template void template_member_read_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_member_read_bad<int>' requested here}}

// A read through a [[ref_to_uninit]] parameter inside a template body fires at
// definition time when the operand is non-dependent
// (template_read_nondependent_bad; the parameter remap rebuilds the read at
// instantiation, repeating the diagnostic) and defers to instantiation when it
// is dependent (template_read_dependent_bad). Mirrors the binding template_*
// cases above.
template <typename T>
void template_read_nondependent_bad(int *p [[ref_to_uninit]]) {
  int y = *p; // expected-error 2 {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}
template void template_read_nondependent_bad<int>(int *); // expected-note {{in instantiation of function template specialization 'template_read_nondependent_bad<int>' requested here}}

template <typename T>
T template_read_dependent_bad(T *p [[ref_to_uninit]]) {
  return *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}
template int template_read_dependent_bad<int>(int *); // expected-note {{in instantiation of function template specialization 'template_read_dependent_bad<int>' requested here}}

// A never-instantiated pattern diagnoses its non-dependent read at definition
// time, exactly once.
template <typename T>
void template_read_never_instantiated(int *p [[ref_to_uninit]]) {
  int y = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}

// An all-global read: the definition-time fire, plus a repeat when the
// initialization of the local y is rebuilt at instantiation.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "static_marker")]] [[uninit]] Pair g_uninit_pair;

template <typename T>
void template_global_read_bad() {
  int y = g_uninit_pair.x; // expected-error 2 {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}
template void template_global_read_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_global_read_bad<int>' requested here}}

template <typename T>
void template_global_read_never_instantiated() {
  int y = g_uninit_pair.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}

// A compound assignment and a built-in ++/-- read the old value before
// storing, but build no lvalue-to-rvalue node for the operand, so the
// operator sites check the load directly. Every compound form through a
// [[ref_to_uninit]] pointer or reference is diagnosed; the shift forms load
// through their LHS promotion instead and must fire exactly once. Reading an
// unmarked pointer's pointee is trusted, and ++ on the marked pointer itself
// reads the (initialized) pointer object, not through it. Each form gets a
// fresh marker: a compound form both reads (the error) and stores, and the
// store credits the pointee for everything after it in parse order (see
// test_pointee_store_credit) -- except through an element access, which
// never sees the credit (p[i] below, after *p's store; paper §5.4).
void test_compound_read_through(int *p [[ref_to_uninit]], int *q,
                                int &r [[ref_to_uninit]],
                                Inner *ptr [[ref_to_uninit]], int i,
                                int *p2 [[ref_to_uninit]],
                                int *p3 [[ref_to_uninit]],
                                int &r2 [[ref_to_uninit]],
                                int *p4 [[ref_to_uninit]]) {
  *p += 1;     // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  p[i] -= 1;   // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  r *= 2;      // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  ptr->m |= 1; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}} \
               // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
  ++*p2;       // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (*p3)--;     // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  ++r2;        // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  *p4 <<= 1;   // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  *q += 1;     // OK: an unmarked pointer is trusted initialized
  ++p;         // OK: reads the pointer object itself, not through it
}

void test_compound_read_suppress(int *p [[ref_to_uninit]]) {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_read")]] { *p += 1; } // OK: rule-targeted suppress
}

// Like every Decl-less read check, the compound read fires at definition time
// on a non-dependent operand and repeats when the parameter remap rebuilds the
// assignment at instantiation.
template <typename T>
void template_compound_read_bad(int *p [[ref_to_uninit]]) {
  *p += 1; // expected-error 2 {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}
template void template_compound_read_bad<int>(int *); // expected-note {{in instantiation of function template specialization 'template_compound_read_bad<int>' requested here}}

// Parse-order pointee store credit (paper §4.3/§4.5): a whole-`*p` store
// through a [[ref_to_uninit]] pointer is the pointee's initialization, so
// whole-`*p` accesses after it (in parse order) are legal -- and the paper's
// reverse direction applies: the credited pointer now refers to initialized
// memory and REQUIRES an unmarked target. Element accesses never see the
// credit in either direction (§5.4's random-access ban), and reseating the
// pointer clears it.
void test_pointee_store_credit(int *p [[ref_to_uninit]]) {
  *p = 5;      // OK: the write initializes the pointee (and credits it)
  *p = 7;      // OK: further whole-entity stores stay legal
  int x = *p;  // OK: credited (rejected before the store)
  int *r2 [[ref_to_uninit]] = p; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)x; (void)r2;
}

void test_pointee_read_before_store(int *p [[ref_to_uninit]]) {
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  *p = 5;
  (void)x;
}

// The credit is recorded at the tail of the assignment, after the RHS is
// checked: a self-assignment's RHS read must not be silenced by its own
// store (the key recording-order regression test).
void test_pointee_no_self_credit(int *p [[ref_to_uninit]]) {
  *p = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
}

// Element stores neither credit nor invalidate (§5.4/§5.5: element-wise
// state is untrackable by design)...
void test_subscript_store_no_credit(int *p [[ref_to_uninit]]) {
  p[0] = 1;
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x;
}

// ...and element reads never see pointee credit: `*p = 5;` must not legalize
// p[1] (the pointee may be an array with only element 0 written). The model
// is purely syntactic, so even p[0] -- the same storage as *p -- stays an
// error: only the whole-`*p` form is credited.
void test_subscript_read_not_credited(int *p [[ref_to_uninit]], int i) {
  *p = 5;
  int x = p[i]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  int y = p[0]; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x; (void)y;
}

// Reseating the pointer -- plain assignment, compound arithmetic, or ++ --
// clears its pointee credit: the credit described the old pointee.
void test_reseat_clears_credit(int *p [[ref_to_uninit]],
                               int *q [[ref_to_uninit]], int n) {
  *p = 5;
  p = q;
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  *p = 5;
  p += n;
  int y = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  *p = 5;
  p++;
  int z = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x; (void)y; (void)z;
}

// A store through a transparent cast credits (and reseats) exactly like its
// uncast form, symmetric with the recognizers' cast pass-through (§4.3): a
// cast does not launder the store any more than it launders the marking.
void test_cast_store_credits_whole() {
  int u [[uninit]];
  (int &)u = 5;
  int *q = &u; // OK: the cast store credited u whole
  (void)q;
}
void test_cast_store_fires_reverse() {
  int u [[uninit]];
  (int &)u = 5;
  int *m [[ref_to_uninit]] = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)m;
}
void test_cast_deref_store_credits_pointee(int *p [[ref_to_uninit]]) {
  *(int *)p = 5;
  int v = *p; // OK: the cast deref store credited the pointee
  (void)v;
}
void test_cast_reseat_clears(int *p [[ref_to_uninit]],
                             int *q [[ref_to_uninit]]) {
  *p = 5;
  (int *&)p = q; // OK both sides: p's own reseat, judged by p's marker
  int v = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)v;
}

// Handing out a *mutable alias* of the marked pointer object (T*& or T**)
// lets the holder reseat it, so the escape withdraws the Definite pointee
// credit -- the firing basis of the marked-target diagnostic -- while the
// suppressing Maybe credit survives (the callee may equally leave the
// pointer alone): the same "revoke only the firing strength" semantics as
// a conditional [[now_uninit]] destroy.
void alias_by_ref(int *&);
void alias_by_ptr(int **);
void alias_by_const_ref(int *const &);
void test_alias_escape_by_reference(int *p [[ref_to_uninit]]) {
  *p = 5;
  alias_by_ref(p);
  int *m [[ref_to_uninit]] = p; // OK: the callee may have reseated p
  int *u2 = p;                  // OK: the Maybe credit still suppresses
  (void)m; (void)u2;
}
void test_alias_escape_by_pointer(int *p [[ref_to_uninit]]) {
  *p = 5;
  alias_by_ptr(&p);
  int *m [[ref_to_uninit]] = p; // OK: the callee may have reseated p
  (void)m;
}
void test_const_alias_does_not_withdraw(int *p [[ref_to_uninit]]) {
  *p = 5;
  alias_by_const_ref(p);
  int *m [[ref_to_uninit]] = p; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)m;
}
void test_alias_escape_declaration_form(int *p [[ref_to_uninit]]) {
  *p = 5;
  int **pp = &p;
  int *m [[ref_to_uninit]] = p; // OK: pp can reseat p
  (void)pp; (void)m;
}

// The *pointee* credit map keys on local VarDecls, so a [[ref_to_uninit]]
// *member* pointer is never credited: a read through it keeps failing even
// after a store through the exact same lvalue. The per-object *whole-member*
// credit below deliberately does not extend here: pointee aliasing is
// per-value, not per-object (a copy of the object shares the pointee), so
// crediting `(w, p)` would be unsound the moment w is copied.
struct WithMarkedPtrField {
  int *p [[ref_to_uninit]] = &g_uninit;
};
void test_member_pointer_never_credited(WithMarkedPtrField w) {
  *w.p = 5;
  int x = *w.p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x;
}

// A member store through a marked class-typed pointer is a subobject store:
// rejected by uninit_write (the piecemeal ban; whole-object construct_at is
// the remedy) and never crediting, so the member read after it still fails.
void test_member_store_never_credits(Inner *ptr [[ref_to_uninit]]) {
  ptr->m = 5; // expected-error {{writing a member of uninitialized storage reached through a '[[ref_to_uninit]]' pointer or reference does not initialize it under profile 'std::init'; initialize the whole object ('construct_at()' for a class object)}}
  int y = ptr->m; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)y;
}

// Whole-pointee assignment of a CLASS pointee never reaches the built-in
// assignment funnel: it resolves to the member operator=, whose implicit
// object parameter binds the uninitialized pointee -- the object-argument
// rule's rejection, for the element form too. The sanctioned class-pointee
// initialization is construct_at.
void test_class_pointee_assignment(Pair *p [[ref_to_uninit]]) {
  *p = Pair{1, 2}; // expected-error {{calling member function 'operator=' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}
void test_class_element_assignment(Pair *p [[ref_to_uninit]]) {
  p[2] = Pair{1, 2}; // expected-error {{calling member function 'operator=' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}
// An explicit-object (deducing-this) operator= declares its object as
// parameter 0, so the same shape is rejected by the parameter-binding
// funnel instead: the unmarked `this EPair &self` parameter must not bind
// uninitialized memory.
struct EPair {
  int x;
  int y;
  EPair &operator=(this EPair &self, const EPair &o);
};
void test_explicit_object_assignment(EPair *p [[ref_to_uninit]]) {
  *p = EPair{1, 2}; // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Per-object whole-member store credit (paper §4.2: "After initialization,
// the object is no longer [[uninit]]"; §6: ordinary assignment initializes a
// built-in): `a.m = 5` credits exactly the (base object, member) pair, so a
// later binding of that member through the same base is legal -- the member
// analog of the locals credit above. The base identity is the directly named
// local-storage variable or, for current-object accesses (this->m / m), the
// enclosing function declaration, so unrelated objects and other function
// bodies never share credit. Same parse-order semantics: no dominance or
// flow analysis, missed diagnostics only.
struct MemberCredit { int m [[uninit]]; }; // expected-note {{member 'm' declared here}}
void mc_sink_ptr(int *);
void mc_sink_ref(const int &);
void mc_fill(int *p [[ref_to_uninit]]);

void test_member_store_credit() {
  MemberCredit a;
  mc_sink_ref(a.m); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  mc_sink_ptr(&a.m); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  a.m = 5;
  mc_sink_ref(a.m);  // OK: credited
  mc_sink_ptr(&a.m); // OK: credited
  int *q = &a.m;     // OK: credited
  (void)q;
}

// The reverse direction applies too (mirroring test_assign_credited_to_marked):
// a credited member is initialized memory and now requires an unmarked target.
void test_member_credit_reverse_direction() {
  MemberCredit a;
  a.m = 5;
  mc_fill(&a.m); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// The credit is per base object: a store to one local's member says nothing
// about another local of the same type (and per §5.2, nothing about a copy).
void test_member_credit_per_object() {
  MemberCredit a1, a2;
  a1.m = 5;
  mc_sink_ref(a1.m); // OK: credited
  mc_sink_ref(a2.m); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Current-object members key on the enclosing function: `m = 5` in one member
// function never credits a binding in another, `this->m` / `m` / `(*this).m`
// share the key within one body, and a this-capturing lambda's body is its
// own function (its stores and the enclosing function's do not mix, in
// either direction).
struct ThisMemberCredit {
  int m [[uninit]];
  void store_then_bind() {
    this->m = 5;
    mc_sink_ref(m);          // OK: credited (same key as this->m)
    mc_sink_ptr(&(*this).m); // OK: credited
  }
  void bind_without_store() {
    mc_sink_ref(m); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  }
  void lambda_store_isolated() {
    auto l = [this] { m = 5; }; // records under the lambda's own key
    mc_sink_ptr(&m); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    (void)l;
  }
  void lambda_bind_isolated() {
    m = 5; // records under this function's key
    auto l = [this] {
      mc_sink_ptr(&m); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    };
    (void)l;
  }
};

// In a constructor, the parse-time binding after a whole-member store is
// credited the same way (this was the escape-adjacent false positive); the
// CFG ctor-body pass independently keeps governing *reads*, with real flow
// analysis (safety-profile-init-ctor-body.cpp).
struct CtorMemberCredit {
  int m [[uninit]];
  CtorMemberCredit() {
    m = 5;
    mc_sink_ptr(&m); // OK: credited by the store above
  }
  CtorMemberCredit(int) {
    mc_sink_ptr(&m); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
    m = 5;
  }
};

// Compound assignment and ++/-- record through the same arm: the store side
// credits later bindings (the read side of `a.m += 1` on a local aggregate is
// the CFG local-members pass's, which flags it with flow precision).
void test_member_credit_compound() {
  MemberCredit a;
  a.m += 1; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
  mc_sink_ref(a.m); // OK: the compound store credited (a, m)
}

// Only a single-level, directly named base earns or consults credit. A store
// below a marked class-type member (x.agg.m) is itself the banned piecemeal
// initialization (uninit_write, §5.4) and resolves no base, so it earns
// nothing -- the later binding still sees agg's marker below top level. A
// whole-member `x.agg = ...` cannot credit either: a class-typed assignment
// is a member operator= call on uninitialized storage, rejected outright.
struct AggMemberCredit { MemberCredit agg [[uninit]]; };
void test_member_credit_single_level() {
  AggMemberCredit x;
  x.agg.m = 5; // expected-error {{writing a member of an '[[uninit]]' object does not initialize it under profile 'std::init'; initialize the whole object}}
  mc_sink_ptr(&x.agg.m); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  x.agg = MemberCredit(); // expected-error {{calling member function 'operator=' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}

// A member of an object reached through a reference or pointer parameter is
// the pinned aliasing boundary: the store is trusted as a write, but the
// binding through that alias stays strict.
void test_member_credit_alias_boundary(MemberCredit &r, MemberCredit *p) {
  r.m = 5;
  mc_sink_ref(r.m); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  p->m = 5;
  mc_sink_ptr(&p->m); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Credit is recorded at pattern-parse time and independently at instantiation
// (fresh field and function declarations), so the store-then-bind sequence
// holds in templates too; the dependent binding defers on the pattern and
// re-runs on the rebuilt (credited-in-order) instantiation.
template <typename T>
struct TmplMemberCredit {
  int m [[uninit]];
  void f() {
    this->m = 5;
    mc_sink_ref(this->m); // OK at the pattern and at instantiation
  }
};
template struct TmplMemberCredit<int>;

// TreeTransform hands a fully non-dependent statement back *unchanged* when
// instantiating a body, so `m = 5` inside a generic lambda runs Sema (and
// records its credit) only at the pattern parse. The current-object key is
// the function's parse-time pattern on record and consult alike, so the
// instantiated call operators' checks still find that credit -- while the
// per-function isolation above is unchanged (different functions have
// different patterns).
struct GenericLambdaMemberCredit {
  int m [[uninit]];
  void go() {
    auto l = [this](auto) {
      m = 5;
      mc_sink_ref(m);  // OK: credited at the pattern parse and per instantiation
      mc_sink_ptr(&m); // OK
    };
    l(1);
    l(2L);
  }
};

// The same statement reuse through a non-generic lambda in a member function
// template: the transformed call operator reaches its parsed pattern through
// a member-specialization link.
struct LambdaInMemberTemplateCredit {
  int m [[uninit]];
  template <class T> void go() {
    auto l = [this] { m = 5; mc_sink_ref(m); }; // OK
    l();
  }
};
template void LambdaInMemberTemplateCredit::go<int>();

// [[now_init]] (P4222R2 §6.2): binding an entity to a [[ref_to_uninit]]
// parameter of a [[now_init]] function earns the same parse-order credit as
// the equivalent direct store -- the callee initializes the storage it was
// handed. A plain (non-[[now_init]]) callee still earns nothing (the strict
// no-escape-credit doctrine; the paper reserves callee-initialization for
// exactly this annotation). Inside constructors the CFG ctor-body pass has
// its own flow-precise [[now_init]] credit (safety-profile-init-ctor-body.cpp).
[[now_init]] void now_init_fill(int *p [[ref_to_uninit]]);
[[now_init]] void now_init_fill_ref(int &r [[ref_to_uninit]]);
[[now_init]] void now_init_variadic(int *p [[ref_to_uninit]], ...);
void ni_sink(int *);
void ni_cref(const int &);

void test_now_init_whole_local() {
  int u [[uninit]];
  ni_sink(&u);       // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  now_init_fill(&u); // OK: marked target, uninitialized source
  ni_sink(&u);       // OK: the callee initialized u
  ni_cref(u);        // OK
  now_init_fill(&u); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// A plain callee taking the same marked parameter earns no credit: the
// binding after it stays the strict error.
void plain_fill(int *p [[ref_to_uninit]]);
void test_plain_callee_no_credit() {
  int u [[uninit]];
  plain_fill(&u);
  ni_sink(&u); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// Passing the marked pointer's *value* is §6.2's initialize2(p) example
// verbatim: the callee initializes the pointee, so whole-`*p` accesses after
// the call are legal -- composed with the existing reseat machinery, which
// clears the credit like any other pointee credit.
void test_now_init_pointee(int *p [[ref_to_uninit]]) {
  int before = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  now_init_fill(p); // OK: marked-to-marked binding
  int v = *p;       // OK: the callee initialized the pointee
  ni_sink(p);       // OK: p now refers to initialized memory
  (void)before; (void)v;
}

void test_now_init_reseat(int *p [[ref_to_uninit]], int *q [[ref_to_uninit]]) {
  now_init_fill(p);
  p = q;      // reseating clears the pointee credit
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x;
}

void test_now_init_reference(int &r [[ref_to_uninit]]) {
  now_init_fill_ref(r); // OK: marked reference onward to a marked parameter
  int v = r;            // OK: the referent is initialized (never lapses)
  (void)v;
}

// Members earn the same per-object credit as a direct member store, under
// the same base-identity keys and boundaries: a member of a parameter-
// reached object stays uncredited (the pinned aliasing boundary).
void test_now_init_member() {
  MemberCredit a;
  now_init_fill(&a.m); // OK
  mc_sink_ptr(&a.m);   // OK: (a, m) credited by the callee
  mc_sink_ref(a.m);    // OK
}
void test_now_init_member_boundary(MemberCredit &r) {
  now_init_fill(&r.m); // OK: marked target, uninitialized source
  mc_sink_ptr(&r.m);   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// The callee credit survives statement reuse in instantiated lambda bodies
// exactly like a direct store (the current-object key is the parse-time
// pattern; see GenericLambdaMemberCredit above).
struct GenericLambdaNowInitMemberCredit {
  int m [[uninit]];
  void go() {
    auto l = [this](auto) {
      now_init_fill(&m);
      mc_sink_ptr(&m); // OK: credited at the pattern parse and per instantiation
    };
    l(1);
  }
};

// A variadic argument reaches no declared parameter, so it earns nothing
// even from a [[now_init]] callee (and is checked as an unmarked target).
void test_now_init_variadic_no_credit() {
  int u [[uninit]];
  int w [[uninit]];
  now_init_variadic(&u, &w); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  ni_sink(&u); // OK: the marked parameter credited u
  ni_sink(&w); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// [[now_init]] on a function template: a call to a concrete [[now_init]]
// callee credits at pattern-parse time, so definition-time checks find it; a
// dependent callee defers with the rest of the call and credits per
// instantiation, in the rebuilt parse order.
template <typename T>
[[now_init]] void now_init_tmpl(T *p [[ref_to_uninit]]);

template <typename T>
void template_now_init_nondependent() {
  int u [[uninit]];
  now_init_fill(&u);
  ni_sink(&u); // OK at definition time and at instantiation
}
template void template_now_init_nondependent<int>();

template <typename T>
void template_now_init_dependent() {
  T u [[uninit]];
  now_init_tmpl(&u);
  T *s = &u; // OK per instantiation: the rebuilt call credits first
  (void)s;
}
template void template_now_init_dependent<int>();

// R2 §4.4's now_init() library function works today as a *pure declaration*
// with no compiler support at all: its unmarked return classifies as
// initialized (trusted, §4.3), so the returned pointer launders the storage
// -- the paper's own deliberate profile hole (its `return p;` definition is
// written once, under suppression). What the declaration alone cannot do is
// legalize the *original name* after the call; that is exactly what the §6.2
// [[now_init]] attribute adds when placed on the same declaration.
template <class T> T *now_init(T *p [[ref_to_uninit]]);
template <class T> [[now_init]] T *now_init_annotated(T *p [[ref_to_uninit]]);

void test_now_init_library_pattern() {
  int m [[uninit]];
  int v = *now_init(&m); // OK today: the unmarked return is trusted
  int *s = now_init(&m); // OK: unmarked pointer from an unmarked return
  ni_cref(m); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)v; (void)s;
}
void test_now_init_library_pattern_annotated() {
  int m [[uninit]];
  int *s = now_init_annotated(&m); // OK
  ni_cref(m); // OK: the attribute legalizes the original name
  (void)s;
}

// R2 §4.5's requested library annotation, verbatim: construct_at takes a
// [[ref_to_uninit]] pointer and returns a pointer to an initialized object.
// As a [[now_init]]-annotated declaration the lifecycle *start* works: the
// argument's whole object is credited and a repeated construct_at is even
// caught by the reverse-direction rule (a credited source no longer refers
// to uninitialized memory). The rest of the §4.5 lifecycle -- destroy_at,
// use-after-destroy -- remains future work.
template <class T, class... A>
[[now_init]] T *construct_at(T *p [[ref_to_uninit]], A &&...args);

struct CtorAtPayload { int x; int y; };
void test_construct_at_bridge() {
  CtorAtPayload s [[uninit]];
  construct_at(&s, 1, 2);
  int v = s.x; // OK: construct_at initialized the whole object
  construct_at(&s, 3, 4); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  CtorAtPayload t [[uninit]];
  int w = t.x; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
  (void)v; (void)w;
}

// The §4.5 lifecycle *end*, via [[now_uninit]] -- the recording §4.4 wishes
// for ("the object subjected to destroy_at() should be considered
// uninitialized, but there is no way of recording that in the code"): a
// call to a [[now_uninit]] function withdraws the parse-order credit of the
// storage bound to each pointer/reference parameter, so the storage is
// uninitialized again. Re-construction becomes legal, binding the storage
// to an unmarked target is the ordinary unmarked-direction violation, and
// a second destruction is the dedicated double_destroy violation: the
// callee's own parameters take initialized (or credited) storage --
// destroying storage that is still or again uninitialized is the
// dedicated destroy_uninit violation, while raw-release callees (free)
// keep any-state acceptance (see Limitations).
template <class T>
[[now_uninit]] void destroy_at(T *p);
[[now_uninit]] void nu_wipe(int *p);
[[now_uninit]] void nu_wipe_ref(int &r);
[[now_uninit]] void nu_wipe2(int *p, int *q);

void test_destroy_at_lifecycle() {
  CtorAtPayload s [[uninit]];
  construct_at(&s, 1, 2);
  destroy_at(&s);         // OK: destroys initialized storage
  construct_at(&s, 3, 4); // OK: uninitialized again (the bridge's error above)
  int v = s.x;            // OK: re-credited
  destroy_at(&s);         // OK: re-destroy after re-construction
  (void)v;
}

void test_double_destroy() {
  int u [[uninit]];
  now_init_fill(&u);
  nu_wipe(&u); // OK
  nu_wipe(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// Destroying storage that was never initialized is rejected: destruction
// makes an object uninitialized, so a first destroy of never-constructed
// storage is as much an access to raw memory as a second one ("Lifetimes",
// p4222r2.md:922-927 -- "it is an error to uninitialize an object twice").
// Raw-release callees (free) keep any-state acceptance: taking storage
// that may never have been constructed is their contract (see
// Limitations).
void test_destroy_never_initialized() {
  int u [[uninit]];
  nu_wipe(&u); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  int *r [[ref_to_uninit]] = &u; // OK: still uninitialized
  (void)r;
}

// A whole-entity store retires the destroyed state -- a write to a built-in
// is its (re)initialization -- so a later destroy is not a double destroy.
void test_store_retires_destroyed() {
  int u [[uninit]];
  u = 5;
  nu_wipe(&u);
  u = 7;
  nu_wipe(&u); // OK
}

// The destroyed state is per shape, exactly like the credit it shadows.
void test_double_destroy_pointee(int *p [[ref_to_uninit]]) {
  *p = 5;
  nu_wipe(p);
  nu_wipe(p); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}
void test_double_destroy_member() {
  struct M { int m [[uninit]]; } a;
  a.m = 5;
  nu_wipe(&a.m);
  nu_wipe(&a.m); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// Reseating a marked pointer retires its pointee's destroyed state with the
// rest of the pointee facts: they described the old pointee. The proof is
// mutual exclusivity: the destroy after the reseat fires destroy_uninit
// (the new pointee is uncredited marked storage), not double_destroy.
void test_reseat_clears_destroyed(int *p [[ref_to_uninit]],
                                  int *q [[ref_to_uninit]]) {
  *p = 5;
  nu_wipe(p);
  p = q;
  nu_wipe(p); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

// Destroying through a marked pointer with no prior store is rejected: the
// marker asserts an uninitialized pointee at entry. A deliberate
// strictness -- if a helper filled the pointee, mark the helper
// [[now_init]], store through the marker first, or suppress (see
// Limitations).
void test_destroy_marked_no_store(int *p [[ref_to_uninit]]) {
  nu_wipe(p); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

// The by-reference parameter dispatches the glvalue recognizer: a fresh
// referent is rejected like the pointer form.
void test_destroy_by_reference_never_stored() {
  int u [[uninit]];
  nu_wipe_ref(u); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

// The escape-strictness shape: a plain callee taking the marked parameter
// earns no credit (only [[now_init]] does), so the destroy after it is
// rejected even if the callee did fill the storage -- the same strictness
// and remedies as test_plain_callee_no_credit above.
void test_destroy_after_plain_fill() {
  int u [[uninit]];
  plain_fill(&u);
  nu_wipe(&u); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}
void test_destroy_after_now_init_fill() {
  int u [[uninit]];
  now_init_fill(&u);
  nu_wipe(&u); // OK: the [[now_init]] callee initialized the storage
}

// A destroy-role callee may declare a parameter [[ref_to_uninit]]: the
// marker is the author's contract that the parameter takes
// possibly-uninitialized storage -- the annotation spelling for an
// unrecognized storage-release function (see Limitations) -- so
// destroy_uninit yields to it. Per parameter, unlike the reinitializer's
// call-wide exemption: a sibling unmarked parameter still fires.
[[now_uninit]] void nu_release(int *p [[ref_to_uninit]]);
[[now_uninit]] void nu_release2(int *p [[ref_to_uninit]], int *q);
void test_marked_param_destroy_exempt() {
  int u [[uninit]];
  nu_release(&u); // OK: the marked parameter accepts uninitialized storage
}
void test_marked_param_exemption_is_per_parameter() {
  int u [[uninit]], v [[uninit]];
  nu_release2(&u, &v); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}

// A conditional store earns Maybe credit, which suppresses: the destroy is
// accepted although one path destroys never-stored storage -- a missed
// diagnostic relative to the paper's "for acceptance all alternatives must
// provide the desired solution" ("Guarantees", p4222r2.md:1982-1985), the
// usual parse-order conservatism.
void test_conditional_store_then_destroy(bool c) {
  int u [[uninit]];
  if (c)
    u = 5;
  nu_wipe(&u); // OK: Maybe credit suppresses
}

// destroy_uninit and double_destroy are mutually exclusive by state: on
// never-stored storage the first destroy fires destroy_uninit -- and still
// records the destroyed state -- so the second fires double_destroy.
void test_destroy_uninit_then_double_destroy() {
  int u [[uninit]];
  nu_wipe(&u); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  nu_wipe(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// Each rule suppresses under its own name...
void test_suppress_destroy_uninit() {
  int u [[uninit]];
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "destroy_uninit")]]
  nu_wipe(&u); // OK: suppressed
}
// ...and a suppressed double destroy yields silence, not a swapped
// destroy_uninit error: the branch keys on the state (destroyed), not on
// whether the double_destroy diagnostic was emitted.
void test_suppress_double_destroy_stays_silent() {
  int u [[uninit]];
  u = 1;
  nu_wipe(&u); // OK: destroying initialized storage
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "double_destroy")]]
  nu_wipe(&u); // OK: suppressed, and no destroy_uninit in its place
}

// A dependent source defers to instantiation, exactly like the sibling
// binding checks.
template <class T>
void template_destroy_dependent() {
  T u [[uninit]];
  nu_wipe(&u); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
}
template void template_destroy_dependent<int>(); // expected-note {{in instantiation of function template specialization 'template_destroy_dependent<int>' requested here}}

// Ctor-body twins: member credit is a parse-order fact, so a destroy of a
// never-assigned [[uninit]] member fires while an assigned one is clean.
struct DestroyInCtor {
  int m [[uninit]];
  DestroyInCtor() {
    nu_wipe(&m); // expected-error {{uninitialized storage is destroyed by a '[[now_uninit]]' function under profile 'std::init'}}
  }
  DestroyInCtor(int) {
    m = 1;
    nu_wipe(&m); // OK: parse-order member credit
  }
};

// A decayed-array argument credits the [[uninit]] array whole, exactly the
// storage the dedicated acceptance arm already binds (§6's
// uninitialized_fill(arr, ...) shape): accept and credit agree.
void test_now_init_array_decay_credit() {
  [[uninit]] int arr[8];
  now_init_fill(arr); // OK: marked target, uninitialized source
  ni_sink(arr);       // OK: the callee initialized the array whole
  int x = arr[0];     // OK: credited
  (void)x;
}
void test_now_init_array_decay_reverse() {
  [[uninit]] int arr[8];
  now_init_fill(arr);
  now_init_fill(arr); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
// The element-address shape resolves nothing (§5.4's element ban), and a
// file-scope [[uninit]] array is not a creditable local: neither earns
// credit, so the unmarked binding after the call keeps failing.
void test_now_init_array_element_no_credit() {
  [[uninit]] int arr[8];
  now_init_fill(&arr[0]);
  ni_sink(arr); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
void test_now_init_array_static_no_credit() {
  now_init_fill(g_uninit_arr);
  int *q = g_uninit_arr; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q;
}
// [[now_uninit]] withdrawal shares the resolver: a decayed-array argument
// withdraws the whole-array credit again.
void test_now_uninit_array_decay_withdrawal() {
  [[uninit]] int arr[4];
  now_init_fill(arr);
  ni_sink(arr); // OK: credited
  nu_wipe(arr);
  ni_sink(arr); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A *conditional* destroy records no destroyed state -- it may not have
// run, so a later destroy is not a definite double destroy.
void test_conditional_destroy_no_destroyed_state(bool c) {
  int u [[uninit]];
  u = 5;
  if (c)
    nu_wipe(&u);
  nu_wipe(&u); // OK
}

void test_use_after_destroy_binding() {
  int u [[uninit]];
  now_init_fill(&u);
  nu_wipe(&u);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *r [[ref_to_uninit]] = &u; // OK: uninitialized again
  (void)q; (void)r;
}

// The pointee shapes withdraw like the store-credit forms: destroying
// through the marked pointer clears its pointee credit, so a later read
// through it is the read-through violation again.
void test_destroy_pointee(int *p [[ref_to_uninit]]) {
  *p = 5;
  int x = *p; // OK: credited
  nu_wipe(p);
  int y = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x; (void)y;
}

// A by-reference [[now_uninit]] parameter destroys its referent; a marked
// reference's referent credit -- which no store can clear -- is withdrawn
// the same way.
void test_destroy_by_reference() {
  int u [[uninit]];
  u = 5;
  nu_wipe_ref(u);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q;
}
void test_destroy_marked_reference(int &r [[ref_to_uninit]]) {
  r = 5;
  nu_wipe(&r);
  int x = r; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x;
}

// Whole-member credit is withdrawn per base object: destroying a1's member
// leaves a2's credit intact.
struct NuMember { int m [[uninit]]; };
void test_destroy_member_per_object() {
  NuMember a1, a2;
  a1.m = 5;
  a2.m = 5;
  nu_wipe(&a1.m);
  int *q1 = &a1.m; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *q2 = &a2.m; // OK: a2's credit is untouched
  (void)q1; (void)q2;
}

// A multi-parameter [[now_uninit]] function withdraws every pointer
// argument's credit (the attribute's contract covers all of them).
void test_destroy_multi_param() {
  int u [[uninit]], v [[uninit]];
  u = 1;
  v = 2;
  nu_wipe2(&u, &v);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *r = &v; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q; (void)r;
}

// A destroy in a never-executed context destroys nothing (mirroring the
// no-credit contexts of the store recorder).
void test_destroy_never_executed() {
  int u [[uninit]];
  u = 5;
  using X = decltype(nu_wipe(&u));
  int *q = &u; // OK: the unevaluated destroy withdrew nothing
  (void)q;
}

// A suppressed destroy still destroys, exactly as a suppressed store still
// credits: failing to withdraw would turn suppression into later missed
// double-destroy diagnostics.
void test_suppressed_destroy_withdraws() {
  int u [[uninit]];
  u = 5;
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]]
  nu_wipe(&u);
  int *q = &u; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)q;
}

// A destroy through a function pointer presents no ParmVarDecl, so it
// withdraws nothing -- stale credit, a known missed diagnostic (the same
// boundary as [[now_init]] call credit).
void test_destroy_through_function_pointer() {
  int u [[uninit]];
  u = 5;
  void (*fp)(int *) = nu_wipe;
  fp(&u);
  int *q = &u; // OK: known gap -- the marker is invisible through the pointer
  (void)q;
}

// A *conditional* destroy is not an unconditional withdrawal: it revokes
// only the credit's firing strength -- the destroy may have run, so the
// requires-uninit direction may no longer fire on the credit -- while the
// suppressing credit survives (the storage may still be initialized), so
// the unmarked direction gains no new errors either. Both bindings below
// are accepted.
void test_conditional_destroy(bool c) {
  int u [[uninit]];
  now_init_fill(&u);
  if (c)
    nu_wipe(&u);
  int *q = &u;                   // OK: suppressing credit survives
  int *r [[ref_to_uninit]] = &u; // OK: no definite credit left to fire on
  (void)q; (void)r;
}

// A callee carrying both markers is a reinitializer: it destroys and then
// constructs its argument's storage, so the net post-call state is
// initialized (withdrawal is recorded before credit). Fresh never-stored
// storage is its canonical input -- destroy-then-construct legalizes it --
// so a reinitializer is exempt from destroy_uninit (the two calls below
// on fresh storage are the exemption's canaries).
[[now_init]] [[now_uninit]] void nu_reinit(int *p [[ref_to_uninit]]);
void test_reinit_nets_initialized() {
  int u [[uninit]];
  nu_reinit(&u);
  int *q = &u; // OK: net state is initialized
  nu_wipe(&u); // OK: destroying initialized storage
  (void)q;
}
void test_reinit_reverse_direction() {
  int u [[uninit]];
  nu_reinit(&u);
  int *r [[ref_to_uninit]] = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)r;
}

// A reinitializer called on *initialized* storage is exactly what a
// reinitializer exists for: the [[now_uninit]] half makes its parameter
// accept storage in any live state, so the marked parameter's
// requires-uninit direction must not reject it.
void test_reinit_on_initialized() {
  int u [[uninit]];
  u = 5;
  nu_reinit(&u); // OK
  int *q = &u;   // OK: net state is initialized
  (void)q;
}

// ...but its destroy half is as invalid on *destroyed* storage as anyone
// else's; construct_at (a plain [[now_init]] function) is the sanctioned
// recovery path.
void test_reinit_on_destroyed() {
  int u [[uninit]];
  u = 5;
  nu_wipe(&u);
  nu_reinit(&u); // expected-error {{storage already destroyed by a '[[now_uninit]]' function is destroyed again under profile 'std::init'}}
}

// The reinitializer exemption is call-wide: a dual-attributed callee's
// *unmarked* destroy-only pointer parameters -- whose storage the paper
// requires live -- are exempt from destroy_uninit too. A missed
// diagnostic, not a rule (see Limitations); the marked parameter is what
// positively legalizes fresh storage.
[[now_init]] [[now_uninit]] void nu_reinit2(int *p [[ref_to_uninit]], int *q);
void test_reinit_exemption_is_call_wide() {
  int u [[uninit]], v [[uninit]];
  nu_reinit2(&u, &v); // OK: &v (never stored, unmarked parameter) is exempt
}

// The reverse direction applies through the assignment funnel too: a
// credited marked pointer refers to initialized memory, so assigning it to
// another marked pointer is the requires-uninit error -- while an unmarked
// target now accepts it (paper §4.3: "p no longer refers to uninitialized
// memory").
void test_assign_credited_to_marked(int *p [[ref_to_uninit]],
                                    int *q [[ref_to_uninit]]) {
  *p = 5;
  q = p; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}
void test_credited_to_unmarked(int *p [[ref_to_uninit]]) {
  *p = 5;
  int *s = p; // OK: the credited pointee is initialized
  (void)s;
}

// A store through a marked *reference* credits its referent; a reference
// cannot be reseated, so the credit is never cleared.
void test_marked_ref_store_credit(int &r [[ref_to_uninit]]) {
  r = 5;
  int x = r; // OK: the store initialized the referent
  (void)x;
}

// ...and the reference gets the reverse direction too: after the store its
// referent is initialized, so a marked reference can no longer bind to it.
void test_marked_ref_reverse_direction(int &r [[ref_to_uninit]]) {
  r = 5;
  int &r3 [[ref_to_uninit]] = r; // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)r3;
}

void test_marked_ref_read_before_store(int &r [[ref_to_uninit]]) {
  int x = r; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  r = 5;
  (void)x;
}

// The requires-uninit direction fires on credit only when the store is
// *unconditional* in the entity's own function: a store under an if, a
// loop, a switch, a try, &&/||/?:, or inside a lambda body may not have
// executed on the path reaching the binding, so a marked binding after it
// stays legal -- rejecting it would be a false positive on the untaken
// path. The lenient direction is untouched: the same conditional store
// still suppresses the unmarked-target error (the documented parse-order
// missed diagnostic).
void test_conditional_store_if(bool c) {
  int u [[uninit]];
  if (c)
    u = 5;
  int *r [[ref_to_uninit]] = &u; // OK: the store may not have run
  int *q = &u;                   // OK: suppressing credit still applies
  (void)r; (void)q;
}
void test_conditional_store_else(bool c) {
  int u [[uninit]];
  if (c)
    ;
  else
    u = 5;
  int *r [[ref_to_uninit]] = &u; // OK
  (void)r;
}
void test_conditional_store_while(bool c) {
  int u [[uninit]];
  while (c)
    u = 5;
  int *r [[ref_to_uninit]] = &u; // OK
  (void)r;
}
void test_conditional_store_for(int n) {
  int u [[uninit]];
  for (int i = 0; i < n; ++i)
    u = 5;
  int *r [[ref_to_uninit]] = &u; // OK
  (void)r;
}
// A do-body store always runs, but the walk conservatively counts the loop
// scope as conditional -- a missed strict-direction diagnostic, never a
// false positive.
void test_conditional_store_do(bool c) {
  int u [[uninit]];
  do
    u = 5;
  while (c);
  int *r [[ref_to_uninit]] = &u; // OK: conservatively conditional
  (void)r;
}
void test_conditional_store_switch(int n) {
  int u [[uninit]];
  switch (n) {
  case 0:
    u = 5;
    break;
  default:
    break;
  }
  int *r [[ref_to_uninit]] = &u; // OK
  (void)r;
}
void test_conditional_store_try() {
  int u [[uninit]];
  try {
    u = 5;
  } catch (...) {
  }
  int *r [[ref_to_uninit]] = &u; // OK: conservatively conditional
  (void)r;
}
void test_conditional_store_catch() {
  int u [[uninit]];
  try {
  } catch (...) {
    u = 5;
  }
  int *r [[ref_to_uninit]] = &u; // OK
  (void)r;
}
void test_conditional_store_logical_and(bool c) {
  int u [[uninit]];
  (void)(c && (u = 5));
  int *r [[ref_to_uninit]] = &u; // OK: the RHS of && is conditional
  (void)r;
}
void test_conditional_store_logical_or(bool c) {
  int u [[uninit]];
  (void)(c || (u = 5));
  int *r [[ref_to_uninit]] = &u; // OK: the RHS of || is conditional
  (void)r;
}
void test_conditional_store_ternary(bool c) {
  int u [[uninit]];
  int v [[uninit]];
  (void)(c ? (u = 5) : 0);
  (void)(c ? 0 : (v = 5));
  int *ru [[ref_to_uninit]] = &u; // OK: the ?-arm is conditional
  int *rv [[ref_to_uninit]] = &v; // OK: the :-arm is conditional
  (void)ru; (void)rv;
}
void test_conditional_store_gnu_ternary(int c) {
  int u [[uninit]];
  (void)(c ?: (u = 5));
  int *r [[ref_to_uninit]] = &u; // OK: the :-arm is conditional (Maybe credit)
  int *q = &u;                   // OK: the Maybe credit still suppresses
  (void)r; (void)q;
}
void test_lambda_body_store_conditional() {
  int u [[uninit]];
  auto f = [&] { u = 5; };
  (void)f;
  int *r [[ref_to_uninit]] = &u; // OK: the lambda may never run
  (void)r;
}
// A block body is a lambda body's twin -- it may never run -- but
// getCurFunctionDecl skips BlockDecls and a block body's scope carries the
// function-scope flag, so neither the owner check nor the depth walk sees
// it; the innermost-context walk must.
void test_block_body_store_conditional(bool c) {
  __block int u [[uninit]];
  void (^b)() = ^{ u = 5; };
  if (c)
    b();
  int *r [[ref_to_uninit]] = &u; // OK: the block may never run
  int *q = &u;                   // OK: suppressing credit still applies
  (void)r; (void)q;
}
// A destroy inside a block body is likewise merely possible: it revokes
// the firing strength only and records no destroyed state.
void test_block_body_destroy_conditional(bool c) {
  __block int u [[uninit]];
  u = 5;
  void (^b)() = ^{ nu_wipe(&u); };
  if (c)
    b();
  int *q = &u; // OK: suppressing credit survives
  nu_wipe(&u); // OK: no destroyed state was recorded by the block's destroy
  (void)q;
}
// A store in a plain nested { } block is unconditional: the block scope
// carries no control flag, so the credit keeps its firing strength.
void test_plain_block_store_still_fires() {
  int u [[uninit]];
  {
    u = 5;
  }
  int *r [[ref_to_uninit]] = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)r;
}
// A store and a marked binding both at the top level of one lambda body:
// the conditional-depth walk stops at the *nearest* function scope, so the
// store is unconditional within the lambda and still fires.
void test_lambda_toplevel_store_still_fires() {
  auto f = [] {
    int u [[uninit]];
    u = 5;
    int *r [[ref_to_uninit]] = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
    (void)r;
  };
  f();
}
// A store in a control-statement *condition* always runs, but the control
// scope is pushed before the parens are parsed, so it conservatively
// counts as conditional: suppressing credit only.
void test_store_in_condition_suppresses_only(bool c) {
  int u [[uninit]];
  if ((u = 5))
    ;
  int *r [[ref_to_uninit]] = &u; // OK: conservatively conditional
  int *q = &u;                   // OK: suppressing credit applies
  (void)r; (void)q;
}
// A goto seen earlier in the function can skip any later store without
// introducing a scope, so a store after one records suppressing credit
// only...
void test_store_after_goto(bool c) {
  int u [[uninit]];
  if (c)
    goto skip;
  u = 5;
skip:;
  int *r [[ref_to_uninit]] = &u; // OK: the goto path skips the store
  int *q = &u;                   // OK: suppressing credit applies
  (void)r; (void)q;
}
// ...while a store *before* the first goto keeps its firing strength: no
// later jump can skip it on any path that still reaches the binding.
void test_store_before_goto(bool c) {
  int u [[uninit]];
  u = 5;
  if (c)
    goto skip;
skip:;
  int *r [[ref_to_uninit]] = &u; // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  (void)r;
}
// A goto-skippable *destroy* records no destroyed state either: the later
// destroy may be the only one that ran.
void test_destroy_after_goto(bool c) {
  int u [[uninit]];
  u = 5;
  if (c)
    goto skip;
  nu_wipe(&u);
skip:;
  nu_wipe(&u); // OK: the first destroy may have been skipped
}
// The branch flag is shared with switch, so a store after one is
// conservatively suppressing-only too -- a documented over-inclusion in
// the safe direction (a switch cannot actually skip a later store).
void test_store_after_switch(int n) {
  int u [[uninit]];
  switch (n) {
  default:
    break;
  }
  u = 5;
  int *r [[ref_to_uninit]] = &u; // OK: conservatively conditional
  (void)r;
}

// Store credit is recorded at pattern-parse time too: non-dependent
// store-then-read inside a template is checked at definition time (the
// documented phase-7 trade-off) and must find the pattern-time credit;
// instantiations rebuild every DeclRefExpr against fresh declarations and
// re-record independently.
template <typename T>
void template_store_then_read(int *p [[ref_to_uninit]]) {
  *p = 5;
  int x = *p; // OK at definition time and at instantiation
  (void)x;
}
template void template_store_then_read<int>(int *);

// A store in a discarded if-constexpr branch is not instantiated, so the
// rebuilt read finds no credit at that instantiation -- while the pattern's
// store did credit the definition-time check (a dependent condition
// discards nothing at parse).
template <bool B>
void template_discarded_store(int *p [[ref_to_uninit]]) {
  if constexpr (B)
    *p = 5;
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x;
}
template void template_discarded_store<true>(int *);  // OK: store instantiated
template void template_discarded_store<false>(int *); // expected-note {{in instantiation of function template specialization 'template_discarded_store<false>' requested here}}

// Known residual gap (documented definition-time-purity trade-off): a store
// with a *type-dependent RHS* routes through the overloaded-operator path at
// pattern time and never reaches the built-in assignment funnel, so it earns
// no pattern-time credit and the following non-dependent read
// false-positives at definition time. The instantiation is clean (its
// rebuilt store re-records first) -- exactly one error total.
template <typename T>
void template_dependent_rhs_store(int *p [[ref_to_uninit]], T t) {
  *p = t;
  int x = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)x;
}
template void template_dependent_rhs_store<int>(int *, int);

// std::init / ref_to_uninit (paper §5): a pointer/reference member given a
// *written* constructor member-initializer is checked with the enclosing
// constructor as the Decl, so a class-template pattern defers and fires once at
// instantiation, mirroring ctor_uninit_member. Both the parenthesized and the
// braced member-initializer forms reach the same recognizer.
struct CtorMemberPtrBad {
  int *p1;
  int *p2 [[ref_to_uninit]];
  CtorMemberPtrBad() : p1(&g_uninit), p2(&g_init) {}
  // expected-error@-1 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  // expected-error@-2 {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
};

struct CtorMemberPtrOK {
  int *p;
  int *q [[ref_to_uninit]];
  CtorMemberPtrOK() : p(&g_init), q(&g_uninit) {} // OK
};

struct CtorMemberRefBad {
  int &r;
  int &s [[ref_to_uninit]];
  CtorMemberRefBad() : r(g_uninit), s(g_init) {}
  // expected-error@-1 {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  // expected-error@-2 {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
};

struct CtorMemberRefOK {
  int &r;
  int &s [[ref_to_uninit]];
  CtorMemberRefOK() : r(g_init), s(g_uninit) {} // OK
};

// A member of an anonymous struct/union reaches the member-initializer site as
// an IndirectFieldDecl; the [[ref_to_uninit]] marking lives on the underlying
// field and is read from there.
struct CtorAnonStructMarkedOK {
  struct {
    int *p [[ref_to_uninit]];
  };
  CtorAnonStructMarkedOK() : p(&g_uninit) {} // OK: marked target, uninit source
};

struct CtorAnonStructMarkedBad {
  struct {
    int *p [[ref_to_uninit]];
  };
  CtorAnonStructMarkedBad() : p(&g_init) {} // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
};

struct CtorAnonStructUnmarkedBad {
  struct {
    int *p;
  };
  CtorAnonStructUnmarkedBad() : p(&g_uninit) {} // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

struct CtorAnonUnionMarkedOK {
  union {
    int *p [[ref_to_uninit]];
    long *q;
  };
  CtorAnonUnionMarkedOK() : p(&g_uninit) {} // OK: marked target, uninit source
};

// A braced member-initializer is looked through to its single element, exactly
// like the variable-init site.
struct CtorBracedPtr {
  int *p;
  CtorBracedPtr() : p{&g_uninit} {} // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

struct CtorBracedRef {
  int &r;
  CtorBracedRef() : r{g_uninit} {} // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

// An out-of-line constructor definition is checked where it is defined; the
// enclosing constructor is still the CurContext there.
struct CtorOutOfLine {
  int *p;
  CtorOutOfLine();
};
CtorOutOfLine::CtorOutOfLine() : p(&g_uninit) {} // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}

// Cast and call sources reach the recognizer at the member-init site too: a
// cast of a [[ref_to_uninit]]-returning call propagates the marking.
struct CtorCastSource {
  int *p;
  CtorCastSource() : p((int *)alloc_void()) {} // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

struct CtorSuppressWhole {
  int *p;
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] CtorSuppressWhole() : p(&g_uninit) {} // OK: suppressed
};

struct CtorSuppressRule {
  int *p;
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]] CtorSuppressRule() : p(&g_uninit) {} // OK: suppressed
};

template <typename T>
struct CtorTmpl {
  int *p;
  CtorTmpl() : p(&g_uninit) {} // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};
template struct CtorTmpl<int>; // expected-note {{in instantiation of member function 'CtorTmpl<int>::CtorTmpl' requested here}}

// A never-instantiated class template stays silent: the deferred check never
// runs on the pattern.
template <typename T>
struct CtorTmplNever {
  int *p;
  CtorTmplNever() : p(&g_uninit) {}
};

// A written pointer member-initializer that binds to uninitialized memory
// yields exactly one ref_to_uninit error; the member is written, so
// ctor_uninit_member does not also fire.
struct NoDoubleFire {
  int *p;
  NoDoubleFire() : p(&g_uninit) {} // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
};

// A pointer member left uninitialized is not a ref_to_uninit binding (there is
// no written initializer) and yields only the ctor_uninit_member error.
struct UninitMemberOnly {
  int *p; // expected-note {{member 'p' declared here}}
  UninitMemberOnly() {} // expected-error {{constructor does not initialize member 'p' under profile 'std::init'}}
};

// std::init / ref_to_uninit (paper §5): a pointer/reference field initialized
// by an enclosing aggregate's init list is checked Decl-less, scoped to the
// field subobject, so the enclosing variable/argument/return is left to its own
// site (the variable site is not a pointer/reference here) and there is no
// double diagnostic.
struct AggPtr { int *p; };
struct AggPtrMarked { int *p [[ref_to_uninit]]; };
struct AggRef { int &r; };
struct AggRefMarked { int &r [[ref_to_uninit]]; };

void test_aggregate_pointer() {
  AggPtr a1{&g_uninit};       // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggPtr a2{&g_init};          // OK
  AggPtr a3 = {&g_uninit};     // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggPtrMarked m1{&g_init};    // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  AggPtrMarked m2{&g_uninit};  // OK
  (void)a1; (void)a2; (void)a3; (void)m1; (void)m2;
}

void test_aggregate_reference() {
  AggRef a1{g_uninit};         // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggRef a2{g_init};            // OK
  AggRefMarked m1{g_init};      // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  AggRefMarked m2{g_uninit};    // OK
  (void)a1; (void)a2; (void)m1; (void)m2;
}

void test_aggregate_designated() {
  AggPtr a{.p = &g_uninit}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggPtr b{.p = &g_init};    // OK
  (void)a; (void)b;
}

struct AggNested { AggPtr inner; };

void test_aggregate_nested() {
  AggNested a{{&g_uninit}}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggNested b{{&g_init}};    // OK
  (void)a; (void)b;
}

// A pointer *element* of an array is a binding position with no declaration to
// carry the marker (like a variadic argument), so it is checked as an unmarked
// target and paper §4.3's rules apply per pointer element. The enclosing array
// variable's own site no-ops (an array is not a pointer/reference), so each
// violating element fires exactly once.
void test_array_element_pointer() {
  int *a1[2] = {&g_uninit, &g_init}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *a2[2] = {&g_init, &g_init};   // OK
  int *base [[ref_to_uninit]] = &g_uninit;
  int *a3[1] = {base};        // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *a4[1] = {allocate(3)}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  // Null sources stay accepted: {} value-initializes the elements to null and
  // nullptr is a null source, consistent with an unmarked target (paper §4.3).
  int *a5[2] = {};                 // OK
  int *a6[2] = {nullptr, &g_init}; // OK
  (void)a1; (void)a2; (void)base; (void)a3; (void)a4; (void)a5; (void)a6;
}

// Parse-order store credit applies to element sources like any other binding:
// after the whole-entity store, &u refers to initialized memory.
void test_array_element_store_credit() {
  int u [[uninit]];
  u = 5;
  int *a[1] = {&u}; // OK: credited by the store above
  (void)a;
}

// Composition with nested aggregates: an array-of-struct element recurses back
// into the member gate (the field's own marker governs), and a struct's array
// member reaches the element gate.
struct WithPtrArray { int *a[2]; };

void test_array_element_nested() {
  AggPtr arr1[1] = {{&g_uninit}};        // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggPtrMarked arr2[1] = {{&g_uninit}};  // OK: the field carries the marker
  AggPtrMarked arr3[1] = {{&g_init}};    // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  WithPtrArray w = {{&g_uninit, &g_init}}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)arr1; (void)arr2; (void)arr3; (void)w;
}

// A designated array element (a C99 extension in C++) is the same element
// binding.
void test_array_element_designated() {
  // expected-warning@+2 {{array designators are a C99 extension}} no-profiles-warning@+2 {{array designators are a C99 extension}}
  // expected-error@+1 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *d[3] = {[1] = &g_uninit};
  (void)d;
}

// A dependent element source defers on the pattern and is rebuilt at
// instantiation, where the Decl-less check fires per specialization (like
// template_throw_bad).
template <typename T>
void template_array_element_bad() {
  T *a[1] = {(T *)&g_uninit}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)a;
}
template void template_array_element_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_array_element_bad<int>' requested here}}

// An aggregate temporary built from an init list is checked the same way
// wherever it appears -- as a call argument, a return value, or a new-expression
// initializer. The enclosing pointer (the parameter, the return type, the
// new-expression result) is not a pointer/reference to the field's storage, so
// only the field binding is diagnosed.
void take_agg_ptr(AggPtr a);
AggPtr make_agg_bad() { return {&g_uninit}; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
AggPtr make_agg_ok() { return {&g_init}; }     // OK

void test_aggregate_temporary() {
  take_agg_ptr(AggPtr{&g_uninit});   // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  take_agg_ptr(AggPtr{&g_init});      // OK
  AggPtr *h = new AggPtr{&g_uninit}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  delete h;
}

void test_aggregate_suppress() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] AggPtr a{&g_uninit};                        // OK: whole-profile suppress
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]] AggPtr b{&g_uninit}; // OK: rule-targeted suppress
  (void)a; (void)b;
}

// Aggregate field init inside a template body is non-dependent here, so it
// fires at definition time and repeats when the local variable's
// initialization is rebuilt at instantiation; a never-instantiated template
// diagnoses at definition, once.
template <typename T>
void template_aggregate_bad() {
  AggPtr a{&g_uninit}; // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)a;
}
template void template_aggregate_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_aggregate_bad<int>' requested here}}

template <typename T>
void template_aggregate_never() {
  AggPtr a{&g_uninit}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)a;
}

// A thrown pointer copy-initializes the exception object, which cannot carry
// [[ref_to_uninit]], so it must not point to uninitialized memory. A read
// like `throw *p` is the read-through check's territory instead.
void throw_ptr_ok() { throw &g_init; } // OK
void throw_ptr_bad() { throw &g_uninit; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
void throw_marked_ptr_bad(int *p [[ref_to_uninit]]) { throw p; } // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
void throw_ptr_suppressed() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { throw &g_uninit; } // OK: suppressed
}

// A dependent thrown operand defers on the pattern and is rebuilt at
// instantiation, where the check fires per specialization. A fully
// non-dependent throw fires at definition time instead (see
// template_allglobal_bad): TreeTransform reuses it unchanged, so deferring
// would lose the diagnostic.
template <typename T>
void template_throw_bad() {
  throw (T *)&g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template void template_throw_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_throw_bad<int>' requested here}}

template <typename T>
void template_throw_nondependent_bad() {
  throw &g_uninit; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
template void template_throw_nondependent_bad<int>();

// C++20 parenthesized aggregate initialization performs the same per-field
// bindings as the braced form and is checked identically.
void test_aggregate_paren() {
  AggPtr a1(&g_uninit);      // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggPtr a2(&g_init);        // OK
  AggPtrMarked m1(&g_init);  // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  AggPtrMarked m2(&g_uninit); // OK
  AggRef r1(g_uninit);       // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  AggRefMarked r2(g_init);   // expected-error {{reference marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  AggNested n1((AggPtr(&g_uninit))); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] AggPtr s(&g_uninit); // OK: suppressed
  (void)a1; (void)a2; (void)m1; (void)m2; (void)r1; (void)r2; (void)n1; (void)s;
}

template <typename T>
void template_aggregate_paren_bad() {
  AggPtr a(&g_uninit); // expected-error 2 {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)a;
}
template void template_aggregate_paren_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_aggregate_paren_bad<int>' requested here}}

// A parenthesized aggregate's array elements are the same unmarked element
// bindings as the braced form; the hook runs only in the build phase, so the
// verify pass adds no second diagnostic.
void test_array_element_paren() {
  int *pa[2](&g_uninit, &g_init); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  int *pb[2](&g_init, nullptr);   // OK
  (void)pa; (void)pb;
}

// A plain pointer *variable* with a braced initializer is checked once at its
// own variable site (EK_Variable); the aggregate field hooks are scoped to a
// member subobject, so this fires exactly once with no new duplicate.
void test_variable_braced_once() {
  int *p{&g_uninit}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)p;
}

// An array new-expression's pointer elements are the same unmarked element
// bindings, for constant and variable bounds alike. The scalar
// new-initializer check stays out of array news -- a lone initializer (a
// one-element braced list, peeled by the single-element pass-through, or a
// single C++20 paren argument) binds the first *element*, already checked by
// the element hooks -- so each violation fires exactly once. Scalar
// allocations keep their single variable-like check.
void test_array_new_element(int n) {
  (void)new int *[2]{&g_uninit, &g_init}; // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)new int *[2]{&g_uninit};          // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)new int *[1](&g_uninit);          // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)new int *[n]{&g_uninit};          // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)new int *[n](&g_uninit);          // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)new int *[2]{};                   // OK: value-initialized null elements
  (void)new int *(&g_uninit);             // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)new int *{&g_uninit};             // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// std::init / ref_to_uninit (paper §5): a source whose syntactic form the
// recognizer does not model -- pointer arithmetic, an integer-to-pointer cast,
// or a call through a function pointer -- is unknown, not initialized. A marked
// target binds from such a source without error (it cannot be proven
// initialized, so rejecting it would be a false positive); an unmarked target
// also binds without error (a documented missed diagnostic). Neither run
// diagnoses these.
void test_unknown_pointer_arithmetic() {
  int *base [[ref_to_uninit]] = &g_uninit;
  int *p1 [[ref_to_uninit]] = base + 1; // OK
  int *p2 = base + 1;                    // OK
  (void)p1; (void)p2;
}

void test_unknown_int_to_ptr(long n) {
  int *p1 [[ref_to_uninit]] = reinterpret_cast<int *>(n); // OK
  int *p2 = reinterpret_cast<int *>(n);                    // OK
  (void)p1; (void)p2;
}

void test_unknown_fnptr_call(int *(*fp)()) {
  int *p1 [[ref_to_uninit]] = fp(); // OK
  int *p2 = fp();                    // OK
  (void)p1; (void)p2;
}

// The unknown classification propagates through the pass-through forms.
void test_unknown_passthrough(bool c) {
  int *base [[ref_to_uninit]] = &g_uninit;
  int *p1 [[ref_to_uninit]] = c ? base + 1 : &g_init; // OK: an unknown arm keeps the whole unknown
  int *p2 [[ref_to_uninit]] = (h(), base + 1);        // OK
  int *p3 [[ref_to_uninit]] = {base + 1};             // OK
  (void)p1; (void)p2; (void)p3;
}

// The unknown classification reaches the assignment, call-argument, and return
// sites unchanged.
void test_unknown_assignment(long n) {
  int *base [[ref_to_uninit]] = &g_uninit;
  int *p [[ref_to_uninit]] = &g_uninit;
  p = base + 1;                   // OK
  p = reinterpret_cast<int *>(n); // OK
  int *q = &g_init;
  q = base + 1;                   // OK
  (void)p; (void)q;
}

void test_unknown_call_argument(long n) {
  int *base [[ref_to_uninit]] = &g_uninit;
  take_uninit_ptr(base + 1);                   // OK
  take_uninit_ptr(reinterpret_cast<int *>(n)); // OK
  take_ptr(base + 1);                          // OK
}

[[ref_to_uninit]] int *ret_unknown_marked() {
  int *base [[ref_to_uninit]] = &g_uninit;
  return base + 1; // OK
}
int *ret_unknown_unmarked() {
  int *base [[ref_to_uninit]] = &g_uninit;
  return base + 1; // OK
}

// Regression guard: the fix narrows only the unknown case. A marked target
// bound from an affirmatively initialized source is still rejected, and an
// unmarked target from an affirmatively uninitialized source is still rejected.
void test_unknown_regression_guard() {
  int *m1 [[ref_to_uninit]] = &g_init;    // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *m2 [[ref_to_uninit]] = new int(5); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
  int *u1 = &g_uninit;                    // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)m1; (void)m2; (void)u1;
}

// A member call binds its implicit object parameter to the object argument
// (paper §7.2), and that parameter can never carry [[ref_to_uninit]], so a
// call on an object recognized as uninitialized storage is always the
// unmarked-direction violation. Every member-call flavor converts its object
// argument through the same funnel: dot and arrow calls, member operators,
// functor operator(), operator->, and conversion operators.
struct Callee {
  int m;
  int f() { return m; }
  static int sf() { return 0; }
  Callee &operator=(const Callee &);
  bool operator==(const Callee &) const;
  operator int() const;
  int *operator->();
  int operator()(int);
  int &operator[](int);
};

void test_member_call_on_uninit_object() {
  Callee s [[uninit]];
  s.f();     // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  (&s)->f(); // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}

void test_member_call_through_marked_pointer(Callee *p [[ref_to_uninit]]) {
  p->f();   // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  (*p).f(); // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}

// Member operators bind the same implicit object parameter, so whole-object
// assignment to an [[uninit]] class object -- previously unchecked through
// the overloaded operator= path -- is caught here too.
void test_member_operators_on_uninit_object() {
  Callee s [[uninit]];
  Callee t{};
  s = t;          // expected-error {{calling member function 'operator=' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  (void)(s == t); // expected-error {{calling member function 'operator==' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  int v = s;      // expected-error {{calling member function 'operator int' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  s(1);           // expected-error {{calling member function 'operator()' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  s[0] = 1;       // expected-error {{calling member function 'operator[]' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
  (void)v;
}

void test_member_arrow_on_uninit_object() {
  Callee s [[uninit]];
  (void)*(s.operator->()); // expected-error {{calling member function 'operator->' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}

// A [[ref_to_uninit]]-returning reference function yields an uninitialized
// referent; calling a member function on it is the same violation.
[[ref_to_uninit]] Callee &get_uninit_callee();
void test_member_call_on_marked_call_result() {
  get_uninit_callee().f(); // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}

Callee make_callee();
struct WithDtor {
  int m;
  void g();
  ~WithDtor();
};

// A static call operator has no implicit object parameter; the object
// argument is evaluated but its value never used, exactly like a static
// member function named through an object.
struct StaticCall {
  int m;
  static int operator()(int x) { return x; }
};

void test_member_call_silent_forms() {
  Callee s [[uninit]];
  s.sf();                 // OK: a static member uses no object argument
  (void)sizeof(s.f());    // OK: unevaluated
  using Unevaluated = decltype(s.f()); // OK: unevaluated
  (void)Unevaluated{};
  Callee t{};
  t.f();                  // OK: initialized object
  Callee{}.f();           // OK: a prvalue object is not uninitialized storage
  make_callee().f();      // OK: unmarked call result is trusted initialized
  WithDtor d [[uninit]];
  d.~WithDtor();          // OK: destruction is the deferred destroy_at slice
  StaticCall c [[uninit]];
  c(1);                   // OK: a static call operator uses no object argument
}

// A call through a pointer-to-member resolves no method at the call and
// bypasses the object-argument conversion -- the pointer-to-member analog of
// the call-through-function-pointer gap (a known gap, not an endorsement).
void test_member_call_through_pointer_to_member() {
  Callee s [[uninit]];
  int (Callee::*pmf)() = &Callee::f;
  (s.*pmf)(); // OK: known gap
}

// The object itself being unmarked keeps the trust decision: a class whose
// *member* is [[uninit]] may still have its member functions called (its
// constructor body may have assigned the member, paper §5.1/§5.2).
struct MemberOnlyUninit {
  int m [[uninit]];
  MemberOnlyUninit() { m = 1; }
  int get() { return m; }
};
void test_member_call_unmarked_object_trusted(MemberOnlyUninit &r) {
  MemberOnlyUninit o;
  o.get(); // OK
  r.get(); // OK: unknown-state reference parameter is not affirmatively uninit
}

// An explicit object member function initializes its object as an ordinary
// parameter, so the existing parameter binding check owns it (and its
// parameter *could* carry the marker).
struct ExplicitObj {
  int m;
  void f(this ExplicitObj &self);
};
void test_member_call_explicit_object() {
  ExplicitObj x [[uninit]];
  x.f(); // expected-error {{reference to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}

// A member with enable_if converts availability-check arguments under a
// SFINAE trap; the real call still diagnoses exactly once.
struct WithEnableIf {
  int m;
  void f() __attribute__((enable_if(true, "")));
};
void test_member_call_enable_if() {
  WithEnableIf s [[uninit]];
  s.f(); // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}

void test_member_call_suppressed() {
  Callee s [[uninit]];
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] { s.f(); }         // OK: suppressed
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "ref_to_uninit")]] { s.f(); } // OK
}

// A dependent object argument defers to instantiation, where the rebuilt call
// re-runs the funnel; a non-dependent call in a template fires at definition
// time and repeats when the call is rebuilt at instantiation (the local is
// remapped) -- the accepted repetition.
template <typename T>
void template_member_call_dependent_bad() {
  T s [[uninit]];
  s.f(); // expected-error {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}
template void template_member_call_dependent_bad<Callee>(); // expected-note {{in instantiation of function template specialization 'template_member_call_dependent_bad<Callee>' requested here}}

template <typename T>
void template_member_call_nondependent_bad() {
  Callee s [[uninit]];
  s.f(); // expected-error 2 {{calling member function 'f' binds its implicit object parameter to uninitialized memory under profile 'std::init'}}
}
template void template_member_call_nondependent_bad<int>(); // expected-note {{in instantiation of function template specialization 'template_member_call_nondependent_bad<int>' requested here}}

// Redeclaration (§7.2 header/source split): a parameter's [[ref_to_uninit]]
// written on any declaration is inherited by the parameter's later
// redeclarations, so the definition keeps read-through checking and call
// sites after the redeclaration still see the marker.
void redecl_fill(int *p [[ref_to_uninit]]);
void redecl_fill(int *p) {
  int v = *p; // expected-error {{read through a '[[ref_to_uninit]]' pointer or reference accesses uninitialized memory under profile 'std::init'}}
  (void)v;
  *p = 0;
}

void test_call_after_redecl() {
  int u [[uninit]];
  int i = 0;
  redecl_fill(&u); // OK: the redeclared parameter inherited the marker
  redecl_fill(&i); // expected-error {{pointer marked '[[ref_to_uninit]]' must refer to uninitialized memory under profile 'std::init'}}
}

// [[now_init]] credit survives the redeclaration: the call site resolves to
// the latest declaration, whose parameter carries the inherited marker.
[[now_init]] void redecl_now_init_fill(int *p [[ref_to_uninit]]);
void redecl_now_init_fill(int *p);
void test_now_init_credit_after_redecl() {
  int u [[uninit]];
  redecl_now_init_fill(&u); // OK: marked target, uninitialized source
  ni_sink(&u);              // OK: the [[now_init]] callee initialized u
}

// A marked function's [[ref_to_uninit]] describes its *return value*: the
// function's own decayed pointer value is not uninitialized memory, so
// binding it to an (unmarkable) function pointer -- or casting it to void*
// -- is accepted, while a *call* to it stays a marked source.
[[ref_to_uninit]] int *fnval_alloc();
void test_function_value_not_uninit() {
  int *(*fp1)() = fnval_alloc;  // OK: the function value, not its return
  int *(*fp2)() = &fnval_alloc; // OK: same through the address-of arm
  fp1 = fnval_alloc;            // OK: the assignment funnel agrees
  void *v = (void *)fnval_alloc; // OK: the cast arm recurses into the same arm
  int *r = fnval_alloc(); // expected-error {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
  (void)fp1; (void)fp2; (void)v; (void)r;
}
