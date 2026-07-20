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
/// See clang/docs/ProfilesFrameworkInternals.rst for the design and
/// clang/docs/ProfilesFramework.rst for the user-facing documentation.
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
    /// Begin location of the construct the suppression appertains to (the
    /// declaration or statement, not the attribute). The entry's dominion
    /// starts here (P3589R2 s2.4p3).
    SourceLocation Begin;
    /// End location of the construct, recorded only when the construct was
    /// fully parsed at push time; invalid otherwise, leaving the dominion's
    /// end bounded by the ProfileSuppressScope's lifetime. That fallback is
    /// exact for a construct still being parsed -- its later tokens do not
    /// exist yet, and instantiation of a not-yet-defined template is
    /// deferred past the scope's death -- while a completed construct's
    /// recorded end keeps a live scope from covering a pattern first
    /// declared after it.
    SourceLocation End;
  };
  SmallVector<ProfileSuppressEntry, 4> ProfileSuppressStack;

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

  /// True if a live parse-time suppress entry for \p ProfileName /
  /// \p RuleName covers \p Loc. An entry matches only tokens within its
  /// construct's recorded range (its dominion, P3589R2 s2.4p3); when the
  /// construct was still being parsed at push time no end is recorded and
  /// the owning ProfileSuppressScope's lifetime bounds the dominion's end.
  /// Tokens from outside the construct -- e.g. a template pattern
  /// instantiated synchronously while the scope is live, wherever it is
  /// declared -- are not suppressed.
  bool isProfileSuppressed(StringRef ProfileName, StringRef RuleName,
                           SourceLocation Loc) const;
  /// The post-parse counterpart of the parse-time stack: walk the AST upward
  /// from \p S -- enclosing statement nodes via the ParentMap, then the
  /// analyzed declaration's lexical chain -- for a matching suppression,
  /// via the shared walks in clang/AST/Profiles.h.
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

  /// P3589R2 [decl.attr.enforce]p5: a declaration and its redeclarations must
  /// appear in the dominions of mutually compatible profiles. Called from
  /// \c Sema::CheckRedeclarationInModule when \p New redeclares \p Old. Only
  /// a previous declaration from another module unit (a named module or a
  /// header unit) can carry a different dominion; that TU's dominion is
  /// approximated by the module's exported designator set. Profiles are
  /// compatible by name, with all std:: profiles mutually compatible.
  /// Diagnose-only: the redeclaration is not invalidated.
  void checkRedeclarationProfileCompatibility(const NamedDecl *New,
                                              const NamedDecl *Old);

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

    void push(StringRef ProfileName, StringRef RuleName, SourceLocation Begin,
              SourceLocation End);

  public:
    ProfileSuppressScope(Sema &S, const ParsedAttributesView &Attrs);
    ProfileSuppressScope(Sema &S, const Decl *D,
                         bool WalkLexicalParents = false);
    ProfileSuppressScope(Sema &S, ArrayRef<const Attr *> Attrs,
                         SourceLocation Begin, SourceLocation End);
    ~ProfileSuppressScope();
  };
};

} // namespace clang

#endif // LLVM_CLANG_SEMA_SEMAPROFILES_H
