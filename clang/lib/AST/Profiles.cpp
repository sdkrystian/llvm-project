//===--- Profiles.cpp - C++ profiles suppression walks ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// The C++ profiles framework's [[profiles::suppress]] enumeration over
/// declarations and statements. See clang/include/clang/AST/Profiles.h.
///
//===----------------------------------------------------------------------===//

#include "clang/AST/Profiles.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

bool profiles::forEachSuppression(
    const Decl *D, bool WalkLexicalParents,
    llvm::function_ref<bool(const Decl &, const ProfilesSuppressAttr &)>
        Callback) {
  while (D) {
    for (const auto *PSA : D->specific_attrs<ProfilesSuppressAttr>())
      if (Callback(*D, *PSA))
        return true;
    if (!WalkLexicalParents)
      return false;
    const DeclContext *DC = D->getLexicalDeclContext();
    D = DC ? dyn_cast<Decl>(DC) : nullptr;
  }
  return false;
}

bool profiles::forEachSuppression(
    const Stmt *S,
    llvm::function_ref<bool(const Decl *, const ProfilesSuppressAttr &)>
        Callback) {
  if (const auto *AS = dyn_cast_or_null<AttributedStmt>(S)) {
    for (const Attr *A : AS->getAttrs())
      if (const auto *PSA = dyn_cast<ProfilesSuppressAttr>(A))
        if (Callback(nullptr, *PSA))
          return true;
    return false;
  }
  // [[profiles::suppress]] on a local variable attaches to the VarDecl, not
  // the enclosing DeclStmt; enumerate every declaration of the group with its
  // owner so a consumer can bound each entry to its declarator's dominion.
  if (const auto *DS = dyn_cast_or_null<DeclStmt>(S))
    for (const Decl *D : DS->decls())
      if (forEachSuppression(
              D, /*WalkLexicalParents=*/false,
              [&](const Decl &Owner, const ProfilesSuppressAttr &A) {
                return Callback(&Owner, A);
              }))
        return true;
  return false;
}

SourceRange profiles::declaratorDominion(const Decl &D) {
  if (const auto *DD = dyn_cast<DeclaratorDecl>(&D))
    return SourceRange(DD->getLocation(), DD->getSourceRange().getEnd());
  return SourceRange();
}

bool profiles::dominionCovers(SourceRange Dominion, SourceLocation Loc,
                              const SourceManager &SM) {
  // Compare raw TU token order: expansion-loc normalization would collapse a
  // macro expansion's tokens onto the invocation and over-suppress
  // (isBeforeInTranslationUnit rejects invalid locations).
  SourceLocation Begin = Dominion.getBegin(), End = Dominion.getEnd();
  return Loc.isInvalid() || Begin.isInvalid() ||
         (!SM.isBeforeInTranslationUnit(Loc, Begin) &&
          (End.isInvalid() || !SM.isBeforeInTranslationUnit(End, Loc)));
}

bool profiles::isSuppressedFor(const Decl *D, llvm::StringRef Profile,
                               llvm::StringRef Rule) {
  return forEachSuppression(D, /*WalkLexicalParents=*/true,
                            [&](const Decl &, const ProfilesSuppressAttr &A) {
                              return suppressionMatches(A.getProfileName(),
                                                        A.getRule(), Profile,
                                                        Rule);
                            });
}

bool profiles::isSuppressedFor(const Stmt *S, llvm::StringRef Profile,
                               llvm::StringRef Rule, SourceLocation UseLoc,
                               const SourceManager &SM) {
  return forEachSuppression(
      S, [&](const Decl *Owner, const ProfilesSuppressAttr &A) {
        if (!suppressionMatches(A.getProfileName(), A.getRule(), Profile, Rule))
          return false;
        return !Owner || dominionCovers(declaratorDominion(*Owner), UseLoc, SM);
      });
}
