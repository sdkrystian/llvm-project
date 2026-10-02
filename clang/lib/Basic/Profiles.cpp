//===--- Profiles.cpp - C++ profiles registry -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// The profiles registry: each profile and named rule the implementation
/// knows, with its diagnostic group, generated from the ProfileGroup and
/// ProfileRuleGroup records of DiagnosticGroups.td.
///
//===----------------------------------------------------------------------===//

#include "clang/Basic/Profiles.h"
#include "clang/Basic/DiagnosticCategories.h"
#include "clang/Basic/DiagnosticIDs.h"

using namespace clang;

/// Every profile and named rule of DiagnosticGroups.td.
static constexpr profiles::ProfileRegistryName RegistryNames[] = {
#define GET_PROFILE_TABLE
#define PROFILE(NAME, GROUP) {NAME, ""},
#define PROFILE_RULE(PROFILE, RULE, GROUP) {PROFILE, RULE},
#include "clang/Basic/DiagnosticGroups.inc"
#undef PROFILE_RULE
#undef PROFILE
#undef GET_PROFILE_TABLE
};

/// The diagnostic group of each entry of RegistryNames.
static constexpr diag::Group RegistryGroups[] = {
#define GET_PROFILE_TABLE
#define PROFILE(NAME, GROUP) diag::Group::GROUP,
#define PROFILE_RULE(PROFILE, RULE, GROUP) diag::Group::GROUP,
#include "clang/Basic/DiagnosticGroups.inc"
#undef PROFILE_RULE
#undef PROFILE
#undef GET_PROFILE_TABLE
};

std::optional<llvm::StringRef>
profiles::getProfileDiagGroupName(llvm::StringRef Profile,
                                  llvm::StringRef Rule) {
  for (auto [Name, Group] : llvm::zip_equal(RegistryNames, RegistryGroups))
    if (Name.Profile == Profile && Name.Rule == Rule)
      return DiagnosticIDs::getWarningOptionForGroup(Group);
  return std::nullopt;
}
