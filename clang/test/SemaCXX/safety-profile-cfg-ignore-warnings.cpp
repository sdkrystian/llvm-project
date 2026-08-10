// Profile rules that ride the CFG analyses emit errors, so they must run
// even when the analysis-based *warning* pipeline is skipped: -w disables
// warnings, not errors. The profile-only rerun must not resurrect ordinary
// -Wuninitialized warnings either (the self-init and sometimes-uninitialized
// shapes below would produce them; -verify rejects any unexpected
// diagnostic).
// RUN: %clang_cc1 -fsyntax-only -verify=expected -fprofiles -std=c++23 -w %s
// The post-error path must behave the same under -w: a leading unrelated TU
// error routes every later function through the post-error rerun.
// RUN: %clang_cc1 -fsyntax-only -verify=expected,err -fprofiles -std=c++23 -w -DLEADING_ERROR %s
// Without -fprofiles, -w silences everything: the profile-only pass must not
// run when no profile is enforced.
// RUN: %clang_cc1 -fsyntax-only -verify=quiet -std=c++23 -w %s

// quiet-no-diagnostics

[[profiles::enforce(std::init)]];

#ifdef LEADING_ERROR
int leading_unrelated_error = undeclared_identifier;
// err-error@-1 {{use of undeclared identifier 'undeclared_identifier'}}
#endif

// Repro A: a member read before initialization in a constructor body.
struct X {
  int m [[uninit]]; // expected-note {{member 'm' declared here}}
  X() {
    int y = m; // expected-error {{member 'm' is read before initialization under profile 'std::init'}}
    (void)y;
  }
};

// A local [[uninit]] read: fires through the uninitialized-variables
// analysis with the ProfileOnly reporter.
void local_uninit_read() {
  int u [[uninit]]; // expected-note {{variable 'u' is declared here}}
  int v = u;        // expected-error {{variable 'u' is read before initialization under profile 'std::init'}}
  (void)v;
}

// Would draw -Wuninitialized (self-init) without -w; under -w only the
// profile error may appear.
void self_init() {
  int x =    // expected-note {{variable 'x' is declared here}}
      x;     // expected-error {{variable 'x' is read before initialization under profile 'std::init'}}
  (void)x;
}

// Would draw -Wsometimes-uninitialized without -w; under -w only the profile
// error may appear.
void sometimes_uninit(bool c) {
  int w [[uninit]]; // expected-note {{variable 'w' is declared here}}
  if (c)
    w = 1;
  int z = w; // expected-error {{variable 'w' is read before initialization under profile 'std::init'}}
  (void)z;
}

// No violation: no diagnostics under -w.
void clean(bool c) {
  int a [[uninit]];
  a = c ? 1 : 2;
  int b = a;
  (void)b;
}
