//===--- SemaProfiles.cpp - Semantic Analysis for C++ Profiles ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements semantic analysis for the C++ profiles framework
/// (P3589R2): profile enforcement and suppression state, the shared violation
/// gate, and the class- and constructor-finalization dispatch.
///
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaProfiles.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/Profiles.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Attr.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"

using namespace clang;

SemaProfiles::SemaProfiles(Sema &S) : SemaBase(S) {}

bool SemaProfiles::isProfileEnforced(StringRef ProfileName) const {
  return getASTContext().isProfileEnforced(ProfileName);
}

/// Map every diagnostic of the group \p Group to \p Severity from \p Loc to
/// the end of the translation unit; an unknown group maps nothing.
static void mapProfileGroupFrom(DiagnosticsEngine &Diags, StringRef Group,
                                diag::Severity Severity, SourceLocation Loc) {
  SmallVector<diag::kind, 16> Kinds;
  if (Diags.getDiagnosticIDs()->getDiagnosticsInGroup(
          diag::Flavor::WarningOrError, Group, Kinds))
    return;
  SmallVector<std::pair<diag::kind, DiagnosticMapping>, 16> Mappings;
  for (diag::kind Kind : Kinds)
    Mappings.push_back({Kind, DiagnosticMapping::Make(Severity, /*IsUser=*/true,
                                                      /*IsPragma=*/true)});
  Diags.setDiagnosticMappingsFrom(Mappings, Loc);
}

bool SemaProfiles::addProfileEnforcement(StringRef Name, StringRef Designator,
                                         SourceLocation Loc) {
  if (const auto *Existing = getASTContext().getProfileEnforcement(Name)) {
    if (Existing->Designator != Designator) {
      Diag(Loc, diag::err_profiles_enforce_mismatch) << Name;
      if (llvm::is_contained(getLangOpts().ProfilesEnforce, Name))
        Diag(Loc, diag::note_profiles_enforced_on_command_line) << Name;
      else
        Diag(Existing->EnforceLoc, diag::note_previous_attribute);
      return false;
    }
    return true;
  }
  getASTContext().addEnforcedProfile(Name, Designator, Loc);
  // The enforcement's dominion is the rule diagnostics' mapping from the
  // attribute on (the expansion site of a macro-spelled attribute); an inert
  // profile maps nothing. See ProfilesFrameworkInternals.rst, "Enforcement
  // State".
  if (Loc.isValid() &&
      !profiles::isProfileNameInert(Name, getLangOpts().ProfilesTestProfiles))
    mapProfileGroupFrom(
        getDiagnostics(), profiles::getProfileDiagGroupName(Name, /*Rule=*/""),
        diag::Severity::Error,
        getASTContext().getSourceManager().getExpansionLoc(Loc));
  return true;
}

/// Unzip profile arguments into the parallel key/value/kind arrays that
/// ProfilesSuppressAttr stores (Attr.td cannot hold structured arguments).
static void unzipProfileArguments(ArrayRef<profiles::ProfileArgument> Arguments,
                                  SmallVectorImpl<StringRef> &Keys,
                                  SmallVectorImpl<StringRef> &Values,
                                  SmallVectorImpl<unsigned> &Kinds) {
  for (const auto &Arg : Arguments) {
    Keys.push_back(Arg.Key);
    Values.push_back(Arg.Value);
    Kinds.push_back(static_cast<unsigned>(Arg.Kind));
  }
}

