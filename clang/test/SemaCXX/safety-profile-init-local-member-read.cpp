// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized %s
// The LEADING_ERROR runs add a leading unrelated error so every later function
// is analyzed through the post-error path; the same local-aggregate member
// diagnostics must still fire there.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized -DLEADING_ERROR %s
// -Wunreachable-code makes the main path build a fully linearized CFG
// (setAllAlwaysAdd); the pass must recover the same events from it (the
// linearized-CFG invariant at addNonLinearizedAlwaysAddClasses). The
// post-error rerun is always non-linearized, so this axis is clean-only.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 -Wno-uninitialized -Wunreachable-code %s

// std::init: an [[uninit]] scalar member of a constructor-less aggregate
// local (the paper §5.3 "class exposing uninitialized members" pattern) is
// given a value by a plain member store; a read before the member is
// definitely assigned on every path is diagnosed by a per-function
// definite-assignment pass, the local-variable analog of the ctor-body check.

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

namespace std { enum class byte : unsigned char {}; class type_info; }

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// expected-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
// no-profiles-error@-2 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

struct Agg {
  int m [[uninit]]; // expected-note 20 {{member 'm' declared here}}
};
void take_ref(Agg &);
// The pointee is uninitialized memory, so the parameter carries the marker
// (the unmarked spelling is the ref_to_uninit binding rule's to reject).
void take_ptr(int *p [[ref_to_uninit]]);

