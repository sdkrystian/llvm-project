//===--- CGProfiles.cpp - C++ profiles framework in CodeGen ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements the CodeGen side of the C++ profiles framework
/// (P3589R2): tracking which [[profiles::suppress]] entries cover the code
/// being emitted. See clang/docs/ProfilesFrameworkInternals.rst.
///
//===----------------------------------------------------------------------===//

#include "CGDebugInfo.h"
#include "CodeGenFunction.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Profiles.h"

using namespace clang;
using namespace clang::CodeGen;

void CodeGenFunction::ProfileSuppressionScope::addFromStmt(const Stmt *S) {
  if (!CGF.getLangOpts().Profiles)
    return;
  profiles::forEachSuppression(
      S, [&](const Decl *Owner, const ProfilesSuppressAttr &A) {
        CGF.ProfileStmtSuppressions.push_back(
            {A.getProfileName(), A.getRule(),
             Owner ? profiles::declaratorDominion(*Owner) : SourceRange()});
        return false;
      });
}

void CodeGenFunction::ProfileSuppressionScope::addFromDecl(const Decl *D) {
  // The attribute check is a fast path: nearly every local variable emitted
  // through here carries no attributes at all, and one that does carry a
  // suppression already pushed under an enclosing EmitDeclStmt merely pushes
  // a harmless duplicate.
  if (!CGF.getLangOpts().Profiles || !D || !D->hasAttrs())
    return;
  profiles::forEachSuppression(
      D, /*WalkLexicalParents=*/false,
      [&](const Decl &Owner, const ProfilesSuppressAttr &A) {
        CGF.ProfileStmtSuppressions.push_back(
            {A.getProfileName(), A.getRule(),
             profiles::declaratorDominion(Owner)});
        return false;
      });
}

bool CodeGenFunction::isProfileSuppressionActive(StringRef Profile,
                                                 StringRef Rule,
                                                 SourceLocation Loc) const {
  assert(ProfileSuppressionFloor <= ProfileStmtSuppressions.size() &&
         "suppression floor points past the end of the stack");
  // The declaration side of the query is the anchor when one is set (the code
  // being emitted belongs to that declaration's construct, not to the
  // function hosting the emission) and CurCodeDecl otherwise. Lambda call
  // operators carry their enclosing scopes' suppressions as implicit
  // attributes, so the chain walk recovers statement-level suppression around
  // a lambda; a null CurCodeDecl (a synthesized helper such as a block
  // copy/dispose function) carries none.
  return profiles::isSuppressed(
      {llvm::ArrayRef(ProfileStmtSuppressions)
           .drop_front(ProfileSuppressionFloor),
       ProfileSuppressionAnchor ? ProfileSuppressionAnchor : CurCodeDecl},
      Profile, Rule, Loc, getContext().getSourceManager());
}

void CodeGenFunction::EmitProfileRuntimeCheck(
    StringRef Profile, StringRef Rule, unsigned TrapDiagID, SourceLocation Loc,
    llvm::function_ref<llvm::Value *()> BuildPassed) {
  if (!getLangOpts().Profiles)
    return;
  // Enforcement first, so a TU that does not enforce the profile never pays
  // the suppression walk. The location-aware query keeps code before the
  // enforcement -- global-module-fragment functions emitted after the purview
  // is parsed, or from a BMI -- outside the dominion.
  if (!getContext().isProfileEnforcedAt(Profile, Loc) ||
      getContext().isProfileExemptSystemHeaderLoc(Loc) ||
      isProfileSuppressionActive(Profile, Rule, Loc))
    return;
  // The check fires: only now build the site's "no violation" predicate, so
  // an inactive site emits no IR at all.
  llvm::Value *Passed = BuildPassed();
  // Only pay for building the trap reason when EmitTrapCheck will encode it:
  // it reads the reason under the Detailed -fsanitize-debug-trap-reasons mode
  // only, and encoding it requires debug info. With the reason empty or
  // unavailable the trap falls back to the handler's generic message.
  TrapReason TR;
  if (CGM.getCodeGenOpts().getSanitizeDebugTrapReasons() ==
          CodeGenOptions::SanitizeDebugTrapReasonKind::Detailed &&
      getDebugInfo())
    CGM.BuildTrapReason(TrapDiagID, TR) << Profile;
  // Point the trap at the checked operation. At -O0 EmitTrapCheck never
  // merges trap blocks, keeping location and reason exact per check site;
  // optimized builds coalesce the traps of one handler kind, merging their
  // locations, like UBSan's trap mode.
  ApplyDebugLocation ADL(*this, Loc);
  EmitTrapCheck(Passed, SanitizerHandler::ProfileViolation, /*NoMerge=*/false,
                &TR);
}