bool SemaProfiles::processProfilesEnforceAttr(
    const ParsedAttr &AL, Module *Mod, SmallVectorImpl<StringRef> *NewNames,
    SmallVectorImpl<StringRef> *NewDesignators) {
  ArrayRef<detail::ProfileDesignator> Designators = AL.getProfileDesignators();
  if (Designators.empty()) {
    Diag(AL.getLoc(), diag::err_attribute_too_few_arguments) << AL << 1;
    return false;
  }

  for (const auto &D : Designators) {
    StringRef Name = D.Name;
    StringRef Spelling = D.Spelling;

    // "Already recorded?" must use the ungated lookup: isProfileEnforced
    // filters gated-off test:: names (and -fprofiles off), which would make
    // every repetition of such a profile look new and re-append its
    // designator to the attribute's argument arrays.
    bool IsNew = !getASTContext().getProfileEnforcement(Name);
    if (!addProfileEnforcement(Name, Spelling, AL.getLoc()))
      continue;

    if (Mod && !llvm::any_of(Mod->AdvertisedProfiles,
                             [&](const profiles::EnforcedProfile &EP) {
                               return EP.ProfileName == Name;
                             }))
      Mod->AdvertisedProfiles.push_back({Name.str(), Spelling.str()});

    if (IsNew) {
      if (NewNames)
        NewNames->push_back(Name);
      if (NewDesignators)
        NewDesignators->push_back(Spelling);
    }
  }
  return true;
}

void SemaProfiles::processProfilesRequireAttr(
    Decl *ImportDecl, const ParsedAttributesView &Attrs) {
  if (!ImportDecl)
    return;

  auto *ID = dyn_cast<clang::ImportDecl>(ImportDecl);
  Module *ImportedMod = ID ? ID->getImportedModule() : nullptr;

  for (const auto &AL : Attrs) {
    if (AL.getKind() != ParsedAttr::AT_ProfilesRequire)
      continue;
    if (!AL.diagnoseLangOpts(SemaRef))
      continue;

    if (!ImportedMod) {
      Diag(AL.getLoc(), diag::err_profiles_require_not_on_import);
      continue;
    }

    ArrayRef<detail::ProfileDesignator> Designators =
        AL.getProfileDesignators();
    if (Designators.empty()) {
      Diag(AL.getLoc(), diag::err_attribute_too_few_arguments) << AL << 1;
      continue;
    }

    for (const auto &Desig : Designators) {
      StringRef Spelling = Desig.Spelling;
      bool Found = llvm::any_of(ImportedMod->AdvertisedProfiles,
                                [&](const profiles::EnforcedProfile &EP) {
                                  return EP.Designator == Spelling;
                                });
      if (!Found)
        Diag(AL.getLoc(), diag::err_profiles_require_not_enforced) << Spelling;
    }
  }
}

void SemaProfiles::checkRedeclarationProfileCompatibility(
    const NamedDecl *New, const NamedDecl *Old) {
  if (!getLangOpts().Profiles)
    return;
  // The skipped cases below are enumerated and argued in
  // ProfilesFrameworkInternals.rst, "Redeclaration Compatibility".
  // A textual or PCH previous declaration shares this TU's dominion.
  if (!Old->isFromASTFile())
    return;
  Module *M = Old->getOwningModule();
  if (!M)
    return;
  // A module-map module (-fmodules): textual inclusion, no dominion of its
  // own.
  if (M->isModuleMapModule())
    return;
  // The system-header exemption stopgap covers the previous declaration's
  // dominion too.
  if (getASTContext().isProfileExemptSystemHeaderLoc(Old->getLocation()))
    return;
  // An explicit global-module-fragment declaration's dominion is unknown;
  // skip rather than guess.
  if (M->isExplicitGlobalModule())
    return;
  Module *Top = M->getTopLevelModule();
  // A previous declaration from the same module family is skipped: this unit
  // inherits the interface's enforcements, and an implementation unit's
  // additional TU-local enforcements do not obligate the interface.
  if (Module *Current = SemaRef.getCurrentModule())
    if (Current->getTopLevelModule()->getPrimaryModuleInterfaceName() ==
        Top->getPrimaryModuleInterfaceName())
      return;

  // Both dominions as profile names: the previous unit's recorded enforcement
  // set and this TU's.
  SmallVector<StringRef, 4> There(Top->DominionProfiles.begin(),
                                  Top->DominionProfiles.end());
  SmallVector<StringRef, 4> Here;
  for (const auto &EP : getASTContext().enforced_profiles())
    Here.push_back(EP.ProfileName);

  // A gated-off test:: profile is inert in this compilation, on either side.
  auto IsActive = [&](StringRef Name) {
    return !profiles::isProfileNameInert(Name,
                                         getLangOpts().ProfilesTestProfiles);
  };
  // First active profile in Enforced with no compatible counterpart in
  // Covering; empty if fully covered.
  auto FindUncovered = [&](ArrayRef<StringRef> Enforced,
                           ArrayRef<StringRef> Covering) -> StringRef {
    for (StringRef Name : Enforced) {
      if (!IsActive(Name))
        continue;
      if (llvm::none_of(Covering, [&](StringRef Other) {
            return profiles::areProfilesCompatible(Name, Other);
          }))
        return Name;
    }
    return {};
  };

  // The rule is symmetric: every profile whose dominion covers one
  // declaration must have a compatible counterpart covering the other.
  // Report the first violation in each direction.
  StringRef MissingHere = FindUncovered(There, Here);
  StringRef MissingThere = FindUncovered(Here, There);
  if (MissingHere.empty() && MissingThere.empty())
    return;
  if (!MissingHere.empty())
    Diag(New->getLocation(), diag::err_profiles_redecl_incompatible)
        << /*PreviouslyEnforced=*/0 << New << MissingHere << Top->Name;
  if (!MissingThere.empty())
    Diag(New->getLocation(), diag::err_profiles_redecl_incompatible)
        << /*PreviouslyEnforced=*/1 << New << MissingThere << Top->Name;
  Diag(Old->getLocation(), diag::note_previous_declaration);
}