int test_read_before_any_write() {
  Agg a;
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

int test_read_then_write() {
  Agg a;
  int v = a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
  a.m = 1;
  return v;
}

int test_branch_one_path(bool c) {
  Agg a;
  if (c)
    a.m = 1;
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// A compound assignment and a built-in ++/-- read the old value before
// writing it.
void test_compound_reads_old_value() {
  Agg a;
  a.m += 1; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

void test_incdec_reads_old_value() {
  Agg a;
  a.m++; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// A loop body may run zero times, so an assignment inside it does not reach a
// read after the loop...
int test_loop_may_not_run(int n) {
  Agg a;
  for (int i = 0; i < n; ++i)
    a.m = i;
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// ...and a read at the top of the body precedes the first iteration's write.
int test_loop_read_first_iteration(int n) {
  Agg a;
  int t = 0;
  for (int i = 0; i < n; ++i) {
    t += a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
    a.m = i;
  }
  return t;
}

// A loop-body read after an unconditional assignment stays clean: the loop's
// back-edge join must re-converge on "assigned" (pinning the fixpoint's
// initialized-top enqueue skip; see runDefiniteAssignment).
int test_loop_clean_after_assign(int n) {
  Agg x;
  x.m = 1;
  int acc = 0;
  for (int i = 0; i < n; ++i)
    acc += x.m;
  return acc;
}

// sizeof neither reads the member (unevaluated) nor escapes the object.
int test_sizeof_neither_reads_nor_escapes() {
  Agg a;
  (void)sizeof(a.m);
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// The other unevaluated contexts behave like sizeof: no read, no escape.
int test_unevaluated_matrix() {
  Agg a;
  (void)noexcept(a.m + 1);
  (void)__builtin_constant_p(a.m);
  (void)typeid(a.m);
  bool r = requires { a.m + 1; };
  (void)r;
  [[assume(a.m == 1)]];
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// An unevaluated mention earns no assignment credit either.
int test_unevaluated_no_credit() {
  Agg a;
  (void)__builtin_constant_p(a.m = 1);
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// An [[uninit]] member inherited from a constructor-less non-virtual base is
// tracked like the class's own (nothing can have assigned it earlier).
struct Base {
  int bm [[uninit]]; // expected-note {{member 'bm' declared here}}
};
struct Derived : Base {
  int dm = 0;
};
int test_base_subtree_member() {
  Derived d;
  return d.bm; // expected-error {{member 'bm' is read before initialization under profile 'std::init'}}
}

int test_write_then_read() {
  Agg a;
  a.m = 5;
  return a.m; // OK
}

int test_branch_both_paths(bool c) {
  Agg a;
  if (c)
    a.m = 1;
  else
    a.m = 2;
  return a.m; // OK
}

// Any appearance of the variable outside a recognized member read or write
// conservatively marks every member assigned: the address may be used to
// initialize the object (construct_at, memcpy, an initializing callee).
// These pin the interim (pre-now_init()) leniency, paper §6.2; contrast the
// ctor-body pass's strict assignment-only crediting
// (safety-profile-init-ctor-body.cpp, Escape*).
int test_escape_address_of_object() {
  Agg a;
  (void)&a;
  return a.m; // OK: escaped
}

int test_escape_address_of_member() {
  Agg a;
  take_ptr(&a.m);
  return a.m; // OK: escaped
}

int test_escape_reference_binding() {
  Agg a;
  take_ref(a);
  return a.m; // OK: escaped
}

int test_escape_lambda_capture() {
  Agg a;
  auto init = [&] { a.m = 5; };
  init();
  return a.m; // OK: the capture escapes the object
}

// Placement new takes the object's address -- the same escape as &a.
void *operator new(__SIZE_TYPE__, void *p) noexcept;
int test_escape_placement_new() {
  Agg a;
  new (&a) Agg{1};
  return a.m; // OK: escaped
}

// A transparent reference cast denotes the same storage (paper §4.3), and
// the pass peels exactly the casts the parse-order credit sees through: a
// store through `(int &)a.m` is a recognized member write crediting exactly
// `m` -- not a whole-object escape -- and a read through the cast is
// detected. Taking the cast lvalue's address is still an unrecognized use of
// the base, i.e. an escape crediting every member.
struct CastAgg {
  int m [[uninit]]; // expected-note {{member 'm' declared here}}
  int o [[uninit]]; // expected-note {{member 'o' declared here}}
};
int test_cast_store_credits_member() {
  CastAgg a;
  (int &)a.m = 1;
  return a.m; // OK: assigned through the cast
}

int test_cast_store_credits_only_that_member() {
  CastAgg a;
  (int &)a.m = 1;
  return a.o; // expected-error {{member 'o' is read before initialization under profile 'std::init'}}
}

int test_cast_read_detected() {
  CastAgg a;
  int y = (int &)a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
  return y;
}

int test_cast_address_still_escapes() {
  CastAgg a;
  take_ptr(&(int &)a.m);
  return a.o; // OK: escaped (whole-range credit)
}

// Reaching an *untracked* sibling of a scalar shape is not one of those
// escapes: accessing `n` cannot give `sm` a value, so the tracked member stays
// tracked across a sibling read, store, or increment.
struct Siblings {
  int sm [[uninit]]; // expected-note 3 {{member 'sm' declared here}}
  int n = 0;
};
int test_sibling_read_not_an_escape() {
  Siblings s;
  int v = s.n;
  return v + s.sm; // expected-error {{member 'sm' is read before initialization under profile 'std::init'}}
}

int test_sibling_write_not_an_escape() {
  Siblings s;
  s.n = 1;
  return s.sm; // expected-error {{member 'sm' is read before initialization under profile 'std::init'}}
}

int test_sibling_incdec_not_an_escape() {
  Siblings s;
  ++s.n;
  return s.sm; // expected-error {{member 'sm' is read before initialization under profile 'std::init'}}
}

// An access to a member of an *anonymous* struct is a sibling access like
// any other: reaching `x.a` cannot give `x.m` a value, so the tracked member
// stays tracked (the anonymous step is peeled, not treated as an escape).
struct Mix {
  struct {
    int a [[uninit]];
  };
  int m [[uninit]]; // expected-note 2 {{member 'm' declared here}}
};
int test_anon_sibling_read_not_an_escape() {
  Mix x;
  int v = x.a;
  return v + x.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}
int test_anon_sibling_write_not_an_escape() {
  Mix x;
  x.a = 1;
  return x.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// An anonymous-union leaf can neither be tracked nor reach a tracked member
// ([[uninit]] is banned on union members, so the NSDMI builds the fixture),
// so consuming the base is sound for unions too.
struct MixU {
  union {
    int a = 0;
    float b;
  };
  int m [[uninit]]; // expected-note {{member 'm' declared here}}
};
int test_anon_union_sibling_not_an_escape() {
  MixU x;
  int v = x.a;
  return v + x.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// A pointer leaf inside an anonymous struct keeps the escape: its value may
// denote the tracked member, exactly like a named pointer sibling
// (pointer_marker bans the marker on pointers, so the NSDMI builds the
// fixture).
struct MixP {
  struct {
    int *p = nullptr;
  };
  int m [[uninit]];
};
int test_anon_pointer_sibling_still_escapes() {
  MixP x;
  int *q = x.p;
  (void)q;
  return x.m; // OK: escaped (a missed diagnostic, deliberately)
}

// A pointer or reference sibling is the exception: its *value* can denote the
// tracked member, and a marked one can be aimed at it by a default member
// initializer -- with no `&a.sm` anywhere to escape the object. Reaching such a
// sibling therefore stays an escape, so a store through it keeps crediting the
// tracked member. Erring the other way would make this pass reject a store it
// simply cannot see, and for locals its imprecision must stay a missed
// diagnostic (contrast the ctor-body pass, which is strict by design).
struct SelfAimed {
  int sm [[uninit]];
  int *p [[ref_to_uninit]] = &sm;
};
int test_pointer_sibling_still_escapes() {
  SelfAimed a;
  *a.p = 5;      // initializes a.sm through the marked member
  return a.sm;   // OK: reaching 'p' escaped the object
}

// The same boundary with no aliasing in sight: a bare value read of a pointer
// sibling is an escape too, because the recognizer cannot tell the two apart.
struct PtrSibling {
  int sm [[uninit]];
  int *p = nullptr;
};
int test_pointer_sibling_read_escapes(PtrSibling a) {
  int *local = a.p;
  (void)local;
  return a.sm;   // OK: escaped (a missed diagnostic, deliberately)
}

// A class with a user-provided constructor is trusted (paper §5.1): its
// constructor body may have assigned the member, which local analysis cannot
// see. This pins the deliberate trust decision for non-current-object member
// reads.
struct Slot {
  int y [[uninit]];
  Slot() {}
};
int test_user_provided_ctor_trusted() {
  Slot uu;
  return uu.y; // OK: trusted
}

// A base with a user-provided constructor keeps its members untracked, even
// under a constructor-less derived class.
struct TrustedBase {
  int tm [[uninit]];
  TrustedBase() {}
};
struct DerivedFromTrusted : TrustedBase {};
int test_trusted_base_member() {
  DerivedFromTrusted d;
  return d.tm; // OK: trusted
}

// A value-initializing written form gives every member a value; only the
// bare `Agg a;` form (the implicit no-op default-construction) is tracked.
int test_value_initialized_forms() {
  Agg a{};
  Agg b = {};
  Agg c = Agg();
  return a.m + b.m + c.m; // OK
}

// The written `= T()` form stays untracked when a template rebuilds it: the
// rebuilt initializer is still a zero-initializing value-construction, not
// the plain default-init shape, in the pattern and in the instantiation
// alike (pinning the shared default-init shape predicate).
int test_value_init_zeroing_shape() {
  Agg z = Agg();
  return z.m; // OK
}
template <typename T>
int template_value_init_zeroing_shape() {
  T z = T();
  return z.m; // OK: rebuilt as a zeroing construction, untracked
}
template int template_value_init_zeroing_shape<Agg>();

// A copy does NOT give the [[uninit]] member a value -- it copies
// indeterminate bits (a copy does not inherit initialization, paper §5.2)
// -- but the source's per-member state is unknowable for an untracked
// source, so such copies stay untracked: a known gap, never a false
// positive.
Agg make_agg();
int test_copy_from_untracked_source() {
  Agg e = make_agg();
  return e.m; // OK: known gap (untracked source)
}

// A by-value parameter is a copy of the caller's argument, and a copy does
// not inherit initialization (§5.2): its marked members are tracked from an
// unassigned start. This is the call-boundary twin of the ctor-body pass's
// deliberate strictness -- the paper hands uninitialized-capable storage
// across calls via marked pointers/references (§4.3), not by-value slots --
// so a caller-initialized member is rejected all the same; escapes and
// [[profiles::suppress]] are the remedies.
int test_byvalue_parameter_tracked(Agg p) {
  return p.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

int test_byvalue_parameter_assigned(Agg p) {
  p.m = 5;
  return p.m; // OK
}

int test_byvalue_parameter_escape(Agg p) {
  take_ref(p);
  return p.m; // OK: escaped
}

// Re-passing the parameter by value is an escape like any other bare use
// (the copy-constructor argument reference is not a tracked-copy DeclStmt).
void use_agg(Agg);
int test_byvalue_parameter_repassed(Agg p) {
  use_agg(p);
  return p.m; // OK: escaped
}

// A copy from a by-value parameter chains the tracking: the parameter
// starts unassigned, so the copy does too.
int test_copy_from_parameter(Agg other) {
  Agg d = other;
  return d.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// A reference parameter aliases the caller's own object -- not a copy --
// and stays untracked.
int test_reference_parameter_untracked(Agg &r) {
  return r.m; // OK
}

// A local that is itself [[uninit]]-marked is the parse-time rules'
// territory: the read-through check owns its subobject reads, and exactly one
// diagnostic fires.
int test_marked_local_owned_by_read_through() {
  Agg s [[uninit]];
  return s.m; // expected-error {{read of a subobject of an '[[uninit]]' object accesses uninitialized memory under profile 'std::init'}}
}

// A static local is zero-initialized, never tracked.
int test_static_local_untracked() {
  static Agg a;
  return a.m; // OK
}

// A member of an anonymous struct is reached through an IndirectFieldDecl
// chain, not a direct `a.m` access, so it is not tracked -- consistent with
// the anonymous-aggregate skips in the ctor-body pass and R5 (a known gap).
struct HasAnon {
  struct {
    int m [[uninit]];
  };
};
int test_anonymous_member_untracked() {
  HasAnon a;
  return a.m; // OK: known gap
}

// An array of aggregates is not tracked (element tracking is the deferred
// construct_at slice).
int test_array_of_aggregates_untracked() {
  Agg arr[2];
  return arr[0].m; // OK: known gap
}

// A union local is never tracked: the harvest rejects union types (their
// members are mutually exclusive, and [[uninit]] on a union member is banned
// by union_marker anyway -- suppressed on the function to build the fixture).
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "union_marker")]]
int test_union_local_untracked() {
  union U {
    int x [[uninit]];
  };
  U u = {1};
  return u.x; // OK
}

// std::byte members are exempt (paper §4.5), so a byte-only aggregate has
// nothing to track.
struct ByteBox {
  std::byte b [[uninit]];
};
std::byte test_byte_member_exempt() {
  ByteBox x;
  return x.b; // OK
}

void test_suppress_stmt() {
  Agg a;
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(std::init, rule: "uninit_read")]] {
    int v = a.m; // OK: suppressed
    (void)v;
  }
}

// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init)]]
int test_suppress_decl() {
  Agg a;
  return a.m; // OK: suppressed
}

// A lambda body's own locals are tracked when the lambda's call operator is
// analyzed.
void test_lambda_own_local() {
  auto f = [] {
    Agg a;
    return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
  };
  (void)f;
}

// A template function's body is analyzed per instantiation.
template <typename T>
int template_local_member_read() {
  Agg a;
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}
template int template_local_member_read<int>(); // expected-note {{in instantiation of function template specialization 'template_local_member_read<int>' requested here}}

// ============================================================
// Copies of tracked locals
// ============================================================

// A copy of a tracked local inherits the source's per-member state at the
// copy point -- a copy does not inherit initialization (paper §5.2), it
// inherits whatever state the source has -- so reading the copy's member is
// exactly as (in)valid as reading the source's was there.
int test_copy_read_before_source_assigned() {
  Agg a;
  Agg b = a;
  return b.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

int test_copy_after_source_assigned() {
  Agg a;
  a.m = 5;
  Agg b = a;
  return b.m; // OK: the source was assigned at the copy point
}

int test_copy_then_dest_assigned() {
  Agg a;
  Agg b = a;
  b.m = 1;
  return b.m; // OK
}

// The copy consumes the source ref without escaping it: the source keeps
// its own (unassigned) state.
int test_copy_keeps_source_tracked() {
  Agg a;
  Agg b = a;
  b.m = 1;
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// State transfers at the copy point, not later: a source assignment after
// the copy does not reach the copy.
int test_copy_point_state() {
  Agg a;
  Agg b = a;
  a.m = 5;
  return b.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// The all-branches rule (§1.2) applies through the copy.
int test_copy_after_branch(bool c) {
  Agg a;
  if (c)
    a.m = 5;
  Agg b = a;
  return b.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// State flows through a chain of copies (harvested to a fixpoint, so the
// chain resolves regardless of declaration order in the CFG's block list).
int test_copy_of_copy() {
  Agg a;
  a.m = 5;
  Agg b = a;
  Agg c = b;
  return c.m; // OK
}

int test_copy_of_copy_unassigned() {
  Agg a;
  Agg b = a;
  Agg c = b;
  return c.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// Move construction transfers state the same way (for these classes a move
// is a copy; the explicit-cast peel resolves the directly named source).
int test_move_construction() {
  Agg a;
  Agg b = static_cast<Agg &&>(a);
  return b.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// Paren and brace copy forms behave identically to the `=` form.
int test_copy_forms() {
  Agg a;
  a.m = 5;
  Agg b(a);
  Agg c{a};
  return b.m + c.m; // OK
}

// An escape of the copy credits the copy, like any tracked local.
int test_copy_escape() {
  Agg a;
  Agg b = a;
  take_ref(b);
  return b.m; // OK: escaped
}

// The destruction mirror of escape crediting: a [[now_uninit]] call kills
// the assigned bit of the tracked storage bound to its pointer/reference
// parameters -- destruction makes the storage uninitialized again
// ("Lifetimes", p4222r2.md:922-927) -- as a real dataflow fact, so a
// destroy on any considered-executed path spoils a read at the join
// ("Static analysis", p4222r2.md:306-312).
struct DestroyAgg {
  int m [[uninit]];  // expected-note 5 {{member 'm' declared here}}
  int sm [[uninit]]; // expected-note {{member 'sm' declared here}}
};
template <class T> [[now_uninit]] void destroy_at(T *);
[[now_uninit]] void wipe_whole(DestroyAgg &);

int test_destroy_then_read() {
  DestroyAgg a;
  a.m = 1;
  destroy_at(&a.m);
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

int test_destroy_under_branch(bool c) {
  DestroyAgg a;
  a.m = 1;
  if (c)
    destroy_at(&a.m);
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

int test_destroy_then_reassign() {
  DestroyAgg a;
  a.m = 1;
  destroy_at(&a.m);
  a.m = 2;
  return a.m; // OK: reassigned after the destroy
}

// Sibling leniency: the destroy call's base DeclRefExpr is still a
// non-benign escape of `a`, whose whole-range credit precedes the call
// element in block order -- the destroyed member nets to killed while its
// siblings keep the escape credit (the locals' documented leniency; the
// ctor-body pass has no such credit).
int test_destroy_sibling_keeps_escape_credit() {
  DestroyAgg a;
  a.m = 1;
  a.sm = 1;
  destroy_at(&a.m);
  return a.sm; // OK: the sibling keeps the escape credit
}

// Passing the whole object destroys every tracked member.
int test_destroy_whole_object() {
  DestroyAgg a;
  a.m = 1;
  a.sm = 1;
  destroy_at(&a);
  return a.m // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
         + a.sm; // expected-error {{member 'sm' is read before initialization under profile 'std::init'}}
}

int test_destroy_whole_object_by_reference() {
  DestroyAgg a;
  a.m = 1;
  wipe_whole(a);
  return a.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// A copy after a destroy projects the killed bit (paper §5.2: a copy
// inherits the source's state, not initialization).
int test_copy_after_destroy() {
  DestroyAgg a;
  a.m = 1;
  destroy_at(&a.m);
  DestroyAgg b = a;
  return b.m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
}

// A destroy whose argument is selected at run time (a conditional
// operator) is not resolved by the argument peel: no kill, and the base
// DeclRefExprs keep the escape credit -- a missed diagnostic, never a
// rejection, the same shape limit the [[now_init]] credit resolution has.
// (Both arms are pre-stored so the parse-time destroy_uninit rule stays
// silent on the call.)
int test_destroy_conditional_argument(bool c) {
  DestroyAgg a, b;
  a.m = 1;
  b.m = 1;
  destroy_at(c ? &a.m : &b.m);
  return a.m; // OK: unresolved destroy argument -- the escape credit stands
}
