// std::init's rules preview as warnings under -Wprofile-std-init without an
// enforcement, at every host: a declaration site, the
// constructor-finalization funnel, the CFG rider, and an expression site.
// RUN: %clang_cc1 -fsyntax-only -verify=preview -fprofiles -std=c++23 -Wno-uninitialized -Wprofile-std-init %s
// A rule's own option makes the profile live but maps only that rule.
// RUN: %clang_cc1 -fsyntax-only -verify=decl -fprofiles -std=c++23 -Wno-uninitialized -Wprofile-std-init-uninit-decl %s
// Without an option or an enforcement nothing fires.
// RUN: %clang_cc1 -fsyntax-only -verify=none -fprofiles -std=c++23 -Wno-uninitialized %s
// -w silences the preview; there is no enforcement for it to survive.
// RUN: %clang_cc1 -fsyntax-only -verify=none -fprofiles -std=c++23 -w -Wprofile-std-init %s
// none-no-diagnostics

// An uninitialized automatic variable: the uninit_decl declaration site.
void uninit_decl() {
  int x; // preview-warning {{variable 'x' must be initialized or marked '[[uninit]]' under profile 'std::init'}} decl-warning {{variable 'x' must be initialized or marked '[[uninit]]' under profile 'std::init'}}
}

// A user-provided constructor leaving a member uninitialized: the
// constructor-finalization funnel.
struct Ctor {
  int m; // preview-note {{member 'm' declared here}}
  Ctor() {} // preview-warning {{constructor does not initialize member 'm' under profile 'std::init'}}
};

// A read of an [[uninit]] local before assignment: the CFG rider.
int uninit_read() {
  int x [[uninit]]; // preview-note {{variable 'x' is declared here}}
  return x; // preview-warning {{variable 'x' is read before initialization under profile 'std::init'}}
}

// A pointer bound to uninitialized memory without [[ref_to_uninit]]: the
// ref_to_uninit expression site. Static [[uninit]] storage is the source
// (a local's binding is the CFG rider's), with static_marker suppressed.
[[profiles::suppress(std::init, rule: "static_marker")]] [[uninit]] int g_uninit;
void ref_to_uninit() {
  int *p = &g_uninit; // preview-warning {{pointer to uninitialized memory must be marked '[[ref_to_uninit]]' under profile 'std::init'}}
}
