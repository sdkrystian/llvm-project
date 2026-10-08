// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++23 %s
// RUN: %clang_cc1 -fsyntax-only -verify -fprofiles -fprofiles-test-profiles -std=c++23 -fdelayed-template-parsing %s

// A suppression dominion that begins in a token-cached body, default
// argument, or member initializer is recorded when that region is parsed,
// after the dominions of the constructs that follow it; those keep their
// extent.

[[profiles::enforce(test::type_cast)]];

#define RC (void)reinterpret_cast<int *>(0L)

// Members after a late-parsed statement suppression keep their dominions
struct Members {
  void late() { [[profiles::suppress(test::type_cast)]] RC; }
  void declarator_id [[profiles::suppress(test::type_cast)]] () { RC; }
  [[profiles::suppress(test::type_cast)]] int *nsdmi = reinterpret_cast<int *>(0L);
  [[profiles::suppress(test::type_cast)]] void prefix() { RC; }
  struct [[profiles::suppress(test::type_cast)]] Nested {
    void k() { RC; }
  };
  void control() { RC; } // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
};

// Members after a late-parsed default argument's suppression keep their dominions
struct DefaultArgument {
  void f(int x = [] {
    [[profiles::suppress(test::type_cast)]] RC;
    return 0;
  }());
  void g [[profiles::suppress(test::type_cast)]] () { RC; }
  void control() { RC; } // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
};

// Members after a late-parsed member initializer's suppression keep their dominions
struct MemberInitializer {
  int x = [] {
    [[profiles::suppress(test::type_cast)]] RC;
    return 0;
  }();
  void g [[profiles::suppress(test::type_cast)]] () { RC; }
  void control() { RC; } // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
};

// Code after the class keeps the enforced mapping
void after_members() {
  RC; // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}

// A diagnostic pragma lexed when a body was token-cached lies in a late-parsed statement's dominion
struct CachedPragma {
  void inside() {
    [[profiles::suppress(test::type_cast)]] {
#pragma clang diagnostic warning "-Wunused-variable"
      RC;
    }
    RC; // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  }
  void remaps() {
#pragma clang diagnostic push
    [[profiles::suppress(test::type_cast)]] {
      RC;
#pragma clang diagnostic error "-Wprofile-test-type-cast"
      RC; // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
    }
    RC; // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
#pragma clang diagnostic pop
  }
  void pragma_after() {
    [[profiles::suppress(test::type_cast)]] (void)0;
#pragma clang diagnostic warning "-Wunused-variable"
    RC; // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
  }
  void member [[profiles::suppress(test::type_cast)]] () { RC; }
  void control() { RC; } // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
};

// A template after a statement suppression in an earlier template keeps its dominion
template <class T> void late_template() {
  [[profiles::suppress(test::type_cast)]] RC;
}
template <class T> void later_template [[profiles::suppress(test::type_cast)]] () {
  RC;
}
template <class T> void later_control() {
  RC; // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
}
void use_templates() {
  late_template<int>();
  later_template<int>();
  later_control<int>();
}
