// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 %s

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(std::init)]];

int runtime();   // expected-note {{declared here}}
                 // no-profiles-note@-1 {{declared here}}
constexpr int compile_time() { return 7; }

int g_const = 0;
int g_constexpr = compile_time();
int g_array_const[3] = {1, 2, 3};
int g_runtime = runtime();              // expected-error {{non-local variable 'g_runtime' requires constant initialization under profile 'std::init'}}
int g_runtime_array[3] = {1, runtime(), 3}; // expected-error {{non-local variable 'g_runtime_array' requires constant initialization under profile 'std::init'}}

struct Trivial { int x; };
struct WithDtor { ~WithDtor(); };
struct WithCtor { WithCtor(); };

int g_scalar;
Trivial g_trivial;
Trivial g_trivial_braced = {};
WithDtor g_with_dtor;
WithCtor g_with_ctor;                   // expected-error {{non-local variable 'g_with_ctor' requires constant initialization under profile 'std::init'}}

thread_local int t_ns = runtime();      // OK: thread storage duration, not static

constinit int g_ci = 0;
// The constinit hard error fires regardless of -fprofiles.
constinit int g_ci_runtime = runtime();
// expected-error@-1 {{variable does not have a constant initializer}}
// expected-note@-2 {{required by 'constinit' specifier here}}
// expected-note@-3 {{non-constexpr function 'runtime' cannot be used in a constant expression}}
// no-profiles-error@-4 {{variable does not have a constant initializer}}
// no-profiles-note@-5 {{required by 'constinit' specifier here}}
// no-profiles-note@-6 {{non-constexpr function 'runtime' cannot be used in a constant expression}}

namespace inside {
  int n_runtime = runtime();            // expected-error {{non-local variable 'n_runtime' requires constant initialization under profile 'std::init'}}
}

void test_locals() {
  int x = runtime();
  static int s = runtime();
  thread_local int t = runtime();
  (void)x; (void)s; (void)t;
}

// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "static_runtime_init")]]
int g_suppressed = runtime();

// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init)]]
int g_suppressed_all = runtime();

// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(test::other)]]
int g_wrong_suppress = runtime();       // expected-error {{non-local variable 'g_wrong_suppress' requires constant initialization under profile 'std::init'}}

// A suppress on the enclosing namespace covers its variables (found by the
// declaration's lexical-parent walk).
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
namespace [[profiles::suppress(std::init)]] suppressed_ns {
int n_ok = runtime(); // OK: suppressed by the namespace-level attribute
}

// std::init / static_marker: a static or thread-local is zero-initialized by
// language rule, so marking it [[uninit]] is a contradiction (paper section
// 4.2: "an initialized object marked [[uninit]] is an error"). The
// with-initializer case stays uninit_with_initializer's (a real initializer
// already contradicts the marker); this rule covers the zero-initialized,
// no-initializer case.

namespace std { enum class byte : unsigned char {}; }

// The paper's section 4.2 'int glob2 [[uninit]]' example, plus the explicit
// 'static' and 'thread_local' spellings.
int g_marked [[uninit]];                      // expected-error {{'[[uninit]]' cannot be applied to variable 'g_marked' with static storage duration under profile 'std::init'; it is zero-initialized}}
static int g_static_marked [[uninit]];        // expected-error {{'[[uninit]]' cannot be applied to variable 'g_static_marked' with static storage duration under profile 'std::init'; it is zero-initialized}}
thread_local int g_thread_marked [[uninit]];  // expected-error {{'[[uninit]]' cannot be applied to variable 'g_thread_marked' with thread storage duration under profile 'std::init'; it is zero-initialized}}

// std::byte fires too: a static std::byte is zero-initialized, unlike an
// automatic one (which uninit_decl exempts because it may be left
// indeterminate).
std::byte g_byte_marked [[uninit]];           // expected-error {{'[[uninit]]' cannot be applied to variable 'g_byte_marked' with static storage duration under profile 'std::init'; it is zero-initialized}}

