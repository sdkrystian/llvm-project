//===----- SemaProfiles.h ----- Semantic Analysis for C++ Profiles --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares semantic analysis for the C++ profiles framework
/// (P3589R2): enforcement and suppression recording, the violation gate, and
/// the class- and constructor-finalization dispatch.
/// See clang/docs/ProfilesFrameworkInternals.rst for the design and
/// clang/docs/ProfilesFramework.rst for the user-facing documentation.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SEMA_SEMAPROFILES_H
#define LLVM_CLANG_SEMA_SEMAPROFILES_H

#include "clang/AST/ASTFwd.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/Profiles.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Sema/SemaBase.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

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

  /// Thin wrapper over ASTContext::isProfileEnforced (the enforcement state
  /// lives on the ASTContext).
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

  /// Record an enforcement of \p Name into the ASTContext's list and map the
  /// profile's rule diagnostics to errors from \p Loc to the end of the
  /// translation unit (P3589R2 [decl.attr.enforce]p4), diagnosing a
  /// designator mismatch with an already-recorded enforcement of the same
  /// profile ([decl.attr.enforce]p3).
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

  /// True if a violation at \p Loc diagnosed by \p DiagID is to be
  /// diagnosed: the rule is enforced and not suppressed at \p Loc and \p Loc
  /// is not system-header-exempt (ASTContext::isProfileRuleActiveAt), plus
  /// the parse-time rungs -- a templated \p D never fires (the rule fires on
  /// the instantiation; see ProfilesFrameworkInternals.rst, "Pattern 1"), nor
  /// does a site in an unevaluated or discarded context. A \p PostParse
  /// site (a CFG analysis, which has no evaluation context of its own) skips
  /// the context rungs.
  bool shouldEmitProfileViolation(unsigned DiagID, SourceLocation Loc,
                                  const Decl *D = nullptr,
                                  bool PostParse = false);
  /// Emit \p DiagID -- the diagnostic of \p RuleName of \p ProfileName,
  /// which names the rule for the reader; the rule's identity is the
  /// diagnostic's group -- at \p Loc if shouldEmitProfileViolation passes;
  /// returns true if the diagnostic was emitted.
  bool checkProfileViolation(StringRef ProfileName, StringRef RuleName,
                             SourceLocation Loc, unsigned DiagID);

  /// A [[profiles::suppress]] dominion being recorded as diagnostic state:
  /// where it begins and, per diagnostic of the suppressed rules, the mapping
  /// the begin replaced, which the end restores. Empty when the attributes
  /// suppressed nothing that was not already ignored.
  struct SuppressionRecord {
    SourceLocation Begin;
    SmallVector<std::pair<diag::kind, DiagnosticMapping>, 8> Restore;
  };

  /// Record into \p Record that the dominion of the [[profiles::suppress]]
  /// attributes among \p Attrs begins at \p Begin: every diagnostic of each
  /// attribute's rule group (the profile's group when it names no rule) is
  /// mapped to ignored from the expansion site of \p Begin on. See
  /// ProfilesFrameworkInternals.rst, "Enforcement and Suppression State".
  void beginSuppression(const ParsedAttributesView &Attrs, SourceLocation Begin,
                        SuppressionRecord &Record);
  /// The same for the ProfilesSuppressAttrs attached to \p D (a template's
  /// templated declaration); an invalid \p Begin starts the dominion at the
  /// attributes themselves.
  void beginSuppression(const Decl *D, SourceLocation Begin,
                        SuppressionRecord &Record);
  /// Record that \p Record's dominion ends at \p End: the mappings its
  /// begin replaced are restored from there on, and in the states a
  /// diagnostic push inside the dominion saved. An \p End not after the
  /// begin (nothing was consumed) ends the dominion where it began.
  void endSuppression(SuppressionRecord &Record, SourceLocation End);

  /// P3589R2 [decl.attr.enforce]p5: a declaration and its redeclarations must
  /// appear in the dominions of mutually compatible profiles. Called from
  /// \c Sema::CheckRedeclarationInModule when \p New redeclares \p Old. Only
  /// a previous declaration from another module unit (a named module or a
  /// header unit) can carry a different dominion; that TU's dominion is its
  /// recorded enforcement set, \c Module::DominionProfiles. Profiles are
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
};

} // namespace clang

#endif // LLVM_CLANG_SEMA_SEMAPROFILES_H
