// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 %s

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

void test_postfix_eq() {
  int c [[uninit]] = 0; // expected-error {{variable 'c' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)c;
}

void test_prefix_eq() {
  [[uninit]] int d = 7; // expected-error {{variable 'd' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)d;
}

void test_brace_init() {
  int e [[uninit]] {}; // expected-error {{variable 'e' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)e;
}

void test_paren_init() {
  int f [[uninit]] (3); // expected-error {{variable 'f' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)f;
}

void test_marker_alone() {
  int x [[uninit]];
  (void)x;
}

// A marked pointer or union with an initializer draws only its subject-type
// rule (pointer_marker / union_marker, which retain the marker);
// uninit_with_initializer stays silent -- exactly one diagnostic per line.
union UWI { int x; float y; };
void test_subject_type_precedence() {
  int *pp [[uninit]] = nullptr; // expected-error {{'[[uninit]]' cannot be applied to a pointer under profile 'std::init'; initialize the pointer (for example to 'nullptr')}}
  [[uninit]] int *pq = nullptr; // expected-error {{'[[uninit]]' cannot be applied to a pointer under profile 'std::init'; initialize the pointer (for example to 'nullptr')}}
  UWI u [[uninit]] = {1};       // expected-error {{'[[uninit]]' cannot be applied to a variable of union type under profile 'std::init'}}
  (void)pp; (void)pq; (void)u;
}

void test_init_alone() {
  int x = 0;
  (void)x;
}

struct WithCtor { WithCtor(); };

void test_class_synthesized_init() {
  // The implicit default-constructor call initializes the object, so the
  // marker is contradicted -- but the user wrote no initializer, so the
  // diagnostic names the type and why instead.
  WithCtor x [[uninit]]; // expected-error {{variable 'x' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'WithCtor' does not leave it uninitialized}} \
                         // expected-note {{default-initialization of 'WithCtor' runs a constructor}}
  [[uninit]] WithCtor y; // expected-error {{variable 'y' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'WithCtor' does not leave it uninitialized}} \
                         // expected-note {{default-initialization of 'WithCtor' runs a constructor}}
  // A *written* `{}` is an initializer, whichever form the AST records it in:
  // value-initialization of a type whose default constructor is trivial
  // zero-initializes, while one with a user-provided default constructor just
  // calls it -- neither is the language's own default-initialization, so both
  // keep the "has an initializer" wording.
  WithCtor z [[uninit]]{}; // expected-error {{variable 'z' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)x; (void)y; (void)z;
}

struct TrivialCtor { int a; };

void test_written_brace_on_trivial() {
  TrivialCtor t [[uninit]]{}; // expected-error {{variable 't' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)t;
}

// A default constructor explicitly defaulted *after* its first declaration
// is user-provided, yet a trivial defaulted definition initializes nothing:
// the marker is factually correct and accepted. (The record-level
// triviality bit is poisoned forever by the plain first declaration, so the
// definition's triviality is recomputed from the class shape.)
struct OutOfLineDefaulted {
  int d [[uninit]]; // acknowledged, so the constructor check stays quiet
  OutOfLineDefaulted();
};
OutOfLineDefaulted::OutOfLineDefaulted() = default;
void test_out_of_line_defaulted_marker() {
  OutOfLineDefaulted s [[uninit]]; // OK: the defaulted definition runs no code
  [[uninit]] OutOfLineDefaulted arr[2]; // OK: element-wise the same question
  (void)s; (void)arr;
}

