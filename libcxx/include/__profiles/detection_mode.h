//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___PROFILES_DETECTION_MODE_H
#define _LIBCPP___PROFILES_DETECTION_MODE_H

#include <__config>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

// P3081R2 [support.contracts.detection]
_LIBCPP_BEGIN_UNVERSIONED_NAMESPACE_STD
enum class detection_mode {
  type,
  bounds,
  lifetime,
};
_LIBCPP_END_UNVERSIONED_NAMESPACE_STD

#endif // _LIBCPP___PROFILES_DETECTION_MODE_H
