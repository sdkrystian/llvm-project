// A declaration's prefix [[profiles::suppress]] dominion is the whole
// declaration's token range (P3589R2 s2.4p3), including a class or enum
// defined in its decl-specifier-seq. The scope is pushed by ParseDeclGroup's
// callers before the decl-specifier-seq is parsed, so parse-time rules firing
// during the member-specification parse are covered, as are a file-scope
// class's late-parsed NSDMIs and method bodies (they parse inside
// ParseCXXMemberSpecification, within the hoisted scope's lifetime).

// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized %s

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::type_cast)]];
// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::uninit_read)]];

// File scope: the NSDMI, a late-parsed method body, and the declarator's
// initializer are all inside the declaration's dominion.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(test::type_cast)]] struct S {
  int *p = reinterpret_cast<int *>(0);
  int *m() { return reinterpret_cast<int *>(8); }
} s = {reinterpret_cast<int *>(0)};

// A CFG-based rule violation in a method body of the declared class: the
// method's CFG passes run during the late parse, while the hoisted scope is
// still live (found through the live-stack consult of the post-parse gate).
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(test::uninit_read)]] struct SCfg {
  int m() {
    int x;
    return x;
  }
} scfg;

// The dominion ends with the declaration: a following declaration of the
// same shape still fires.
struct After {
  int *p = reinterpret_cast<int *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
} after;

// An enum defined in the decl-specifier-seq: enumerator initializers parse
// eagerly, within the hoisted scope.
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(test::type_cast)]] enum E {
  A = (bool)reinterpret_cast<int *>(8)
} e;

enum EAfter {
  B = (bool)reinterpret_cast<int *>(8) // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
} eafter;

// Class scope: a member declaration's prefix suppression covers a tag
// defined in its decl-specifier-seq.
struct Outer {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::type_cast)]] enum E2 {
    C = (bool)reinterpret_cast<int *>(8)
  } e2;

  enum E2After {
    D = (bool)reinterpret_cast<int *>(8) // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  } e2after;

  // A nested class's NSDMIs and method bodies late-parse at the outermost
  // class's closing brace; the member declaration's dominion covers them by
  // position.
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::type_cast)]] struct Inner {
    int *p = reinterpret_cast<int *>(0);
    int *m() { return reinterpret_cast<int *>(8); }
  } inner;
};

// Block scope: the statement and declaration guards both push the same
// attributes; the duplicate entries are harmless.
void block_scope() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::type_cast)]] struct BS {
    int *p = reinterpret_cast<int *>(0);
    int *m() { return reinterpret_cast<int *>(8); }
  } bs = {};
  (void)bs;

  struct BSAfter {
    int *q = reinterpret_cast<int *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  } bs_after;
  (void)bs_after;
}

// A function template's prefix suppression covers its body.
template <class T>
// no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
[[profiles::suppress(test::type_cast)]] int *tf() {
  return reinterpret_cast<int *>(0);
}
int *use_tf = tf<int>();

template <class T>
int *tg() {
  return reinterpret_cast<int *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}
int *use_tg = tg<int>();
