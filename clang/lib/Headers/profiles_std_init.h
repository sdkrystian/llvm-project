//===--- profiles_std_init.h - std::now_init for std::init ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares std::now_init, the identity function of the std::init profile
/// (P4222R2 §4.4), for standard libraries that do not provide it. See
/// clang/docs/ProfilesFramework.rst.
///
//===----------------------------------------------------------------------===//

#ifndef __PROFILES_STD_INIT_H
#define __PROFILES_STD_INIT_H

#if defined(__cplusplus) && __cplusplus >= 201103L
namespace std {
/// Returns \p __p, whose referent the caller asserts initialized. The body
/// binds the marked parameter to the unmarked return, so it is suppressed for
/// builds that judge system headers.
template <class _Tp>
[[profiles::suppress(std::init)]] constexpr _Tp *
now_init(_Tp *__p [[ref_to_uninit]]) noexcept {
  return __p;
}
} // namespace std
#endif

#endif // __PROFILES_STD_INIT_H
