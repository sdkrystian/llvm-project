//===--- CGProfiles.cpp - C++ profiles framework in CodeGen ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements the CodeGen side of the C++ profiles framework
/// (P3589R2): the emission of pattern-5 runtime checks. See
/// clang/docs/ProfilesFrameworkInternals.rst.
///
//===----------------------------------------------------------------------===//

#include "CGDebugInfo.h"
#include "CodeGenFunction.h"
#include "clang/AST/ASTContext.h"

using namespace clang;
using namespace clang::CodeGen;

bool CodeGenFunction::EmitProfileRuntimeCheck(
    StringRef Profile, unsigned TrapDiagID, SourceLocation Loc,
    llvm::function_ref<llvm::Value *()> BuildPassed) {
  if (!getLangOpts().Profiles)
    return false;
  // The positional gate keeps code before the enforcement --
  // global-module-fragment functions emitted after the purview is parsed, or
  // from a BMI -- outside the dominion, and honors a suppression wherever
  // the checked tokens are emitted from.
  if (!getContext().isProfileRuleActiveAt(TrapDiagID, Loc))
    return false;
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
  return true;
}
