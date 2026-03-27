// Tests for P3589R2 profile-designator argument parsing and grammar.
//
// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// =================================================================
// G1: Enforce with profile-argument-list
// P3589R2 [dcl.attr.grammar]: profile-designator may include a
// parenthesized profile-argument-list. Arguments are collected and
// included in the designator for duplicate detection.
// =================================================================

// expected-error@+1 {{unknown safety profile 'acme::hardened(fortify : 3 , sanitize : thread)'}}
[[profiles::enforce(acme::hardened(fortify: 3, sanitize: thread))]];

// expected-error@+1 {{unknown safety profile 'acme::ruleset("CoreCheck")'}}
[[profiles::enforce(acme::ruleset("CoreCheck"))]];

// Known profile names with arguments are resolved by base name.
// expected-error@+1 {{unknown safety profile 'std::lib::hardened(fortify : 3)'}}
[[profiles::enforce(std::lib::hardened(fortify: 3))]];

// =================================================================
// G1: Require with profile-argument-list
// P3589R2 [dcl.attr.require] para 1: require also takes a
// profile-designator which may include arguments.
// =================================================================

// expected-error@+1 {{'profiles::require' attribute only allowed on import declarations}}
[[profiles::require(acme::type(level: 3))]];

// =================================================================
// G2: Suppress with bare non-operator-non-punctuator-token
// P3589R2 [dcl.attr.grammar] profile-argument production
// =================================================================

[[profiles::enforce(std::type)]];

void test_suppress_bare_identifier() {
  [[profiles::suppress(std::type, warning)]]
  int x = 0;
}

void test_suppress_bare_literal() {
  [[profiles::suppress(std::type, "some note")]]
  int x = 0;
}

// =================================================================
// G2: Suppress with non-string rule: value
// P3589R2 [decl.attr.suppress] para 4: rule: takes
// non-comma-balanced-token (not limited to string-literal).
// =================================================================

void test_suppress_rule_non_string() {
  [[profiles::suppress(std::type, rule: 42)]]
  int x;
}

void test_suppress_unknown_key() {
  [[profiles::suppress(std::type, level: "high", justification: "testing")]]
  int x = 0;
}

// =================================================================
// Verify enforcement still works after the grammar tests above.
// =================================================================

void test_enforce_active() {
  int x; // expected-error {{uninitialized variable declaration is not allowed when profile std::type is enforced}}
}
