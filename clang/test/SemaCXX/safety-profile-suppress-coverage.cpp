// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++23 %s

// The suppression-coverage matrix: one test::type_cast violation per parser
// context that can carry [[profiles::suppress]], suppressed and unsuppressed.
// Most rows duplicate coverage in other test files; this file is the one
// place a new context's missing ProfileSuppressScope guard turns red (see
// SemaProfiles::ProfileSuppressScope).

[[profiles::enforce(test::type_cast)]];

// Enum-head suppression covers the enumerator initializers
enum [[profiles::suppress(test::type_cast)]] EnumHead {
  eh = (long)reinterpret_cast<long *>(0) != 0
};
enum EnumHeadControl {
  ehc = (long)reinterpret_cast<long *>(0) != 0 // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
};

// Enumerator suppression covers its own initializer, not a sibling's
enum Enumerator {
  en [[profiles::suppress(test::type_cast)]] =
      (long)reinterpret_cast<long *>(0) != 0,
  en_sibling = (long)reinterpret_cast<long *>(0) != 0 // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
};

// Variable initializer
[[profiles::suppress(test::type_cast)]] long var =
    (long)reinterpret_cast<long *>(0);
long var_control = (long)reinterpret_cast<long *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}

// Namespace
namespace [[profiles::suppress(test::type_cast)]] ns {
long inner = (long)reinterpret_cast<long *>(0);
}
namespace ns_control {
long inner = (long)reinterpret_cast<long *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

// Member declaration with NSDMI
struct Members {
  [[profiles::suppress(test::type_cast)]] long nsdmi =
      (long)reinterpret_cast<long *>(0);
  long nsdmi_control = (long)reinterpret_cast<long *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
};

// Immediate default argument (namespace scope, parsed in place)
void immediate_dfl([[profiles::suppress(test::type_cast)]] long p =
                       (long)reinterpret_cast<long *>(0));
void immediate_dfl_control(long p = (long)reinterpret_cast<long *>(0)); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}

// Late-parsed member default argument and late-parsed member body
struct LateParsed {
  void dfl([[profiles::suppress(test::type_cast)]] long p =
               (long)reinterpret_cast<long *>(0));
  void dfl_control(long p = (long)reinterpret_cast<long *>(0)); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  void body [[profiles::suppress(test::type_cast)]] () {
    long l = (long)reinterpret_cast<long *>(0);
    (void)l;
  }
  void body_control() {
    long l = (long)reinterpret_cast<long *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
    (void)l;
  }
};

// Condition variable
void condition_variable() {
  if ([[profiles::suppress(test::type_cast)]] long x =
          (long)reinterpret_cast<long *>(0))
    (void)x;
  if (long x = (long)reinterpret_cast<long *>(0)) // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
    (void)x;
}

// Lambda body under a statement suppression
void lambda_body() {
  [[profiles::suppress(test::type_cast)]] auto l = [] {
    return (long)reinterpret_cast<long *>(0);
  };
  auto l_control = [] {
    return (long)reinterpret_cast<long *>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  };
  (void)l;
  (void)l_control;
}
