// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++20 %s

// ===================================================================
// Enforce on empty-declaration at TU scope: OK
// ===================================================================
[[profiles::enforce(test::type_cast)]]; // #enforce1

// ===================================================================
// Multiple different profiles enforced: OK
// ===================================================================
[[profiles::enforce(test::bounds)]];

// ===================================================================
// Enforce: exact repetition is OK
// ===================================================================
[[profiles::enforce(test::type_cast)]];

// ===================================================================
// Enforce: mismatch is an error
// ===================================================================
[[profiles::enforce(test::type_cast(strict: true))]]; // expected-error {{repeated enforcement of profile 'test::type_cast' with different designator}} \
                                                      // expected-note@#enforce1 {{previous attribute is here}}

// ===================================================================
// Enforce after a non-empty declaration: error
// ===================================================================
void some_function(); // #some_function
[[profiles::enforce(test::new_profile)]]; // expected-error {{'profiles::enforce' attribute on empty-declaration must precede all non-empty declarations}} \
                                          // expected-note@#some_function {{declaration declared here}}

// ===================================================================
// Enforce on non-empty declaration (function): error
// ===================================================================
[[profiles::enforce(test::type_cast)]] // expected-error {{'profiles::enforce' attribute only allowed on empty-declarations and module-declarations}}
void enforced_func();

// ===================================================================
// Enforce inside class scope: error
// ===================================================================
struct EnforceInClass {
  [[profiles::enforce(test::type_cast)]]; // expected-warning {{declaration does not declare anything}} \
                                        // expected-error {{'profiles::enforce' attribute on empty-declaration must be at translation unit scope}}
  // A member declaration that declares nothing is still not an
  // empty-declaration, so the complaint is about the form rather than the
  // scope. (At namespace scope the parser rejects the attribute list first.)
  [[profiles::enforce(test::type_cast)]] int; // expected-warning {{declaration does not declare anything}} \
                                             // expected-error {{'profiles::enforce' attribute only allowed on empty-declarations and module-declarations}}
};

// ===================================================================
// Enforce inside a namespace: error
// ===================================================================
namespace ns {
  [[profiles::enforce(test::type_cast)]]; // expected-error {{'profiles::enforce' attribute on empty-declaration must be at translation unit scope}}
}

// ===================================================================
// Require not on import: error
// ===================================================================
[[profiles::require(test::type_cast)]]; // expected-error {{'profiles::require' attribute only allowed on module-import-declarations}}

// The designator-list form parses; the placement error is still the only
// complaint.
[[profiles::require(test::type_cast, test::flow(strict: true))]]; // expected-error {{'profiles::require' attribute only allowed on module-import-declarations}}

// ===================================================================
// Suppress on declarations
// ===================================================================
[[profiles::suppress(test::type_cast)]]
int suppressed_var;

[[profiles::suppress(test::type_cast)]]
void suppressed_func();

// ===================================================================
// Suppress on statements
// ===================================================================
void test_stmt_suppress() {
  [[profiles::suppress(test::type_cast)]] int *x = reinterpret_cast<int*>(0);
  [[profiles::suppress(test::type_cast)]] { int *y = reinterpret_cast<int*>(0); }
  int *z = reinterpret_cast<int*>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

// ===================================================================
// Suppress with non-string justification: error
// ===================================================================
[[profiles::suppress(test::type_cast, justification: legacy)]] // expected-error {{'justification' argument of 'profiles::suppress' must be a string literal}}
void bad_justification();

// ===================================================================
// Enforce at block scope: error (this is a null-statement at block
// scope, so enforce cannot appertain to it)
// ===================================================================
void test_block_scope() {
  [[profiles::enforce(test::type_cast)]]; // expected-error {{'profiles::enforce' attribute cannot be applied to a statement}}
}

// ===================================================================
// Diagnostic fires when profile IS enforced
// ===================================================================
void test_enforced_profile_errors() {
  int *p = reinterpret_cast<int*>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

// ===================================================================
// Suppress on additional declaration kinds
// ===================================================================
enum [[profiles::suppress(test::type_cast)]] SuppressedEnum { SE_A, SE_B };

using SuppressedAlias [[profiles::suppress(test::type_cast)]] = int;

// ===================================================================
// Suppress on a definition's declarator-id covers the whole definition
// (mem-initializers and body), for free functions and out-of-line
// members alike; it ends with the definition.
// ===================================================================
void suppressed_free_def [[profiles::suppress(test::type_cast)]] () {
  int *p = reinterpret_cast<int *>(0); // OK: suppressed
  (void)p;
}

void after_suppressed_def() {
  int *p = reinterpret_cast<int *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  (void)p;
}

struct OutOfLine {
  void ool();
  OutOfLine();
  int *m;
};
void OutOfLine::ool [[profiles::suppress(test::type_cast)]] () {
  int *p = reinterpret_cast<int *>(0); // OK: suppressed
  (void)p;
}
OutOfLine::OutOfLine [[profiles::suppress(test::type_cast)]] ()
    : m(reinterpret_cast<int *>(0)) { // OK: the mem-init is covered too
  int *p = reinterpret_cast<int *>(0); // OK: suppressed
  (void)p;
}

// Inline members, inline constructors (mem-init and body), variable
// initializers, and NSDMIs were already covered by their own scopes.
struct InlineSuppressPins {
  int *n [[profiles::suppress(test::type_cast)]] = reinterpret_cast<int *>(0); // OK
  void inline_member [[profiles::suppress(test::type_cast)]] () {
    int *p = reinterpret_cast<int *>(0); // OK: suppressed
    (void)p;
  }
  InlineSuppressPins [[profiles::suppress(test::type_cast)]] ()
      : n(reinterpret_cast<int *>(0)) { // OK
    int *p = reinterpret_cast<int *>(0); // OK: suppressed
    (void)p;
  }
};
int *suppressed_var_init [[profiles::suppress(test::type_cast)]] =
    reinterpret_cast<int *>(0); // OK