// A trivial aggregate at static storage: default-init is a no-op, but the
// object is still zero-initialized, so the marker fires.
Trivial g_agg_marked [[uninit]];              // expected-error {{'[[uninit]]' cannot be applied to variable 'g_agg_marked' with static storage duration under profile 'std::init'; it is zero-initialized}}

// A vector's default-init is vacuous like a scalar's (the indeterminacy walk
// counts vector types like scalars), so a static one draws static_marker.
typedef int v4 __attribute__((vector_size(16)));
v4 g_vec_marked [[uninit]];                   // expected-error {{'[[uninit]]' cannot be applied to variable 'g_vec_marked' with static storage duration under profile 'std::init'; it is zero-initialized}}

// A static pointer / union is owned by pointer_marker / union_marker (they fire
// regardless of storage duration), not static_marker -- exactly one diagnostic.
union U { int x; float y; };
static int *g_ptr_marked [[uninit]];   // expected-error {{'[[uninit]]' cannot be applied to a pointer under profile 'std::init'; initialize the pointer (for example to 'nullptr')}}
static U g_union_marked [[uninit]];    // expected-error {{'[[uninit]]' cannot be applied to a variable of union type under profile 'std::init'}}

// Arrays of pointers / unions key on the base element type: still owned by
// pointer_marker / union_marker, not static_marker (one diagnostic each).
[[uninit]] static int *g_ptr_arr_marked[2]; // expected-error {{'[[uninit]]' cannot be applied to a pointer under profile 'std::init'; initialize the pointer (for example to 'nullptr')}}
[[uninit]] static U g_union_arr_marked[2];  // expected-error {{'[[uninit]]' cannot be applied to a variable of union type under profile 'std::init'}}

// Member pointers are pointer_marker's too (paper section 4.1) -- exactly one
// diagnostic at static storage duration, like the object pointer above.
struct MPOwner { int m; };
static int MPOwner::*g_mp_marked [[uninit]]; // expected-error {{'[[uninit]]' cannot be applied to a pointer under profile 'std::init'; initialize the pointer (for example to 'nullptr')}}

// Unmarked statics / thread-locals are fine (zero-initialized, nothing to mark).
int g_unmarked;
static int g_static_unmarked;
thread_local int g_thread_unmarked;
std::byte g_byte_unmarked;

void test_local_statics_marked() {
  static int s [[uninit]];        // expected-error {{'[[uninit]]' cannot be applied to variable 's' with static storage duration under profile 'std::init'; it is zero-initialized}}
  thread_local int t [[uninit]];  // expected-error {{'[[uninit]]' cannot be applied to variable 't' with thread storage duration under profile 'std::init'; it is zero-initialized}}
  static int s_ok;
  thread_local int t_ok;
  // Automatic storage is uninit_decl's domain, not static_marker: the marker
  // is the accepted way to leave an automatic variable uninitialized.
  int a [[uninit]];
  (void)s; (void)t; (void)s_ok; (void)t_ok; (void)a;
}

// A static [[uninit]] *with* an initializer is uninit_with_initializer's (R4);
// static_marker must not add a second diagnostic (one error on the line).
int g_marked_with_init [[uninit]] = 0; // expected-error {{variable 'g_marked_with_init' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}}

void test_local_static_running_ctor() {
  // The synthesized constructor call initializes the object, so this stays
  // uninit_with_initializer (R4) -- in its no-written-initializer wording;
  // static_marker stays silent (one error).
  static WithCtor w [[uninit]]; // expected-error {{variable 'w' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'WithCtor' does not leave it uninitialized}} \
                                // expected-note {{default-initialization of 'WithCtor' runs a constructor}}
  (void)w;
}

struct MixedAgg { int x; WithCtor s; };
void test_local_static_mixed() {
  // A default-initialization that is not a no-op (a member's user-provided
  // constructor runs) initializes the object too, so the mixed aggregate
  // SWITCHES to uninit_with_initializer; static_marker stays silent -- the
  // shared vacuity guard keeps the pair complementary (exactly one error).
  static MixedAgg m [[uninit]]; // expected-error {{variable 'm' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'MixedAgg' does not leave it uninitialized}} \
                                // expected-note {{default-initialization of 'MixedAgg' runs a constructor}}
  (void)m;
}

