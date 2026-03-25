//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___PROFILES_INDIRECTION_NOT_NULL_H
#define _LIBCPP___PROFILES_INDIRECTION_NOT_NULL_H

#include <__config>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

// P3081R2 [utility.indirection_not_null]
_LIBCPP_BEGIN_NAMESPACE_STD

template <typename _Ptr>
  requires requires(const _Ptr& __p) {
    *__p;
    { __p != _Ptr{} } -> convertible_to<bool>;
  }
constexpr bool indirection_not_null(const _Ptr& __p) noexcept {
  return __p != _Ptr{};
}

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___PROFILES_INDIRECTION_NOT_NULL_H
