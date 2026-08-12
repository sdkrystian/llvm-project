//===--- Profiles.h - C++ profiles framework helpers ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared value types and helpers of the C++ profiles framework (P3589R2):
/// profile arguments and their canonical spelling, enforced-profile records,
/// the suppression-matching rule, and the profile-name-convention policies.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_PROFILES_H
#define LLVM_CLANG_BASIC_PROFILES_H

#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include <string>

namespace clang::profiles {

/// How a profile argument was written: positionally, or as a key: value
/// pair.
enum class ProfileArgumentKind : unsigned {
  Positional = 0,
  Named = 1,
};

/// One argument of a profile-designator or [[profiles::suppress]] attribute.
struct ProfileArgument {
  llvm::StringRef Key;
  llvm::StringRef Value;
  ProfileArgumentKind Kind = ProfileArgumentKind::Positional;
  SourceRange Range;

  bool isNamed() const { return Kind == ProfileArgumentKind::Named; }
};

/// A profile enforced by [[profiles::enforce]]: the profile name plus the
/// canonical spelling of the designator that enforced it (P3589R2
/// [decl.attr.enforce]p3 compares repeated enforcements by their spelling).
/// Shared by Sema's enforcement list, Module's exported enforcement set, and
/// the serialized PCH record.
struct EnforcedProfile {
  std::string ProfileName;
  std::string Designator;
};

/// An enforcement recorded on the translation unit: the enforced profile plus
/// the location of the [[profiles::enforce]] that recorded it (invalid for an
/// enforcement restored from an AST file). The element type of ASTContext's
/// enforcement list.
struct ProfileEnforcement : EnforcedProfile {
  SourceLocation EnforceLoc;
};

/// True if a [[profiles::suppress]] entry naming \p EntryProfile /
/// \p EntryRule suppresses a violation of \p Rule of \p Profile: the profile
/// names must agree, and the entry either names the violated rule or names no
/// rule at all (suppressing the whole profile). The one matching rule shared
/// by every consumer of suppression state -- Sema's parse-time suppress stack
/// and the post-parse AST walks (clang/AST/Profiles.h).
inline bool suppressionMatches(llvm::StringRef EntryProfile,
                               llvm::StringRef EntryRule,
                               llvm::StringRef Profile, llvm::StringRef Rule) {
  return EntryProfile == Profile && (EntryRule.empty() || EntryRule == Rule);
}

/// True if the profile named \p Name is inert in this compilation: the
/// built-in test:: profiles only exercise the framework and their rules
/// never fire unless the test suite opts in via -fprofiles-test-profiles
/// (callers pass LangOptions::ProfilesTestProfiles as
/// \p TestProfilesEnabled). See ProfilesFrameworkInternals.rst, "Test
/// Profiles".
inline bool isProfileNameInert(llvm::StringRef Name, bool TestProfilesEnabled) {
  return !TestProfilesEnabled && Name.starts_with("test::");
}

/// P3589R2 [decl.attr.enforce]p5: profiles are compatible if they are the
/// same -- by name; arguments configure a profile without changing its
/// identity -- or proclaimed compatible by the implementation. "All standard
/// profiles are compatible with each other" is the one proclamation modeled
/// here.
inline bool areProfilesCompatible(llvm::StringRef A, llvm::StringRef B) {
  return A == B || (A.starts_with("std::") && B.starts_with("std::"));
}

/// The canonical spelling of a profile argument: the value token for a
/// positional argument, "key : value" for a named one. Enforcement identity
/// (P3589R2 [decl.attr.enforce]p3) compares designators by this spelling.
/// A template over the argument representation so it serves both
/// ProfileArgument and the parser's owning-string argument type.
template <typename ArgumentT>
std::string getCanonicalProfileArgumentSpelling(const ArgumentT &Argument) {
  if (!Argument.isNamed())
    return std::string(Argument.Value);
  return (llvm::Twine(Argument.Key) + " : " + Argument.Value).str();
}

} // namespace clang::profiles

#endif // LLVM_CLANG_BASIC_PROFILES_H
