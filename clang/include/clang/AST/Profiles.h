//===--- Profiles.h - C++ profiles suppression walks ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// The single implementation of the C++ profiles framework's suppression
/// enumeration: given a declaration or a statement, enumerate the
/// [[profiles::suppress]] attributes that cover it. Pure Decl/Attr/Stmt
/// logic with no Sema dependency, so every consumer of suppression state
/// shares one walk instead of reimplementing it.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_PROFILES_H
#define LLVM_CLANG_AST_PROFILES_H

#include "clang/Basic/Profiles.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

class Decl;
class ProfilesSuppressAttr;
class Stmt;

namespace profiles {

/// Enumerate the [[profiles::suppress]] attributes on \p D and, when
/// \p WalkLexicalParents is set, on each declaration up its lexical
/// declaration-context chain. \p Callback receives each attribute together
/// with the declaration that owns it (so a caller tracking per-construct
/// state -- e.g. a dominion range -- can compute it from the owner) and
/// returns true to stop the enumeration. Returns true if the callback
/// stopped it. A null \p D is a no-op.
bool forEachSuppression(
    const Decl *D, bool WalkLexicalParents,
    llvm::function_ref<bool(const Decl &Owner, const ProfilesSuppressAttr &)>
        Callback);

/// Enumerate the [[profiles::suppress]] attributes carried by the statement
/// node \p S itself: the attributes of an AttributedStmt, or the attributes
/// of each declaration of a DeclStmt ([[profiles::suppress]] on a local
/// variable attaches to the VarDecl, not the enclosing statement, and it
/// covers the whole declaration group). Other statements carry none. Does
/// not ascend to enclosing statements -- ancestry is the caller's to walk
/// (parent maps are context-specific). \p Callback returns true to stop the
/// enumeration; returns true if it did. A null \p S is a no-op.
bool forEachSuppression(
    const Stmt *S, llvm::function_ref<bool(const ProfilesSuppressAttr &)> Callback);

/// True if \p D or a lexical parent carries a [[profiles::suppress]]
/// matching \p Profile / \p Rule.
bool isSuppressedFor(const Decl *D, llvm::StringRef Profile,
                     llvm::StringRef Rule);

/// True if the statement node \p S carries a [[profiles::suppress]] matching
/// \p Profile / \p Rule (see the Stmt enumerator for what a node carries).
bool isSuppressedFor(const Stmt *S, llvm::StringRef Profile,
                     llvm::StringRef Rule);

} // namespace profiles
} // namespace clang

#endif // LLVM_CLANG_AST_PROFILES_H