// The recovery keys on the *definition's* triviality: an NSDMI or a vtable
// pointer makes the defaulted definition initialize something, so the
// marker stays contradicted.
struct OutOfLineDefaultedNSDMI {
  int d = 0;
  OutOfLineDefaultedNSDMI();
};
OutOfLineDefaultedNSDMI::OutOfLineDefaultedNSDMI() = default;
struct OutOfLineDefaultedPolymorphic {
  int d [[uninit]];
  OutOfLineDefaultedPolymorphic();
  virtual void v();
};
OutOfLineDefaultedPolymorphic::OutOfLineDefaultedPolymorphic() = default;
void test_out_of_line_defaulted_nonvacuous_marker() {
  OutOfLineDefaultedNSDMI n [[uninit]]; // expected-error {{variable 'n' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'OutOfLineDefaultedNSDMI' does not leave it uninitialized}} \
                                        // expected-note {{default-initialization of 'OutOfLineDefaultedNSDMI' runs a constructor}}
  OutOfLineDefaultedPolymorphic p [[uninit]]; // expected-error {{variable 'p' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'OutOfLineDefaultedPolymorphic' does not leave it uninitialized}} \
                                              // expected-note {{default-initialization of 'OutOfLineDefaultedPolymorphic' runs a constructor}}
  (void)n; (void)p;
}

// Declaration-order caveat (see Limitations): a marker written before the
// '= default' definition has been parsed still sees the
// declared-but-undefined constructor and stays rejected.
struct DefaultedAfterMarker {
  int d [[uninit]];
  DefaultedAfterMarker();
};
void test_marker_before_defaulted_definition() {
  DefaultedAfterMarker m [[uninit]]; // expected-error {{variable 'm' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'DefaultedAfterMarker' does not leave it uninitialized}} \
                                     // expected-note {{default-initialization of 'DefaultedAfterMarker' runs a constructor}}
  (void)m;
}
DefaultedAfterMarker::DefaultedAfterMarker() = default;

struct MarkedMemberAgg { int x [[uninit]]; };

void test_marked_member_agg() {
  // R4 stays factual: MarkedMemberAgg's default-initialization is a genuine
  // no-op that leaves x indeterminate, so marking the variable too is
  // consistent rather than a contradiction. (Honoring the member marker here
  // would wrongly fire uninit_with_initializer.)
  MarkedMemberAgg a [[uninit]];
  (void)a;
}

struct NoDefaultCtor { NoDefaultCtor() = delete; }; // expected-note {{'NoDefaultCtor' has been explicitly marked deleted here}} \
                                                    // no-profiles-note {{'NoDefaultCtor' has been explicitly marked deleted here}}

void test_failed_init_no_double_diag() {
  // Default-init fails and installs a RecoveryExpr placeholder. That is not a
  // user-written initializer, so R4 must not pile a spurious diagnostic on top
  // of the real error (the absence of an extra '...have an initializer...'
  // diagnostic here is the assertion).
  NoDefaultCtor z [[uninit]]; // expected-error {{call to deleted constructor of 'NoDefaultCtor'}} \
                                     // no-profiles-error {{call to deleted constructor of 'NoDefaultCtor'}}
  (void)z;
}

int g_marker_with_init [[uninit]] = 42; // expected-error {{variable 'g_marker_with_init' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}

void test_suppress() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_with_initializer")]]
  int x [[uninit]] = 0;
  (void)x;
}

void test_suppress_block() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] {
    int a [[uninit]] = 0;
    int b [[uninit]] = 1;
    (void)a; (void)b;
  }
}

// [[profiles::suppress]] on a data member covers the uninit_with_initializer
// check that runs when its NSDMI is finalized, not just its parsing.
struct WithSuppressedNSDMI {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_with_initializer")]] int m [[uninit]] = 0; // OK: rule-targeted suppress
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init)]] int n [[uninit]] = 1;                                  // OK: whole-profile suppress
};

// A suppress on the enclosing record covers its members' NSDMIs.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
struct [[profiles::suppress(std::init)]] WithClassLevelSuppressedNSDMI {
  int m [[uninit]] = 0; // OK: suppressed by the class-level attribute
};

