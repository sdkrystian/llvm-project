//===--- ProfileState.h - Safety profile state tracking ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_PROFILESTATE_H
#define LLVM_CLANG_BASIC_PROFILESTATE_H

#include <cstdint>

namespace clang {

enum class ProfileKind : uint8_t {
  Type,
  Bounds,
  Lifetime,
  Arithmetic,
  NumProfiles
};

enum class ProfileMode : uint8_t {
  None = 0,
  Enforced = 1,
};

class ProfileState {
  static constexpr unsigned NumProfiles =
      static_cast<unsigned>(ProfileKind::NumProfiles);

  // 1 bit per profile for mode (None/Enforced).
  uint8_t Modes = 0;
  // 1 bit per profile for scope-local suppression.
  uint8_t Suppressed = 0;

  static unsigned idx(ProfileKind P) { return static_cast<unsigned>(P); }

public:
  ProfileState() = default;

  ProfileMode getMode(ProfileKind P) const {
    return ((Modes >> idx(P)) & 1) ? ProfileMode::Enforced : ProfileMode::None;
  }

  void setMode(ProfileKind P, ProfileMode M) {
    if (M == ProfileMode::Enforced)
      Modes |= (1u << idx(P));
    else
      Modes &= ~(1u << idx(P));
  }

  bool isSuppressed(ProfileKind P) const {
    return (Suppressed >> idx(P)) & 1;
  }

  void setSuppressed(ProfileKind P, bool V = true) {
    if (V)
      Suppressed |= (1u << idx(P));
    else
      Suppressed &= ~(1u << idx(P));
  }

  bool isEnforced(ProfileKind P) const {
    return ((Modes >> idx(P)) & 1) && !isSuppressed(P);
  }

  bool isEnabled(ProfileKind P) const {
    return isEnforced(P);
  }
};

} // namespace clang

#endif // LLVM_CLANG_BASIC_PROFILESTATE_H
