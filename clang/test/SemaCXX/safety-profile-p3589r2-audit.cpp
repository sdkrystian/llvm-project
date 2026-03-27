// Tests for bugs found during the P3589R2 audit.
//
// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// =====================================================================
// Bug 1: Empty declarations before enforce should be allowed.
// P3589R2 [decl.attr.enforce] para 1: "shall precede any non-empty-declaration"
// A bare `;` (empty-declaration) is not a non-empty-declaration.
// =====================================================================

;
[[profiles::enforce(std::type)]];

// =====================================================================
// Bug 2: Conflicting enforce on the same empty-declaration.
// P3589R2 [decl.attr.enforce] para 3: "exactly the same token sequences"
// =====================================================================

[[profiles::enforce(std::bounds), profiles::enforce(bounds)]]; // expected-error {{conflicting profile enforcement attributes for 'bounds'}}

// =====================================================================
// Bug 4: profiles::require not restricted to imports.
// P3589R2 [decl.attr.require] para 2: "shall appear only in a
// module-import-declaration"
// =====================================================================

[[profiles::require(std::type)]]; // expected-error {{'profiles::require' attribute only allowed on import declarations}}

// =====================================================================
// Bug 5: non-string-literal after justification: or rule:
// P3589R2 [decl.attr.suppress] para 2: "shall be a string-literal"
// =====================================================================

void test_bad_justification() {
  [[profiles::suppress(std::type, justification: 42)]] // expected-error {{expected string literal}}
  int x;
}

void test_bad_rule() {
  // P3589R2 [dcl.attr.grammar]: rule: accepts any non-comma-balanced-token.
  // A non-string value is accepted per the grammar but not stored; this
  // makes the suppress act as a whole-profile suppress rather than per-rule.
  [[profiles::suppress(std::type, rule: 42)]]
  int x;
}

// =====================================================================
// Verify uninit is diagnosed after enforce (confirming enforce works)
// =====================================================================

void test_enforce_works() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}
