// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 -fsafety-profile-enforce=arithmetic %s

// --- Attribute parsing ---

[[profiles::enforce(std::arithmetic)]] void enforced_fn();

[[profiles::suppress(std::arithmetic)]] void suppressed_fn();

[[profiles::enforce(std::unknown)]] void bad_fn(); // expected-error {{unknown safety profile 'unknown'}}

// --- Scoping: suppress within a compound statement ---

void test_suppress() {
  int s = 42;
  unsigned u = s;
  // expected-error@-1 {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}

  {
    [[profiles::suppress(std::arithmetic)]];
    unsigned v = s;
  }

  unsigned w = s;
  // expected-error@-1 {{implicit conversion changes signedness from 'int' to 'unsigned int' when profile std::arithmetic is enforced}}
}
