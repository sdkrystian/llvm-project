// RUN: %clang_cc1 -fsyntax-only -verify=enforced -std=c++20 -fsafety-profile-enforce=bounds %s

// --- Rule 5.1: Pointer arithmetic ---

void test_pointer_add() {
  int x = 0;
  int *p = &x;

  int *q = p + 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  int *r = 1 + p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  p += 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

void test_pointer_sub() {
  int arr[2] = {1, 2};
  int *p = &arr[0];
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
  int *q = &arr[1];
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}

  int *r = p - 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  auto diff = q - p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  p -= 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

void test_pointer_incr_decr() {
  int x = 0;
  int *p = &x;

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  p--;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  ++p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  --p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  // Non-pointer increment is fine
  int i = 0;
  i++;
  ++i;
}

// --- Rule 5.2: Array-to-pointer decay ---

void test_array_decay() {
  int arr[10] = {};

  int *p = arr;
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}

  int *q = &arr[0];
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}

  // String literals also undergo array-to-pointer decay
  const char *s = "hello";
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
}

// --- Suppression ---

void test_suppress() {
  int x = 0;
  int *p = &x;

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}

  [[profiles::suppress(std::bounds)]] {
    p++;
    int arr[10] = {};
    int *q = arr;
  }

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
}

// --- Unevaluated contexts ---

void test_unevaluated() {
  int x = 0;
  int *p = &x;
  (void)sizeof(p + 1);
}
