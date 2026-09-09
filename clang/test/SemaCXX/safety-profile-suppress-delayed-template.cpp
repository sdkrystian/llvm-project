// Under -fdelayed-template-parsing a templated function definition is
// re-lexed at end of TU with every scope unwound; its tokens still lie in the
// dominions recorded when the definition was first lexed -- its own
// [[profiles::suppress]] attributes' and its enclosing classes' and
// namespaces'.

// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 -fdelayed-template-parsing %s
// Parity: the same shapes without the flag.
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 %s

[[profiles::enforce(test::type_cast)]];

// Declarator-id suppression on a function template.
template <class T>
void f [[profiles::suppress(test::type_cast)]] () {
  int *p = reinterpret_cast<int *>(0);
  (void)p;
}
void use_f() { f<int>(); }

// Suppression covering a constructor template's mem-initializer.
struct C {
  int *p;
  template <class T>
  C [[profiles::suppress(test::type_cast)]] (T) : p(reinterpret_cast<int *>(0)) {}
};
void use_c() { C c(1); }

// Suppression on the enclosing class of a member template (lexical walk).
struct [[profiles::suppress(test::type_cast)]] Encl {
  template <class T>
  int *m() {
    return reinterpret_cast<int *>(0);
  }
};
void use_e() {
  Encl e;
  (void)e.m<int>();
}

// An unsuppressed template still fires.
template <class T>
int *unsup() {
  return reinterpret_cast<int *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}
int *use_u = unsup<int>();

// Statement-level suppression inside a late-parsed body covers its own
// declaration only.
template <class T>
int *stmt_in_body() {
  [[profiles::suppress(test::type_cast)]] int *p = reinterpret_cast<int *>(0);
  int *q = reinterpret_cast<int *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  (void)q;
  return p;
}
int *use_s = stmt_in_body<int>();
