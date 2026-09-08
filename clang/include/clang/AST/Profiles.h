//===--- Profiles.h - C++ profiles suppression walks ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared suppression walks for the C++ profiles framework: given a
/// declaration or a statement, enumerate the [[profiles::suppress]]
/// attributes that cover it. Pure Decl/Attr/Stmt logic with no Sema
/// dependency.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_PROFILES_H
#define LLVM_CLANG_AST_PROFILES_H

#include "clang/Basic/Profiles.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

class Decl;
class ParentMap;
class ProfilesSuppressAttr;
class SourceManager;
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
/// variable attaches to the VarDecl, not the enclosing statement). \p Owner
/// is null for an AttributedStmt entry and the owning declaration for a
/// DeclStmt entry, so a consumer can bound the entry to its declarator's
/// dominion (see declaratorDominion). Other statements carry none. Does not
/// ascend to enclosing statements -- ancestry is the caller's to walk
/// (parent maps are context-specific). \p Callback returns true to stop the
/// enumeration; returns true if it did. A null \p S is a no-op.
bool forEachSuppression(
    const Stmt *S,
    llvm::function_ref<bool(const Decl *Owner, const ProfilesSuppressAttr &)>
        Callback);

/// The token range a declaration's suppression covers within its declaration
/// group (P3589R2 §2.4p3): the declarator-id through the end
/// of the declarator's initializer for a DeclaratorDecl, an invalid range
/// otherwise (consumers fail open).
SourceRange declaratorDominion(const Decl &D);

/// True if \p Loc is within \p Dominion in raw TU token order, failing open
/// on any invalid location on either side: an entry with no computable
/// dominion covers every location, and synthesized code keeps plain
/// suppression behavior. The one containment predicate shared by every
/// dominion-checked suppression consumer; see ProfilesFrameworkInternals.rst,
/// "Suppression Dominion Mechanics".
bool dominionCovers(SourceRange Dominion, SourceLocation Loc,
                    const SourceManager &SM);

/// True if an entry of \p Entries (innermost last) suppresses \p Rule of
/// \p Profile at \p Loc: the entry matches (suppressionMatches) and its
/// dominion covers \p Loc (dominionCovers). The one stack loop shared by
/// Sema's parse-time suppress stack and CodeGen's statement-suppression
/// stack.
bool anyEntryCovers(llvm::ArrayRef<SuppressionEntry> Entries,
                    llvm::StringRef Profile, llvm::StringRef Rule,
                    SourceLocation Loc, const SourceManager &SM);

/// True if \p D or a lexical parent carries a [[profiles::suppress]]
/// matching \p Profile / \p Rule.
bool isSuppressedFor(const Decl *D, llvm::StringRef Profile,
                     llvm::StringRef Rule);

/// True if the statement node \p S carries a [[profiles::suppress]] matching
/// \p Profile / \p Rule that covers \p UseLoc: an AttributedStmt entry covers
/// the whole statement (the caller's statement scoping bounds it), a DeclStmt
/// entry covers its owning declarator's dominion.
bool isSuppressedFor(const Stmt *S, llvm::StringRef Profile,
                     llvm::StringRef Rule, SourceLocation UseLoc,
                     const SourceManager &SM);

/// The sources a violation's suppression is looked up in: a stack of live
/// entries (anyEntryCovers), a statement walked upward through \p Parents
/// (each node via isSuppressedFor), and a declaration whose lexical chain is
/// walked (isSuppressedFor). Any member may be empty; a null \p Parents walks
/// \p UseStmt alone.
struct SuppressionQuery {
  llvm::ArrayRef<SuppressionEntry> Stack;
  const Decl *ChainAnchor = nullptr;
  const Stmt *UseStmt = nullptr;
  ParentMap *Parents = nullptr;
};

/// True if a source of \p Q suppresses \p Rule of \p Profile at \p Loc. Any
/// match suppresses, so the composition order (stack, statement walk,
/// declaration chain) is not observable.
bool isSuppressed(const SuppressionQuery &Q, llvm::StringRef Profile,
                  llvm::StringRef Rule, SourceLocation Loc,
                  const SourceManager &SM);

} // namespace profiles
} // namespace clang

#endif // LLVM_CLANG_AST_PROFILES_H
