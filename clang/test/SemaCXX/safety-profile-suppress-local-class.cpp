// A statement-level [[profiles::suppress]] must silence CFG-based rules
// inside a local class's member function: the method runs its CFG passes at
// ActOnFinishFunctionBody, mid-parse of the enclosing function, while the
// enclosing statement's ProfileSuppressScope is still live. The post-parse
// violation gate consults that live stack (dominion-checked) in addition to
// the analyzed function's own AST.

// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify=inert -fprofiles -std=c++23 -Wno-uninitialized %s
// RUN: %clang_cc1 -fsyntax-only -verify=no-profiles -std=c++23 -Wno-uninitialized %s

// inert-no-diagnostics

// no-profiles-warning@+1 {{'profiles::enforce' attribute ignored}}
[[profiles::enforce(test::uninit_read)]];

void suppressed_stmt() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] {
    struct L { int m() { int x; return x; } };
    L l;
    (void)l.m();
  }
}

// An unsuppressed sibling local class still fires: the live scope's dominion
// ended with the suppressed statement.
void unsuppressed_sibling() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] {
    struct L { int m() { int x; return x; } };
    L l;
    (void)l.m();
  }
  struct U {
    int m() {
      int x;    // expected-note {{variable 'x' is declared here}}
      return x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
    }
  };
  U u;
  (void)u.m();
}

// A local class inside a lambda inside the suppressed statement is covered
// too, however deep the nesting.
void lambda_in_suppressed_stmt() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] {
    auto l = [] {
      struct LL { int m() { int x; return x; } };
      LL ll;
      return ll.m();
    };
    (void)l();
  }
}

// Suppression written on the local class itself works through the lexical
// declaration chain.
void suppress_on_class() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  struct [[profiles::suppress(test::uninit_read)]] LC {
    int m() { int x; return x; }
  };
  LC c;
  (void)c.m();
}

// The same shapes inside a function template, checked at instantiation:
// suppression from the pattern is re-established around instantiation, and
// the unsuppressed sibling still fires there.
template <class T>
void tmpl() {
  // no-profiles-warning@+1 {{'profiles::suppress' attribute ignored}}
  [[profiles::suppress(test::uninit_read)]] {
    struct LT { int m() { T x; return x; } };
    LT l;
    (void)l.m();
  }
  struct UT {
    int m() {
      T x;      // expected-note {{variable 'x' is declared here}}
      return x; // expected-error {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
    }
  };
  // expected-note@-6 {{in instantiation of member function 'tmpl()::UT::m' requested here}}
  UT u;
  (void)u.m();
}
void instantiate() {
  tmpl<int>(); // expected-note {{in instantiation of function template specialization 'tmpl<int>' requested here}}
}
