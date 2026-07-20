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
#include "clang/AST/DeclBase.h"
#include "clang/AST/Stmt.h"

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
    llvm::function_ref<bool(const ProfilesSuppressAttr &)> Callback) {
  if (const auto *AS = dyn_cast_or_null<AttributedStmt>(S)) {
    for (const Attr *A : AS->getAttrs())
      if (const auto *PSA = dyn_cast<ProfilesSuppressAttr>(A))
        if (Callback(*PSA))
          return true;
    return false;
  }
  // [[profiles::suppress]] on a local variable attaches to the VarDecl, not
  // the enclosing DeclStmt; enumerate every declaration of the group so the
  // attribute covers the whole declaration statement.
  if (const auto *DS = dyn_cast_or_null<DeclStmt>(S))
    for (const Decl *D : DS->decls())
      if (forEachSuppression(D, /*WalkLexicalParents=*/false,
                             [&](const Decl &, const ProfilesSuppressAttr &A) {
                               return Callback(A);
                             }))
        return true;
  return false;
}

bool profiles::isSuppressedFor(const Decl *D, llvm::StringRef Profile,
                               llvm::StringRef Rule) {
  return forEachSuppression(
      D, /*WalkLexicalParents=*/true,
      [&](const Decl &, const ProfilesSuppressAttr &A) {
        return suppressionMatches(A.getProfileName(), A.getRule(), Profile,
                                  Rule);
      });
}

bool profiles::isSuppressedFor(const Stmt *S, llvm::StringRef Profile,
                               llvm::StringRef Rule) {
  return forEachSuppression(S, [&](const ProfilesSuppressAttr &A) {
    return suppressionMatches(A.getProfileName(), A.getRule(), Profile, Rule);
  });
}