ProfilesSuppressAttr *
SemaProfiles::makeProfilesSuppressAttr(const ParsedAttr &AL) {
  const auto &Args = AL.getProfileSuppressArgs();
  if (Args.Name.empty())
    return nullptr;

  SmallVector<StringRef, 4> RawArgumentKeys;
  SmallVector<StringRef, 4> RawArgumentValues;
  SmallVector<unsigned, 4> RawArgumentKinds;
  unzipProfileArguments(Args.Arguments, RawArgumentKeys, RawArgumentValues,
                        RawArgumentKinds);

  return ::new (getASTContext()) ProfilesSuppressAttr(
      getASTContext(), AL, Args.Name, Args.Justification, Args.Rule,
      RawArgumentKeys.data(), RawArgumentKeys.size(), RawArgumentValues.data(),
      RawArgumentValues.size(), RawArgumentKinds.data(),
      RawArgumentKinds.size());
}

ProfilesSuppressAttr *
SemaProfiles::makeImplicitProfilesSuppressAttr(StringRef ProfileName,
                                               StringRef RuleName) {
  return ProfilesSuppressAttr::CreateImplicit(
      getASTContext(), ProfileName, /*Justification=*/"", RuleName,
      /*RawArgumentKeys=*/nullptr, /*RawArgumentKeysSize=*/0,
      /*RawArgumentValues=*/nullptr, /*RawArgumentValuesSize=*/0,
      /*RawArgumentKinds=*/nullptr, /*RawArgumentKindsSize=*/0);
}

/// Map every diagnostic of the rule groups named by \p Suppressions (profile,
/// rule) to ignored from \p Begin on, recording the replaced mappings into
/// \p Record; a diagnostic already ignored at \p Begin needs nothing.
static void beginSuppressionOf(
    Sema &S, ArrayRef<std::pair<StringRef, StringRef>> Suppressions,
    SourceLocation Begin, SemaProfiles::SuppressionRecord &Record) {
  if (Suppressions.empty() || Begin.isInvalid())
    return;
  DiagnosticsEngine &Diags = S.getDiagnostics();
  Begin = S.getSourceManager().getExpansionLoc(Begin);
  SmallVector<std::pair<diag::kind, DiagnosticMapping>, 8> Ignore;
  for (const auto &[Profile, Rule] : Suppressions) {
    SmallVector<diag::kind, 16> Kinds;
    if (Diags.getDiagnosticIDs()->getDiagnosticsInGroup(
            diag::Flavor::WarningOrError,
            profiles::getProfileDiagGroupName(Profile, Rule), Kinds))
      continue;
    for (diag::kind Kind : Kinds) {
      DiagnosticMapping Current = Diags.getDiagnosticMappingAt(Kind, Begin);
      if (Current.getSeverity() == diag::Severity::Ignored ||
          llvm::any_of(Ignore, [&](const auto &M) { return M.first == Kind; }))
        continue;
      Record.Restore.push_back({Kind, Current});
      Ignore.push_back({Kind, DiagnosticMapping::Make(diag::Severity::Ignored,
                                                      /*IsUser=*/true,
                                                      /*IsPragma=*/true)});
    }
  }
  if (Ignore.empty())
    return;
  Record.Begin = Begin;
  Diags.setDiagnosticMappingsAt(Ignore, Begin);
}

