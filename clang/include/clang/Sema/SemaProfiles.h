//===----- SemaProfiles.h ----- Semantic Analysis for C++ Profiles --------===//
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
    /// starts here (P3589R2 §2.4p3).
    SourceLocation Begin;
    /// End location of the construct, recorded only when the construct was
    /// fully parsed at push time; invalid otherwise, leaving the dominion's
    /// end bounded by the ProfileSuppressScope's lifetime (exact mid-parse;
    /// see ProfilesFrameworkInternals.rst, "Suppression Dominion Mechanics").
    SourceLocation End;
  };
  /// The live parse-time suppress entries, innermost last; pushed and popped
  /// by ProfileSuppressScope guards.
  SmallVector<ProfileSuppressEntry, 4> ProfileSuppressStack;

  /// Thin wrapper over ASTContext::isProfileEnforced (the enforcement state
  /// lives on the ASTContext).
  bool isProfileEnforced(StringRef ProfileName) const;

  /// Thin wrapper over ASTContext::isProfileEnforcedAt. Used by the
  /// per-violation emission gates; the pass-dispatch gates use the name-only
  /// query.
  bool isProfileEnforcedAt(StringRef ProfileName, SourceLocation Loc) const;

  /// True if any entry of \p Entries names an enforced profile. \p Entries is
  /// any profile opt-in table whose elements expose a \c Name member; shared
  /// by the post-parse dispatch gates (the CFG analysis pass guard and the
  /// finalization dispatcher).
  template <typename Table>
  bool anyProfileEnforced(const Table &Entries) const {
    return llvm::any_of(
        Entries, [&](const auto &E) { return isProfileEnforced(E.Name); });
  }

  const profiles::ProfileEnforcement *
  getProfileEnforcement(StringRef ProfileName) const;

  /// Record an enforcement of \p Name into the ASTContext's list, diagnosing
  /// a designator mismatch with an already-recorded enforcement of the same
  /// profile (P3589R2 [decl.attr.enforce]p3).
  bool addProfileEnforcement(StringRef Name, StringRef Designator,
                             SourceLocation Loc);
  /// Record every designator of a [[profiles::enforce]] attribute (also onto
  /// \p Mod for a module interface), appending newly recorded names and
  /// designators to \p NewNames / \p NewDesignators when given. Returns false
  /// if the attribute has no arguments.
  bool processProfilesEnforceAttr(const ParsedAttr &AL, Module *Mod,
                                  SmallVectorImpl<StringRef> *NewNames,
                                  SmallVectorImpl<StringRef> *NewDesignators);
  /// Validate each designator of a [[profiles::require]] on \p ImportDecl
  /// against the imported module's advertised set ([decl.attr.require]p2); a
  /// require on anything but a module-import-declaration is an error.
  void processProfilesRequireAttr(Decl *ImportDecl,
                                  const ParsedAttributesView &Attrs);

  /// Build a ProfilesSuppressAttr from a parsed [[profiles::suppress]];
  /// returns null if the attribute carries no profile name (parse error).
  ProfilesSuppressAttr *makeProfilesSuppressAttr(const ParsedAttr &AL);

  /// Create an implicit ProfilesSuppressAttr carrying just a profile and rule
  /// name (no justification or arguments), for propagating an active
  /// suppression onto a declaration.
  ProfilesSuppressAttr *makeImplicitProfilesSuppressAttr(StringRef ProfileName,
                                                         StringRef RuleName);

  /// True if a live parse-time suppress entry for \p ProfileName /
  /// \p RuleName covers \p Loc, i.e. \p Loc falls within the entry's
  /// dominion (see ProfilesFrameworkInternals.rst, "Suppression Dominion
  /// Mechanics").
  bool isProfileSuppressed(StringRef ProfileName, StringRef RuleName,
                           SourceLocation Loc) const;
  /// The post-parse counterpart of the parse-time stack: walk the AST upward
  /// from \p S -- enclosing statement nodes via the ParentMap, then the
  /// analyzed declaration's lexical chain -- for a matching suppression,
  /// via the shared walks in clang/AST/Profiles.h.
  bool isProfileSuppressed(StringRef ProfileName, StringRef RuleName,
                           const Stmt *S, AnalysisDeclContext &AC) const;

  /// True if \p Loc is exempt from profile enforcement under the temporary
  /// system-header stopgap (on by default; disabled by
  /// -fno-profiles-exempt-system-headers). Exposed for the analysis-based
  /// entry points, which must not run the profile-only CFG pass on a
  /// declaration the emission gates would exempt anyway.
  bool isProfileExemptSystemHeaderLoc(SourceLocation Loc) const;

  /// True if a violation of \p ProfileName / \p RuleName at \p Loc should be
  /// diagnosed: the profile is enforced at \p Loc, \p Loc is not
  /// system-header-exempt, the violation is not suppressed, and the context
  /// is neither unevaluated nor a discarded statement.
  bool shouldEmitProfileViolation(StringRef ProfileName, StringRef RuleName,
                                  SourceLocation Loc);
  /// As above, additionally honoring [[profiles::suppress]] on \p D and its
  /// lexical parents and skipping templated entities (a rule fires on the
  /// instantiation; see ProfilesFrameworkInternals.rst, "Pattern 1").
  bool shouldEmitProfileViolation(StringRef ProfileName, StringRef RuleName,
                                  SourceLocation Loc, const Decl *D);
  /// The post-parse (CFG analysis) counterpart: suppression is looked up by
  /// walking the AST upward from \p UseStmt and by consulting the still-live
  /// parse-time stack.
  bool shouldEmitProfileViolation(StringRef ProfileName, StringRef RuleName,
                                  const Stmt *UseStmt,
                                  AnalysisDeclContext &AC) const;
  /// Emit \p DiagID at \p Loc if shouldEmitProfileViolation passes; returns
  /// true if the diagnostic was emitted.
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

  /// RAII guard pushing entries onto the parse-time suppress stack for a
  /// construct's [[profiles::suppress]] attributes, popping them when the
  /// construct's parse (or instantiation) ends. A declaration's guard is
  /// pushed before its decl-specifier-seq, so the dominion covers a class or
  /// enum defined there, NSDMIs and late-parsed member bodies included
  /// (P3589R2 §2.4p3: the whole declaration's tokens). A declarator whose
  /// attributes are attached only once its Decl exists gets a second,
  /// Decl-keyed guard around its initializer or default argument; the sites
  /// are enumerated in ProfilesFrameworkInternals.rst, "Suppression Dominion
  /// Mechanics". Every parser path that parses an initializer or body for a
  /// construct that can carry [[profiles::suppress]] must install a guard;
  /// clang/test/SemaCXX/safety-profile-suppress-coverage.cpp is the
  /// per-context regression net for this contract.
  class ProfileSuppressScope {
    Sema &S;
    unsigned Count = 0;

    void push(StringRef ProfileName, StringRef RuleName, SourceLocation Begin,
              SourceLocation End);

  public:
    /// Push entries for the suppress attributes among \p Attrs, the prefix
    /// attributes of a construct about to be parsed: the dominion begins at
    /// the attribute and no end is recorded.
    ProfileSuppressScope(Sema &S, const ParsedAttributesView &Attrs);
    /// Push entries for the suppress attributes attached to \p D (and, with
    /// \p WalkLexicalParents, to its lexical parents), each bounded by its
    /// owner's construct range.
    ProfileSuppressScope(Sema &S, const Decl *D,
                         bool WalkLexicalParents = false);
    /// Push entries for the suppress attributes among \p Attrs with the
    /// explicitly supplied dominion [\p Begin, \p End].
    ProfileSuppressScope(Sema &S, ArrayRef<const Attr *> Attrs,
                         SourceLocation Begin, SourceLocation End);
    ProfileSuppressScope(const ProfileSuppressScope &) = delete;
    ProfileSuppressScope &operator=(const ProfileSuppressScope &) = delete;
    ~ProfileSuppressScope();
  };
};

} // namespace clang

#endif // LLVM_CLANG_SEMA_SEMAPROFILES_H
