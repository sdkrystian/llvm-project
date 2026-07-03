//===----- SemaProfiles.h --- C++ profiles framework ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares semantic analysis for the C++ profiles framework
/// (P3589R2).
/// See clang/docs/ProfilesFramework.rst for the design.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SEMA_SEMAPROFILES_H
#define LLVM_CLANG_SEMA_SEMAPROFILES_H

#include "clang/AST/ASTFwd.h"
#include "clang/Basic/Profiles.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Sema/SemaBase.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

class AnalysisDeclContext;
class Module;
class ParsedAttr;
class ParsedAttributesView;
class ProfilesSuppressAttr;

class SemaProfiles : public SemaBase {
public:
  SemaProfiles(Sema &S);

  struct ProfileEnforcement : profiles::EnforcedProfile {
    SourceLocation EnforceLoc;
  };
  SmallVector<ProfileEnforcement, 4> EnforcedProfiles;

  /// True if an included AST file (PCH) contributed a non-empty top-level
  /// declaration to this TU. The [[profiles::enforce]] placement check
  /// (P3589R2 [decl.attr.enforce]p1) consults this instead of deserializing
  /// the PCH's declarations; ASTWriter ORs it forward so chained PCHs
  /// propagate the bit.
  bool TUPrecededByNonEmptyDecl = false;

  struct ProfileSuppressEntry {
    StringRef ProfileName;
    StringRef RuleName;
  };
  SmallVector<ProfileSuppressEntry, 4> ProfileSuppressStack;

  /// True while a class/constructor finalization profile callback runs.
  /// Finalization can fire as a side effect of instantiating an unrelated
  /// entity whose ProfileSuppressScope is still on ProfileSuppressStack, so
  /// during finalization that transient stack is ignored and suppression is
  /// resolved only from the finalized declaration and its lexical parents
  /// (token-based dominion, P3589R2 s2.4p3).
  bool InProfileFinalizationCheck = false;

  bool isProfileEnforced(StringRef ProfileName) const;

  /// True if any entry of \p Entries names an enforced profile. \p Entries is
  /// any profile opt-in table whose elements expose a \c Name member; shared
  /// by the post-parse dispatch gates (the CFG analysis pass guard and the
  /// finalization dispatcher).
  template <typename Table>
  bool anyProfileEnforced(const Table &Entries) const {
    return llvm::any_of(
        Entries, [&](const auto &E) { return isProfileEnforced(E.Name); });
  }

  const ProfileEnforcement *getProfileEnforcement(StringRef ProfileName) const;
  bool addProfileEnforcement(StringRef Name, StringRef Designator,
                             SourceLocation Loc);
  bool processProfilesEnforceAttr(
      const ParsedAttr &AL, Module *Mod, SmallVectorImpl<StringRef> *NewNames,
      SmallVectorImpl<StringRef> *NewDesignators,
      SmallVectorImpl<unsigned> *NewArgumentCounts = nullptr,
      SmallVectorImpl<StringRef> *NewArgumentKeys = nullptr,
      SmallVectorImpl<StringRef> *NewArgumentValues = nullptr,
      SmallVectorImpl<unsigned> *NewArgumentKinds = nullptr);

  ProfilesSuppressAttr *makeProfilesSuppressAttr(const ParsedAttr &AL);

  /// Create an implicit ProfilesSuppressAttr carrying just a profile and rule
  /// name (no justification or arguments), for propagating an active
  /// suppression onto a declaration.
  ProfilesSuppressAttr *makeImplicitProfilesSuppressAttr(StringRef ProfileName,
                                                         StringRef RuleName);

  bool isProfileSuppressed(StringRef ProfileName,
                           StringRef RuleName = "") const;
  bool isProfileSuppressed(StringRef ProfileName, StringRef RuleName,
                           const Decl *D) const;
  bool isProfileSuppressed(StringRef ProfileName, StringRef RuleName,
                           const Stmt *S, AnalysisDeclContext &AC) const;
  bool shouldEmitProfileViolation(StringRef ProfileName, StringRef RuleName,
                                  SourceLocation Loc);
  bool shouldEmitProfileViolation(StringRef ProfileName, StringRef RuleName,
                                  SourceLocation Loc, const Decl *D);
  bool shouldEmitProfileViolation(StringRef ProfileName, StringRef RuleName,
                                  const Stmt *UseStmt,
                                  AnalysisDeclContext &AC) const;
  bool checkProfileViolation(StringRef ProfileName, StringRef RuleName,
                             SourceLocation Loc, unsigned DiagID);

  /// Dispatch class-finalization profile callbacks for a completed class.
  /// Called from \c Sema::CheckCompletedCXXClass so parser, template
  /// instantiation, and lambda finalization paths all reach the same hook.
  /// Dependent, invalid, and lambda classes are filtered out.
  void checkProfileViolationsAtClassFinalization(CXXRecordDecl *RD);

  /// Dispatch constructor-finalization profile callbacks once a constructor's
  /// member-initializer list is complete. Called from \c ActOnMemInitializers
  /// and \c ActOnDefaultCtorInitializers, which also serve template
  /// instantiations (via \c InstantiateMemInitializers), so every
  /// user-defined constructor is covered at the point its \c inits() is fully
  /// populated -- unlike class finalization, which runs before any
  /// constructor body is parsed. Dependent, invalid, and delegating
  /// constructors are filtered out.
  void
  checkProfileViolationsAtConstructorFinalization(CXXConstructorDecl *Ctor);

  class ProfileSuppressScope {
    Sema &S;
    unsigned Count = 0;

    void push(StringRef ProfileName, StringRef RuleName);
    void addFromDecl(const Decl *D);

  public:
    ProfileSuppressScope(Sema &S, const ParsedAttributesView &Attrs);
    ProfileSuppressScope(Sema &S, const Decl *D,
                         bool WalkLexicalParents = false);
    ProfileSuppressScope(Sema &S, ArrayRef<const Attr *> Attrs);
    ~ProfileSuppressScope();
  };
};

} // namespace clang

#endif // LLVM_CLANG_SEMA_SEMAPROFILES_H