void SemaProfiles::beginSuppression(const ParsedAttributesView &Attrs,
                                    SourceLocation Begin,
                                    SuppressionRecord &Record) {
  if (!getLangOpts().Profiles)
    return;
  SmallVector<std::pair<StringRef, StringRef>, 2> Suppressions;
  for (const ParsedAttr &AL : Attrs) {
    if (AL.getKind() != ParsedAttr::AT_ProfilesSuppress)
      continue;
    const auto &Args = AL.getProfileSuppressArgs();
    if (!Args.Name.empty())
      Suppressions.push_back({Args.Name, Args.Rule});
  }
  beginSuppressionOf(SemaRef, Suppressions, Begin, Record);
}

void SemaProfiles::beginSuppression(const Decl *D, SourceLocation Begin,
                                    SuppressionRecord &Record) {
  if (!getLangOpts().Profiles || !D)
    return;
  SmallVector<std::pair<StringRef, StringRef>, 2> Suppressions;
  for (const auto *A : D->specific_attrs<ProfilesSuppressAttr>()) {
    Suppressions.push_back({A->getProfileName(), A->getRule()});
    if (Begin.isInvalid())
      Begin = A->getLocation();
  }
  beginSuppressionOf(SemaRef, Suppressions, Begin, Record);
}

void SemaProfiles::endSuppression(SuppressionRecord &Record,
                                  SourceLocation End) {
  if (Record.Restore.empty())
    return;
  const SourceManager &SM = getASTContext().getSourceManager();
  if (End.isInvalid() || !SM.isBeforeInTranslationUnit(Record.Begin, End))
    End = Record.Begin;
  getDiagnostics().setDiagnosticMappingsAt(Record.Restore, End,
                                           SourceRange(Record.Begin, End));
  Record.Restore.clear();
}

bool SemaProfiles::shouldEmitProfileViolation(unsigned DiagID,
                                              StringRef ProfileName,
                                              StringRef RuleName,
                                              SourceLocation Loc, const Decl *D,
                                              const Stmt *UseStmt,
                                              AnalysisDeclContext *AC) {
  // The suppression sources: the live parse-time stack (a function can be
  // analyzed while an enclosing construct is still mid-parse -- a local
  // class's method body -- and the consult is dominion-checked, so an
  // unrelated live scope never matches), the checked declaration's lexical
  // chain (it survives the parse scope's teardown, so finalization checks
  // still respect suppression), and for a post-parse site the use statement's
  // enclosing statements and the analyzed declaration's chain. See
  // ProfilesFrameworkInternals.rst, "Suppression Dominion Mechanics".
  profiles::SuppressionQuery Q{ProfileSuppressStack,
                               D ? D : (AC ? AC->getDecl() : nullptr), UseStmt,
                               AC ? &AC->getParentMap() : nullptr};
  if (!profiles::shouldEmitProfileViolation(getASTContext(), DiagID,
                                            ProfileName, RuleName, Loc, Q))
    return false;
  // A templated entity is not a phase-7 entity (P3589R2 §1.1), so a profile
  // rule fires only on the instantiation, never on the pattern (checking the
  // pattern would diagnose never-instantiated templates and double-fire). A
  // Decl-less expression check site must instead defer in a dependent context
  // from its own wrapper; see ProfilesFrameworkInternals.rst, "Pattern 1".
  if (D && D->isTemplated())
    return false;
  // The evaluation-context rungs are parse-time facts; a post-parse site has
  // no evaluation context of its own.
  if (AC)
    return true;
  if (SemaRef.isUnevaluatedContext())
    return false;
  if (SemaRef.currentEvaluationContext().isDiscardedStatementContext())
    return false;
  return true;
}