// A profile rule fires on the instantiation, not on the template pattern, so a
// dependent [[uninit]]-with-initializer is diagnosed exactly once.
template <typename T>
void template_marker_with_init() {
  T x [[uninit]] = T{}; // expected-error {{variable 'x' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)x;
}
template void template_marker_with_init<int>(); // expected-note {{in instantiation of function template specialization 'template_marker_with_init<int>' requested here}}

// A *non-dependent* declaration inside a template body is likewise diagnosed
// once -- at instantiation -- not on the pattern.
template <typename T>
void template_nondependent_with_init() {
  int x [[uninit]] = 0; // expected-error {{variable 'x' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)x;
}
template void template_nondependent_with_init<int>(); // expected-note {{in instantiation of function template specialization 'template_nondependent_with_init<int>' requested here}}

// An uninstantiated template pattern is not yet a phase-7 entity, so no profile
// rule fires on it (no expected diagnostic here).
template <typename T>
void template_never_instantiated() {
  int x [[uninit]] = 0;
  (void)x;
}

// Default-initialization that is not a genuine no-op contradicts the marker
// just as a written initializer does (paper §4.2 rule 2, §5.3): something is
// initialized, so the object is not left uninitialized. The wording differs --
// there is no initializer to point at, only a type -- and the note gives the
// reason: a constructor runs (Mixed, Polymorphic), a default member
// initializer runs (WithNSDMIMember), or nothing is left indeterminate.
struct MixedInner { MixedInner(); };
struct Mixed { int x; MixedInner s; };
struct WithNSDMIMember { int a; int b = 0; };
struct Polymorphic { virtual void f(); int x; };
struct TrivialAgg { int x; };

void test_default_init_not_noop() {
  Mixed s4 [[uninit]];            // expected-error {{variable 's4' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'Mixed' does not leave it uninitialized}} \
                                  // expected-note {{default-initialization of 'Mixed' runs a constructor}}
  WithNSDMIMember q [[uninit]];   // expected-error {{variable 'q' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'WithNSDMIMember' does not leave it uninitialized}} \
                                  // expected-note {{default-initialization of 'WithNSDMIMember' runs a constructor}}
  Polymorphic v [[uninit]];       // expected-error {{variable 'v' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'Polymorphic' does not leave it uninitialized}} \
                                  // expected-note {{default-initialization of 'Polymorphic' runs a constructor}}
  // The error names the array type, the note the element type whose
  // default-initialization is the problem.
  [[uninit]] Mixed arr[2];        // expected-error {{variable 'arr' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'Mixed[2]' does not leave it uninitialized}} \
                                  // expected-note {{default-initialization of 'Mixed' runs a constructor}}
  TrivialAgg t [[uninit]];        // OK: a genuine no-op, 't.x' really is left
                                  // indeterminate
  (void)s4; (void)q; (void)v; (void)arr; (void)t;
}

// A `= P()` value-initialization zeroes the object -- an initialization the
// marker contradicts; the `= P{}` list form was already rejected (pinned
// here), and the two must not diverge.
struct P { int a; int b; };
void test_value_init() {
  P p [[uninit]] = P();   // expected-error {{variable 'p' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  P p2 [[uninit]] = P{};  // expected-error {{variable 'p2' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
  (void)p; (void)p2;
}

// An NSDMI in a class template is checked once, when its initializer is
// instantiated (here forced by initializing the specialization).
template <typename T>
struct WithTemplatedNSDMI {
  T m [[uninit]] = T{}; // expected-error {{member 'm' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
};
void use_templated_nsdmi() {
  WithTemplatedNSDMI<int> w = {}; // expected-note {{in instantiation of default member initializer 'WithTemplatedNSDMI<int>::m' requested here}}
  (void)w;
}

// A written `{}` NSDMI on a type with a user-provided default constructor is
// an initializer too, not the language's default-initialization -- the member
// twin of test_class_synthesized_init's `z`.
struct WithBracedNSDMI {
  WithCtor m [[uninit]]{}; // expected-error {{member 'm' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}
};
