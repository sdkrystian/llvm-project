//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <__config>
#include <__profiles/detection_mode.h>
#include <cstdlib>

// P3081R2 [dcl.attr.profile]: default replaceable profile violation handler.
// The default behavior terminates in an implementation-defined manner.
_LIBCPP_BEGIN_UNVERSIONED_NAMESPACE_STD

[[gnu::weak]] void profile_violation(detection_mode) noexcept {
  std::abort();
}

_LIBCPP_END_UNVERSIONED_NAMESPACE_STD