bool SemaProfiles::checkProfileViolation(StringRef ProfileName,
                                         StringRef RuleName, SourceLocation Loc,
                                         unsigned DiagID) {
  if (!shouldEmitProfileViolation(DiagID, ProfileName, RuleName, Loc))
    return false;
  Diag(Loc, DiagID) << ProfileName;
  return true;
}

void SemaProfiles::ProfileSuppressScope::push(StringRef ProfileName,
                                              StringRef RuleName,
                                              SourceLocation Begin,
                                              SourceLocation End) {
  S.Profiles().ProfileSuppressStack.push_back(
      {ProfileName, RuleName, SourceRange(Begin, End)});
  ++Count;
}

SemaProfiles::ProfileSuppressScope::ProfileSuppressScope(
    Sema &S, const ParsedAttributesView &Attrs)
    : S(S) {
  if (!S.getLangOpts().Profiles)
    return;
  for (const auto &AL : Attrs) {
    if (AL.getKind() != ParsedAttr::AT_ProfilesSuppress)
      continue;
    const auto &Args = AL.getProfileSuppressArgs();
    // These are the prefix attributes of a statement or declaration about to
    // be parsed, so the attribute's own location is the construct's begin and
    // no end is known yet (the scope's lifetime bounds it).
    if (!Args.Name.empty())
      push(Args.Name, Args.Rule, AL.getLoc(), SourceLocation());
  }
}

/// The end location of \p D's construct if it is fully parsed, invalid
/// otherwise. A partially parsed construct's end location is usually *valid
/// but early*, so each arm gates on the marker that the construct's real end
/// has been seen. An unhandled declaration kind returns invalid, which falls
/// back to scope-lifetime bounding -- a longer dominion, more suppression,
/// never a false positive.
static SourceLocation getCompletedConstructEnd(const Decl *D) {
  // The brace range is set only by ActOnTagFinishDefinition, so a mid-parse
  // tag yields an invalid end with no explicit gate.
  if (const auto *TD = dyn_cast<TagDecl>(D))
    return TD->getBraceRange().getEnd();
  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    // isLateTemplateParsed makes doesThisDeclarationHaveABody true while the
    // body is merely token-cached and the range still ends at the declarator.
    if (FD->doesThisDeclarationHaveABody() && !FD->isLateTemplateParsed())
      return FD->getSourceRange().getEnd();
    return SourceLocation();
  }
  if (const auto *VD = dyn_cast<VarDecl>(D)) {
    // A declarator with no initializer attached yet ends -- valid but early
    // -- at the declarator itself; gate on the initializer.
    if (VD->hasInit())
      return VD->getSourceRange().getEnd();
    return SourceLocation();
  }
  if (const auto *FD = dyn_cast<FieldDecl>(D)) {
    // The in-class initializer expression is null while its late parse is
    // still pending.
    if (FD->hasNonNullInClassInitializer())
      return FD->getInClassInitializer()->getEndLoc();
    return SourceLocation();
  }
  // The r-brace is recorded only when the namespace body finishes.
  if (const auto *ND = dyn_cast<NamespaceDecl>(D))
    return ND->getRBraceLoc();
  // Unhandled kinds fall back to scope-lifetime bounding (see above).
  return SourceLocation();
}

SemaProfiles::ProfileSuppressScope::ProfileSuppressScope(
    Sema &S, const Decl *D, bool WalkLexicalParents)
    : S(S) {
  if (!S.getLangOpts().Profiles || !D)
    return;
  // Each entry's dominion is its own owner's construct range, so the range is
  // computed per owning declaration as the shared walk surfaces it.
  profiles::forEachSuppression(
      D, WalkLexicalParents,
      [&](const Decl &Owner, const ProfilesSuppressAttr &A) {
        SourceLocation Begin = Owner.getBeginLoc();
        if (Begin.isInvalid())
          Begin = Owner.getLocation();
        push(A.getProfileName(), A.getRule(), Begin,
             getCompletedConstructEnd(&Owner));
        return false;
      });
}

