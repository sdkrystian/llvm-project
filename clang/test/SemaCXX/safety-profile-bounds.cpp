// RUN: %clang_cc1 -fsyntax-only -verify=enforced -std=c++20 -fsafety-profile-enforce=bounds %s
// RUN: %clang_cc1 -fsyntax-only -verify=applied -std=c++20 -fsafety-profile-apply=bounds -Wsafety-profile-bounds %s

// --- Rule 5.1: Pointer arithmetic ---

void test_pointer_add() {
  int x = 0;
  int *p = &x;

  int *q = p + 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  int *r = 1 + p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  p += 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}
}

void test_pointer_sub() {
  int arr[2] = {1, 2};
  int *p = &arr[0];
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{array-to-pointer decay is discouraged by profile std::bounds}}
  int *q = &arr[1];
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{array-to-pointer decay is discouraged by profile std::bounds}}

  int *r = p - 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  auto diff = q - p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  p -= 1;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}
}

void test_pointer_incr_decr() {
  int x = 0;
  int *p = &x;

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  p--;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  ++p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  --p;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

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
  // applied-warning@-2 {{array-to-pointer decay is discouraged by profile std::bounds}}

  int *q = &arr[0];
  // enforced-error@-1 {{array-to-pointer decay is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{array-to-pointer decay is discouraged by profile std::bounds}}

  // String literals are excluded
  const char *s = "hello";
}

// --- Suppression ---

void test_suppress() {
  int x = 0;
  int *p = &x;

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}

  {
    [[profiles::suppress(std::bounds)]];
    p++;
    int arr[10] = {};
    int *q = arr;
  }

  p++;
  // enforced-error@-1 {{pointer arithmetic is not allowed when profile std::bounds is enforced}}
  // applied-warning@-2 {{pointer arithmetic is discouraged by profile std::bounds}}
}

// --- Unevaluated contexts ---

void test_unevaluated() {
  int x = 0;
  int *p = &x;
  (void)sizeof(p + 1);
}