// At namespace scope the same case additionally draws the independent
// static_runtime_init (the member's constructor is a runtime initializer) --
// a pre-existing pairing, not a static_marker double.
MixedAgg g_mixed_marked [[uninit]]; // expected-error {{variable 'g_mixed_marked' cannot be marked '[[uninit]]' under profile 'std::init'; default-initialization of its type 'MixedAgg' does not leave it uninitialized}} \
                                    // expected-note {{default-initialization of 'MixedAgg' runs a constructor}} \
                                    // expected-error {{non-local variable 'g_mixed_marked' requires constant initialization under profile 'std::init'}}

// Suppression: rule-targeted and whole-profile.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "static_marker")]] int g_marker_suppressed_rule [[uninit]];
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init)]] int g_marker_suppressed_all [[uninit]];

// The marker is rejected at the declaration that writes it, defining or not:
// an extern declaration and an in-class static data member declaration are
// zero-initialized entities like any other static.
extern int g_extern_marked [[uninit]];        // expected-error {{'[[uninit]]' cannot be applied to variable 'g_extern_marked' with static storage duration under profile 'std::init'; it is zero-initialized}}
struct InClassStatic {
  static int s [[uninit]];                    // expected-error {{'[[uninit]]' cannot be applied to variable 's' with static storage duration under profile 'std::init'; it is zero-initialized}}
};

// A redeclaration that merely inherits the marker is not diagnosed again --
// in either direction, and not on an out-of-line definition either.
extern int g_redecl_first [[uninit]];         // expected-error {{'[[uninit]]' cannot be applied to variable 'g_redecl_first' with static storage duration under profile 'std::init'; it is zero-initialized}}
extern int g_redecl_first;
extern int g_redecl_second;
extern int g_redecl_second [[uninit]];        // expected-error {{'[[uninit]]' cannot be applied to variable 'g_redecl_second' with static storage duration under profile 'std::init'; it is zero-initialized}}
int InClassStatic::s;

// A marked extern declaration *with* an initializer is a definition and
// stays uninit_with_initializer's (exactly one of the pair fires).
extern int g_extern_with_init [[uninit]] = 0; // expected-error {{variable 'g_extern_with_init' cannot be both '[[uninit]]' and have an initializer under profile 'std::init'}} \
                                              // expected-warning {{'extern' variable has an initializer}} \
                                              // no-profiles-warning {{'extern' variable has an initializer}}

// Suppression covers the non-defining host like the defining one.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(std::init, rule: "static_marker")]] extern int g_extern_suppressed [[uninit]];

// A class template's static data member classifies at instantiation (the
// pattern's marker is a fresh clone there, not an inherited one).
template <typename T>
struct TemplateStatic {
  static T s [[uninit]]; // expected-error {{'[[uninit]]' cannot be applied to variable 's' with static storage duration under profile 'std::init'; it is zero-initialized}}
};
template struct TemplateStatic<int>; // expected-note {{in instantiation of template class 'TemplateStatic<int>' requested here}}

// A profile rule fires on the instantiation, not the template pattern: a
// dependent static is diagnosed once, at instantiation.
template <typename T>
void template_static_marked() {
  static T s [[uninit]]; // expected-error {{'[[uninit]]' cannot be applied to variable 's' with static storage duration under profile 'std::init'; it is zero-initialized}}
  (void)s;
}
template void template_static_marked<int>(); // expected-note {{in instantiation of function template specialization 'template_static_marked<int>' requested here}}

// An uninstantiated template pattern is not yet a phase-7 entity, so no rule
// fires on it (no expected diagnostic here).
template <typename T>
void template_static_never_instantiated() {
  static T s [[uninit]];
  (void)s;
}