SemaProfiles::ProfileSuppressScope::ProfileSuppressScope(
    Sema &S, ArrayRef<const Attr *> Attrs, SourceLocation Begin,
    SourceLocation End)
    : S(S) {
  if (!S.getLangOpts().Profiles)
    return;
  for (const auto *A : Attrs)
    if (const auto *PSA = dyn_cast<ProfilesSuppressAttr>(A))
      push(PSA->getProfileName(), PSA->getRule(), Begin, End);
}

SemaProfiles::ProfileSuppressScope::~ProfileSuppressScope() {
  assert(S.Profiles().ProfileSuppressStack.size() >= Count);
  S.Profiles().ProfileSuppressStack.pop_back_n(Count);
}

namespace {
/// Row for the unified finalization dispatch shared by class-finalization
/// (pattern 3) and constructor-finalization (pattern 4): a profile name plus
/// a callback invoked once per finalized, non-dependent, non-invalid Node (a
/// CXXRecordDecl or a CXXConstructorDecl). Adding a new profile is a single
/// row in the matching table below plus a ProfileRule diagnostic in
/// DiagnosticSemaKinds.td and a callback that consults
/// SemaProfiles::shouldEmitProfileViolation before emitting.
template <class Node> struct FinalizationProfile {
  StringRef Name;
  void (*Callback)(Sema &, Node *);
};

void runTestClassFinalCallback(Sema &S, CXXRecordDecl *RD) {
  if (!S.Profiles().shouldEmitProfileViolation(
          diag::err_profile_class_final_test, "test::class_final",
          /*Rule=*/"", RD->getLocation(), RD))
    return;
  S.Diag(RD->getLocation(), diag::err_profile_class_final_test)
      << "test::class_final" << RD;
}

void runTestCtorFinalCallback(Sema &S, CXXConstructorDecl *Ctor) {
  if (!S.Profiles().shouldEmitProfileViolation(
          diag::err_profile_ctor_final_test, "test::ctor_final", /*Rule=*/"",
          Ctor->getLocation(), Ctor))
    return;
  S.Diag(Ctor->getLocation(), diag::err_profile_ctor_final_test)
      << "test::ctor_final" << Ctor->getParent();
}

constexpr FinalizationProfile<CXXRecordDecl> ClassFinalizationProfiles[] = {
    {"test::class_final", &runTestClassFinalCallback},
};

constexpr FinalizationProfile<CXXConstructorDecl>
    ConstructorFinalizationProfiles[] = {
        {"test::ctor_final", &runTestCtorFinalCallback},
};

/// Run the enforced finalization-profile callbacks in \p Table for \p D; the
/// per-node filter (dependent, lambda, delegating, ...) stays at each call
/// site. Each callback consults shouldEmitProfileViolation with \p D, which
/// honors [[profiles::suppress]] on \p D or a lexical parent, so the
/// dispatcher needs no suppress scope of its own.
template <class Node, std::size_t N>
void dispatchFinalizationProfiles(Sema &S, Node *D,
                                  const FinalizationProfile<Node> (&Table)[N]) {
  if (!S.Profiles().anyProfileEnforced(Table))
    return;
  for (const auto &E : Table)
    if (S.Profiles().isProfileEnforced(E.Name))
      E.Callback(S, D);
}
} // namespace

void SemaProfiles::checkProfileViolationsAtClassFinalization(
    CXXRecordDecl *RD) {
  if (!getLangOpts().Profiles || !RD)
    return;
  if (RD->isInvalidDecl() || RD->isDependentType() || RD->isLambda())
    return;
  dispatchFinalizationProfiles(SemaRef, RD, ClassFinalizationProfiles);
}

void SemaProfiles::checkProfileViolationsAtConstructorFinalization(
    CXXConstructorDecl *Ctor) {
  if (!getLangOpts().Profiles || !Ctor)
    return;
  // A dependent constructor pattern re-fires on instantiation; a delegating
  // constructor leaves member initialization to its target.
  if (Ctor->isInvalidDecl() || Ctor->isDependentContext() ||
      Ctor->isDelegatingConstructor())
    return;
  dispatchFinalizationProfiles(SemaRef, Ctor, ConstructorFinalizationProfiles);
}
