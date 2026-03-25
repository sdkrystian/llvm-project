//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___PROFILES_INDEX_IN_RANGE_H
#define _LIBCPP___PROFILES_INDEX_IN_RANGE_H

#include <__config>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

#if _LIBCPP_STD_VER >= 20

// P3081R2 [utility.index_in_range]
_LIBCPP_BEGIN_NAMESPACE_STD

template <class _Cont, class _Idx>
  requires requires(const _Cont& __a) {
    { std::as_const(__a)[2] };
  }
constexpr bool index_in_range(const _Cont& __a, const _Idx& __b) noexcept
  requires(requires { std::ssize(__a); } && is_signed_v<_Idx>) ||
          (requires { std::size(__a); } && is_unsigned_v<_Idx>)
{
  if constexpr (is_signed_v<_Idx>) {
    return __b >= 0 && __b < std::ssize(__a);
  } else {
    return __b < std::size(__a);
  }
}

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP_STD_VER >= 20

#endif // _LIBCPP___PROFILES_INDEX_IN_RANGE_H
