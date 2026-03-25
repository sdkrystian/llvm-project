//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___PROFILES_NARROW_H
#define _LIBCPP___PROFILES_NARROW_H

#include <__config>
#include <__type_traits/is_arithmetic.h>
#include <__type_traits/is_base_of.h>
#include <__type_traits/is_constructible.h>
#include <__type_traits/is_convertible.h>
#include <__type_traits/is_reference.h>
#include <__type_traits/is_signed.h>
#include <typeinfo>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

// P3081R2 [utility.narrow]
// Mandates: To is not a reference type, is_constructible_v<To, From> is true,
// and either converting From to To is an implicit conversion or To is a base
// class of From.
// Note: The paper also mandates "the argument to from is not a
// constant-expression", which requires compiler support and is not enforced here.
_LIBCPP_BEGIN_NAMESPACE_STD

template <class _To, class _From>
  requires(!is_reference_v<_To> && is_constructible_v<_To, const _From&> &&
           (is_convertible_v<const _From&, _To> || is_base_of_v<_To, _From>))
constexpr _To narrow(const _From& __from) {
  _To __to = static_cast<_To>(__from);
  if (static_cast<_From>(__to) != __from)
    throw bad_cast();
  if constexpr (is_arithmetic_v<_To> && is_arithmetic_v<_From>) {
    if constexpr (is_signed_v<_To> != is_signed_v<_From>) {
      if ((__to < _To{}) != (__from < _From{}))
        throw bad_cast();
    }
  }
  return __to;
}

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___PROFILES_NARROW_H
