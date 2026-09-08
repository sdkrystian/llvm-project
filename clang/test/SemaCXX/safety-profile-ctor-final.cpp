// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 %s

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::ctor_final)]];

// A constructor with a member-initializer list fires once, at finalization.
struct Written {
  int x;
  Written() : x(0) {} // expected-error {{test profile fired on finalization of a constructor for class 'Written' under profile 'test::ctor_final'}}
};

// A constructor with no member-initializer list reaches the same dispatch.
struct NoList {
  int x;
  NoList() { x = 0; } // expected-error {{test profile fired on finalization of a constructor for class 'NoList' under profile 'test::ctor_final'}}
};

// Out-of-line definitions fire at the definition, not the declaration.
struct OutOfLine {
  OutOfLine();
};
OutOfLine::OutOfLine() {} // expected-error {{test profile fired on finalization of a constructor for class 'OutOfLine' under profile 'test::ctor_final'}}

// An in-class defaulted constructor has no definition of its own to reach
// the dispatch (it is not user-provided).
struct Defaulted {
  int x;
  Defaulted() = default;
};

// An out-of-line '= default' *is* the constructor's definition
// (user-provided per [class.default.ctor]) and dispatches there, like a
// written body.
struct DefaultedOutOfLine {
  int x;
  DefaultedOutOfLine();
};
DefaultedOutOfLine::DefaultedOutOfLine() = default; // expected-error {{test profile fired on finalization of a constructor for class 'DefaultedOutOfLine' under profile 'test::ctor_final'}}

// The dispatch is uniform over constructor kinds: an out-of-line defaulted
// copy or move constructor is equally user-provided and fires too (a
// profile that must not see them filters in its own callback).
struct DefaultedCopyOutOfLine {
  int x;
  DefaultedCopyOutOfLine();
  DefaultedCopyOutOfLine(const DefaultedCopyOutOfLine &);
  DefaultedCopyOutOfLine(DefaultedCopyOutOfLine &&);
};
DefaultedCopyOutOfLine::DefaultedCopyOutOfLine() = default; // expected-error {{test profile fired on finalization of a constructor for class 'DefaultedCopyOutOfLine' under profile 'test::ctor_final'}}
DefaultedCopyOutOfLine::DefaultedCopyOutOfLine(const DefaultedCopyOutOfLine &) = default; // expected-error {{test profile fired on finalization of a constructor for class 'DefaultedCopyOutOfLine' under profile 'test::ctor_final'}}
DefaultedCopyOutOfLine::DefaultedCopyOutOfLine(DefaultedCopyOutOfLine &&) = default; // expected-error {{test profile fired on finalization of a constructor for class 'DefaultedCopyOutOfLine' under profile 'test::ctor_final'}}

// A class with no user-declared constructor never reaches the dispatch.
struct NoCtor {
  int x;
};

// A delegating constructor leaves member initialization to its target, so it
// is skipped; the target constructor fires.
struct Delegating {
  int x;
  Delegating() : Delegating(0) {}
  Delegating(int v) : x(v) {} // expected-error {{test profile fired on finalization of a constructor for class 'Delegating' under profile 'test::ctor_final'}}
};

// A dependent constructor pattern is skipped; the diagnostic fires on the
// instantiation.
template <typename T>
struct Tmpl {
  T x;
  Tmpl() : x() {} // expected-error {{test profile fired on finalization of a constructor for class 'Tmpl<int>' under profile 'test::ctor_final'}}
};
template struct Tmpl<int>; // expected-note {{in instantiation of member function 'Tmpl<int>::Tmpl' requested here}}

// A constructor rule is never applied to an uninstantiated class template.
template <typename T>
struct NeverInstantiatedCtor {
  int a;
  NeverInstantiatedCtor() {}
};

// Suppression on the constructor.
struct SuppressedCtor {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::ctor_final)]] SuppressedCtor() {}
};

// Suppression on the enclosing class.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
struct [[profiles::suppress(test::ctor_final)]] SuppressedClass {
  SuppressedClass() {}
};

// Rule-targeted suppression (the profile has a single implicit rule).
struct SuppressedByRule {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::ctor_final, rule: "")]] SuppressedByRule() {}
};

// A non-matching suppress does not silence the diagnostic.
struct WrongSuppress {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::other)]] WrongSuppress() {} // expected-error {{test profile fired on finalization of a constructor for class 'WrongSuppress' under profile 'test::ctor_final'}}
};
