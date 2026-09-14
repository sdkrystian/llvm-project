// A profile is live when it is enforced or a rule of it is enabled by a
// command-line option; a live profile's checks fire at every host as
// warnings without an enforcement.
// RUN: %clang_cc1 -fsyntax-only -verify=preview -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized -Wprofile-test-type-cast -Wprofile-test-class-final -Wprofile-test-ctor-final -Wprofile-test-uninit-read %s
// The umbrella group enables every profile's rules, so its runs leave out
// the read of an uninitialized local, which a real profile's rule fires on
// as well.
// RUN: %clang_cc1 -fsyntax-only -verify=preview -fprofiles -fprofiles-test-profiles -std=c++23 -Wprofiles -DUMBRELLA %s
// Without an option or an enforcement nothing is live.
// RUN: %clang_cc1 -fsyntax-only -verify=none -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized %s
// An inert test:: profile is never live.
// RUN: %clang_cc1 -fsyntax-only -verify=none -fprofiles -std=c++23 -Wno-uninitialized -Wprofile-test-type-cast -Wprofile-test-class-final -Wprofile-test-ctor-final -Wprofile-test-uninit-read %s
// RUN: %clang_cc1 -fsyntax-only -verify=none -fprofiles -std=c++23 -Wprofiles -DUMBRELLA %s
// -w silences a preview; there is no enforcement for it to survive.
// RUN: %clang_cc1 -fsyntax-only -verify=none -fprofiles -fprofiles-test-profiles -std=c++23 -w -Wprofiles %s
// A diagnostic pragma alone does not make a profile live.
// RUN: %clang_cc1 -fsyntax-only -verify=none -fprofiles -fprofiles-test-profiles -std=c++23 -Wno-uninitialized -DPRAGMA %s
// none-no-diagnostics

#ifdef PRAGMA
#pragma clang diagnostic warning "-Wprofiles"
#endif

// A reinterpret_cast: the test::type_cast expression site.
void type_cast(long l) {
  (void)reinterpret_cast<int *>(l); // preview-warning {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

// A completed class: the test::class_final class-completion funnel; its
// user-provided constructor: the test::ctor_final constructor funnel.
struct Finalized { // preview-warning {{test profile fired on completion of class 'Finalized' under profile 'test::class_final'}}
  int m;
  Finalized() : m(0) {} // preview-warning {{test profile fired on finalization of a constructor for class 'Finalized' under profile 'test::ctor_final'}}
};

// A read of an uninitialized local: the test::uninit_read CFG rider.
#ifndef UMBRELLA
int uninit_read() {
  int x; // preview-note {{variable 'x' is declared here}}
  return x; // preview-warning {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
}
#endif
