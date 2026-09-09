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
/// the diagnostic-group naming rule, and the profile-name-convention
/// policies.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_PROFILES_H
#define LLVM_CLANG_BASIC_PROFILES_H

#include "clang/Basic/CharInfo.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
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
};

/// A profile enforced by [[profiles::enforce]]: the profile name plus the
/// canonical spelling of the designator that enforced it (P3589R2
/// [decl.attr.enforce]p3 compares repeated enforcements by their spelling).
/// Shared by Sema's enforcement list, Module's advertised set, and the
/// serialized PCH record.
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

/// True if \p Name spells a profile-name (P3589R2 [decl.attr.grammar]:
/// identifiers joined by "::"), the form -fprofiles-enforce= accepts.
inline bool isValidProfileName(llvm::StringRef Name) {
  llvm::SmallVector<llvm::StringRef, 4> Parts;
  Name.split(Parts, "::");
  return llvm::all_of(
      Parts, [](llvm::StringRef Part) { return isValidAsciiIdentifier(Part); });
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

/// The diagnostic group of \p Rule of the profile named \p Profile, or of
/// the whole profile when \p Rule is empty: "profile-<profile>-<rule>" in
/// lowercase, with "::", "_", and "." spelled "-" (test::arith / zero_divide
/// is profile-test-arith-zero-divide). A name the implementation does not
/// know yields a group that does not exist, which the diagnostics engine
/// ignores, so an unknown profile enforces and suppresses nothing.
inline std::string getProfileDiagGroupName(llvm::StringRef Profile,
                                           llvm::StringRef Rule) {
  std::string Name = "profile";
  auto Append = [&](llvm::StringRef Part) {
    Name += '-';
    for (char C : Part) {
      if (C == ':' || C == '_' || C == '.') {
        if (Name.back() != '-')
          Name += '-';
      } else {
        Name += toLowercase(C);
      }
    }
  };
  Append(Profile);
  if (!Rule.empty())
    Append(Rule);
  return Name;
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
/// A template over the argument representation, which the parser instantiates
/// with its own owning-string argument type.
template <typename ArgumentT>
std::string getCanonicalProfileArgumentSpelling(const ArgumentT &Argument) {
  if (!Argument.isNamed())
    return std::string(Argument.Value);
  return (llvm::Twine(Argument.Key) + " : " + Argument.Value).str();
}

} // namespace clang::profiles

#endif // LLVM_CLANG_BASIC_PROFILES_H
