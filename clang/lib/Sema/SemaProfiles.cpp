//===--- SemaProfiles.cpp - Semantic Analysis for C++ Profiles ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements semantic analysis for the C++ profiles framework
/// (P3589R2) and the built-in std::init initialization profile (P4222R2):
/// profile enforcement and suppression state, the shared violation gate, the
/// class-completion and constructor-finalization check wrappers, the
/// parse-time std::init rule checks, and the recognizers and tracked-storage
/// predicates shared with the CFG-based std::init checks in
/// AnalysisBasedWarnings.cpp, which judge every access to flow-tracked
/// storage.
///
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaProfiles.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/IgnoreExpr.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SimpleTypoCorrection.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang;

SemaProfiles::SemaProfiles(Sema &S) : SemaBase(S) {}

bool SemaProfiles::isProfileEnforced(StringRef ProfileName) const {
  return getASTContext().isProfileEnforced(ProfileName);
}

bool SemaProfiles::isProfileLive(StringRef Profile) {
  ASTContext &Ctx = getASTContext();
  if (Ctx.isProfileEnforcedByAnyUnit(Profile))
    return true;
  if (!getLangOpts().Profiles ||
      profiles::isProfileNameInert(Profile, getLangOpts().ProfilesTestProfiles))
    return false;
  // The option half is a constant of the compilation, computed once per
  // profile.
  auto It = RulesEnabledByOption.find(Profile);
  if (It != RulesEnabledByOption.end())
    return It->second;
  std::optional<StringRef> Group =
      profiles::getProfileDiagGroupName(Profile, /*Rule=*/"");
  SmallVector<diag::kind, 16> Kinds;
  bool Enabled = Group &&
                 !getDiagnostics().getDiagnosticIDs()->getDiagnosticsInGroup(
                     diag::Flavor::WarningOrError, *Group, Kinds) &&
                 llvm::any_of(Kinds, [&](diag::kind K) {
                   return Ctx.isProfileRuleEnabledByOption(K);
                 });
  RulesEnabledByOption.try_emplace(Profile, Enabled);
  return Enabled;
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
  // profile maps nothing. See ProfilesFrameworkInternals.rst,
  // "Enforcement and Suppression State".
  if (Loc.isInvalid() ||
      profiles::isProfileNameInert(Name, getLangOpts().ProfilesTestProfiles))
    return true;
  if (std::optional<StringRef> Group =
          profiles::getProfileDiagGroupName(Name, /*Rule=*/""))
    mapProfileGroupFrom(
        getDiagnostics(), *Group, diag::Severity::Error,
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

/// True if \p Profile names a profile of the registry that is not inert in
/// this compilation.
static bool isKnownProfile(Sema &S, StringRef Profile) {
  return !profiles::isProfileNameInert(Profile,
                                       S.getLangOpts().ProfilesTestProfiles) &&
         profiles::getProfileDiagGroupName(Profile, /*Rule=*/"");
}

/// Warn at \p Loc about a profile name that names no known profile but is a
/// near miss of one, and about a rule name that names no rule of a known
/// profile: neither enforces nor suppresses anything.
static void diagnoseUnknownProfileNames(Sema &S, SourceLocation Loc,
                                        StringRef Profile, StringRef Rule) {
  if (!isKnownProfile(S, Profile)) {
    SimpleTypoCorrection Correction(Profile);
    for (const profiles::ProfileRegistryName &N :
         profiles::getProfileRegistryNames())
      if (N.Rule.empty() && isKnownProfile(S, N.Profile))
        Correction.add(N.Profile);
    if (Correction.hasCorrection())
      S.Diag(Loc, diag::warn_profiles_unknown_profile)
          << Profile << *Correction.getCorrection();
    return;
  }
  if (Rule.empty() || profiles::getProfileDiagGroupName(Profile, Rule))
    return;
  SimpleTypoCorrection Correction(Rule);
  for (const profiles::ProfileRegistryName &N :
       profiles::getProfileRegistryNames())
    if (N.Profile == Profile && !N.Rule.empty())
      Correction.add(N.Rule);
  S.Diag(Loc, diag::warn_profiles_unknown_rule)
      << Profile << Rule << Correction.hasCorrection()
      << Correction.getCorrection().value_or("");
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
    if (IsNew)
      diagnoseUnknownProfileNames(SemaRef, AL.getLoc(), Name, /*Rule=*/"");
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
  // Prefix attributes reach each declarator of a declaration; diagnose them
  // once.
  if (!AL.hasProcessingCache()) {
    AL.setProcessingCache(0);
    diagnoseUnknownProfileNames(SemaRef, AL.getLoc(), Args.Name, Args.Rule);
  }

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

/// The position up to which a suppression's begin or end reaches the states
/// recorded ahead of it: the expansion site of the last token of the
/// token-cached region \p Frontier names, or invalid (no bound) outside a
/// late parse.
static SourceLocation suppressionFrontier(const SourceManager &SM,
                                          SourceLocation Frontier) {
  return Frontier.isValid() ? SM.getExpansionRange(Frontier).getEnd()
                            : Frontier;
}

/// Map every diagnostic of the rule groups named by \p Suppressions (profile,
/// rule) to ignored from \p Begin on, recording the changes into \p Record;
/// a diagnostic already ignored at \p Begin needs nothing.
static void
beginSuppressionOf(Sema &S,
                   ArrayRef<std::pair<StringRef, StringRef>> Suppressions,
                   SourceLocation Begin, SourceLocation Frontier,
                   SemaProfiles::SuppressionRecord &Record) {
  if (Suppressions.empty() || Begin.isInvalid())
    return;
  DiagnosticsEngine &Diags = S.getDiagnostics();
  Begin = S.getSourceManager().getExpansionLoc(Begin);
  for (const auto &[Profile, Rule] : Suppressions) {
    std::optional<StringRef> Group =
        profiles::getProfileDiagGroupName(Profile, Rule);
    SmallVector<diag::kind, 16> Kinds;
    if (!Group || Diags.getDiagnosticIDs()->getDiagnosticsInGroup(
                      diag::Flavor::WarningOrError, *Group, Kinds))
      continue;
    for (diag::kind Kind : Kinds) {
      DiagnosticMapping Current = Diags.getDiagnosticMappingAt(Kind, Begin);
      if (Current.getSeverity() == diag::Severity::Ignored ||
          llvm::any_of(Record.Changes,
                       [&](const auto &C) { return C.Diag == Kind; }))
        continue;
      // Unlike a pragma's mapping, this one has neither the no-Werror nor
      // the no-fatal flag, so the end tells a pragma's remapping from it.
      Record.Changes.push_back(
          {Kind, Current,
           DiagnosticMapping::Make(diag::Severity::Ignored, /*IsUser=*/true,
                                   /*IsPragma=*/true)});
    }
  }
  if (Record.Changes.empty())
    return;
  Record.Begin = Begin;
  Diags.replaceDiagnosticMappingsAt(
      Record.Changes, Begin,
      suppressionFrontier(S.getSourceManager(), Frontier));
}

void SemaProfiles::beginSuppression(const ParsedAttributesView &Attrs,
                                    SourceLocation Begin,
                                    SourceLocation Frontier,
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
  beginSuppressionOf(SemaRef, Suppressions, Begin, Frontier, Record);
}

void SemaProfiles::beginSuppression(const Decl *D, SourceLocation Begin,
                                    SourceLocation Frontier,
                                    SuppressionRecord &Record) {
  if (!getLangOpts().Profiles || !D)
    return;
  // A template's attributes are attached to its templated declaration.
  if (const auto *TD = dyn_cast<TemplateDecl>(D))
    D = TD->getTemplatedDecl();
  SmallVector<std::pair<StringRef, StringRef>, 2> Suppressions;
  for (const auto *A : D->specific_attrs<ProfilesSuppressAttr>()) {
    Suppressions.push_back({A->getProfileName(), A->getRule()});
    if (Begin.isInvalid())
      Begin = A->getLocation();
  }
  beginSuppressionOf(SemaRef, Suppressions, Begin, Frontier, Record);
}

void SemaProfiles::endSuppression(SuppressionRecord &Record, SourceLocation End,
                                  SourceLocation Frontier) {
  if (Record.Changes.empty())
    return;
  const SourceManager &SM = getASTContext().getSourceManager();
  if (End.isInvalid() || !SM.isBeforeInTranslationUnit(Record.Begin, End))
    End = Record.Begin;
  for (DiagnosticsEngine::DiagnosticMappingChange &C : Record.Changes)
    std::swap(C.From, C.To);
  getDiagnostics().replaceDiagnosticMappingsAt(
      Record.Changes, End, suppressionFrontier(SM, Frontier));
  Record.Changes.clear();
}

bool SemaProfiles::shouldEmitProfileViolation(StringRef Profile,
                                              unsigned DiagID,
                                              SourceLocation Loc, const Decl *D,
                                              bool PostParse) {
  if (!isProfileLive(Profile))
    return false;
  if (!getASTContext().isProfileRuleActiveAt(DiagID, Loc))
    return false;
  // A templated entity is not a phase-7 entity (P3589R2 §1.1), so a profile
  // rule fires only on the instantiation, never on the pattern (checking the
  // pattern would diagnose never-instantiated templates and double-fire);
  // std::init's marker and binding checks pass the Decl and defer here. A
  // Decl-less expression check site must instead defer in a dependent context
  // from its own wrapper; see ProfilesFrameworkInternals.rst, "Check Sites".
  if (D && D->isTemplated())
    return false;
  // The evaluation-context rungs are parse-time facts; a post-parse site has
  // no evaluation context of its own.
  if (PostParse)
    return true;
  if (SemaRef.isUnevaluatedContext())
    return false;
  if (SemaRef.currentEvaluationContext().isDiscardedStatementContext())
    return false;
  return true;
}

// True if a field of RD -- or, transitively, of an anonymous-record member
// of RD -- carries a default member initializer. Default-initializing a
// union performs the initialization of its single variant member with a
// default member initializer ([class.union.general]p6; at most one variant
// may have one), and initializers written on the leaves of an
// anonymous-record variant activate it the same way.
static bool anyLeafHasNSDMI(const CXXRecordDecl *RD) {
  if (!RD->hasDefinition() || RD->isInvalidDecl())
    return false;
  for (const FieldDecl *F : RD->fields()) {
    if (F->isUnnamedBitField())
      continue;
    if (F->hasInClassInitializer())
      return true;
    if (F->isAnonymousStructOrUnion())
      if (const auto *FRD = F->getType()->getAsCXXRecordDecl())
        if (anyLeafHasNSDMI(FRD))
          return true;
  }
  return false;
}

// UntrustRoot bypasses the user-provided default-constructor trust for the
// *root* record only (recursion always re-trusts): defaultInitNonVacuityReason
// uses it to ask the factual question about a class whose out-of-line defaulted
// default constructor is user-provided yet initializes nothing.
static bool defaultInitLeavesScalarIndeterminateImpl(
    ASTContext &Ctx, QualType T, bool HonorUninitMarkers,
    llvm::SmallPtrSetImpl<const CXXRecordDecl *> &Visited,
    bool UntrustRoot = false) {
  if (T->isDependentType() || T->isIncompleteType())
    return false;
  if (const ArrayType *AT = Ctx.getAsArrayType(T))
    // An array of T default-initializes its elements: same question, same
    // root (an [[uninit]] S s[4] stands or falls with S itself).
    return defaultInitLeavesScalarIndeterminateImpl(
        Ctx, AT->getElementType(), HonorUninitMarkers, Visited, UntrustRoot);
  if (T->isReferenceType())
    return false;
  // An atomic object's state is its value type's: recurse (handles
  // _Atomic(struct) too, and _Atomic(std::byte) stays exempt through the
  // recursion). Same root, like the array arm.
  if (const auto *AT = T->getAs<AtomicType>())
    return defaultInitLeavesScalarIndeterminateImpl(
        Ctx, AT->getValueType(), HonorUninitMarkers, Visited, UntrustRoot);
  const auto *RD = T->getAsCXXRecordDecl();
  if (!RD)
    // Scalars, pointers, and enums are left indeterminate by default-init,
    // except std::byte, which the profile permits to be uninitialized
    // (paper §4), so a std::byte subobject does not make a record
    // indeterminate. Vector and matrix types are element packs of such
    // scalars and are equally indeterminate (precedent: -Wuninitialized's
    // isTrackedVar tracks scalar + vector). Sizeless (SVE/RVV) types are
    // deliberately not handled: they cannot be members or array elements,
    // and a bare local is caught by uninit_decl's no-initializer arm.
    return (T->isScalarType() || T->isVectorType() || T->isMatrixType()) &&
           !T->isStdByteType();
  if (RD->isInvalidDecl())
    return false;
  // A union's members are mutually exclusive, so the per-member walk below does
  // not apply. Default-initialization leaves it without an initialized member
  // (paper §6.5) unless it has a user-provided default constructor (trusted),
  // a default member initializer initializes one, or it has no members --
  // unnamed bit-fields are not members ([class.bit]) and cannot be named or
  // initialized, so a union of only unnamed bit-fields counts as empty.
  if (RD->isUnion()) {
    if (!UntrustRoot && RD->hasUserProvidedDefaultConstructor())
      return false;
    bool AnyMember = false;
    bool AllStdByte = true;
    for (const FieldDecl *F : RD->fields()) {
      if (F->isUnnamedBitField())
        continue;
      AnyMember = true;
      if (F->hasInClassInitializer())
        return false;
      // Default member initializers on the leaves of an anonymous-record
      // member activate that variant during default-initialization, so the
      // union is indeterminate iff the activated variant itself still is
      // (returning at the first match is sound: at most one variant may
      // carry default member initializers).
      if (F->isAnonymousStructOrUnion())
        if (const auto *FRD = F->getType()->getAsCXXRecordDecl();
            FRD && anyLeafHasNSDMI(FRD))
          return defaultInitLeavesScalarIndeterminateImpl(
              Ctx, F->getType(), HonorUninitMarkers, Visited);
      if (!Ctx.getBaseElementType(F->getType())->isStdByteType())
        AllStdByte = false;
    }
    // A union of only std::byte members (arrays included) mirrors the scalar
    // exemption above: the profile permits std::byte to be uninitialized
    // (paper §4.5), so whichever member is active needs no acknowledgement.
    if (!AnyMember || AllStdByte)
      return false;
    // Deliberately not exempted: a member whose type avoids indeterminacy
    // only through a trusted user-provided default constructor (union {
    // Trusted t; }) -- the union's default-initialization activates no
    // member, so that constructor never runs.
    return true;
  }
  // Break cycles from ill-formed self-containing types (e.g. struct S {S x;}).
  if (!Visited.insert(RD->getCanonicalDecl()).second)
    return false;
  // Trust a user-provided default constructor: ctor_uninit_member checks at its
  // definition.
  if (!UntrustRoot && RD->hasUserProvidedDefaultConstructor())
    return false;
  for (const CXXBaseSpecifier &Base : RD->bases())
    if (defaultInitLeavesScalarIndeterminateImpl(Ctx, Base.getType(),
                                                 HonorUninitMarkers, Visited))
      return true;
  for (const FieldDecl *F : RD->fields()) {
    if (F->isUnnamedBitField() || F->hasInClassInitializer())
      continue;
    // A member the type's author marked [[uninit]] is acknowledged as
    // intentionally uninitialized, so it does not leave an unacknowledged
    // scalar indeterminate (paper §6.2).
    if (HonorUninitMarkers && F->hasAttr<UninitAttr>())
      continue;
    if (defaultInitLeavesScalarIndeterminateImpl(Ctx, F->getType(),
                                                 HonorUninitMarkers, Visited))
      return true;
  }
  return false;
}

bool SemaProfiles::defaultInitLeavesScalarIndeterminate(QualType T) {
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
  return defaultInitLeavesScalarIndeterminateImpl(
      getASTContext(), T, /*HonorUninitMarkers=*/true, Visited);
}

// Whether RD has a default constructor explicitly defaulted *after* its
// first declaration: the one default-constructor form that is user-provided
// ([class.default.ctor]) while potentially initializing nothing. RD's
// record-level triviality bit misreports it forever -- addedMember saw the
// plain first declaration and marked the class non-trivial, and both the
// deferred recomputation at class completion and Sema::SpecialMemberIsTrivial
// refuse user-provided members -- so a caller that needs the truth must
// recompute it from the class shape (below).
static bool hasOutOfLineDefaultedDefaultCtor(const CXXRecordDecl *RD) {
  for (const CXXConstructorDecl *Ctor : RD->ctors()) {
    if (!Ctor->isDefaultConstructor() || Ctor->isDeleted())
      continue;
    for (const FunctionDecl *R : Ctor->redecls())
      if (R->isExplicitlyDefaulted() && R->isThisDeclarationADefinition())
        return true;
  }
  return false;
}

// Whether RD's explicitly-defaulted default constructor is trivial --
// initializes nothing at all -- recomputed from the class shape because
// every stored triviality bit misreports the out-of-line defaulted form
// (see above). The ingredients of [class.default.ctor]p3, each answered
// from subobject state that *is* accurate: no vtable pointer to write
// (dynamic class), no default member initializer anywhere it would apply
// (an anonymous-record member's leaves make that member's own record
// non-trivial, which the member check sees), and only trivially
// default-constructible bases and members. Conservative by construction: a
// subobject whose own bit is poisoned the same way reads non-trivial, so
// the recovery is missed there and the marker stays rejected -- never the
// reverse.
static bool defaultedDefaultCtorIsTrivial(ASTContext &Ctx,
                                          const CXXRecordDecl *RD) {
  if (RD->isDynamicClass())
    return false;
  for (const CXXBaseSpecifier &Base : RD->bases()) {
    const auto *BRD = Base.getType()->getAsCXXRecordDecl();
    if (!BRD || !BRD->hasDefinition() || !BRD->hasTrivialDefaultConstructor())
      return false;
  }
  for (const FieldDecl *F : RD->fields()) {
    if (F->hasInClassInitializer())
      return false;
    if (const auto *FRD =
            Ctx.getBaseElementType(F->getType())->getAsCXXRecordDecl())
      if (!FRD->hasDefinition() || !FRD->hasTrivialDefaultConstructor())
        return false;
  }
  return true;
}

std::optional<unsigned> SemaProfiles::defaultInitNonVacuityReason(QualType T) {
  QualType BaseTy = getASTContext().getBaseElementType(T);
  bool UntrustRoot = false;
  if (const auto *RD = BaseTy->getAsCXXRecordDecl()) {
    // hasTrivialDefaultConstructor asserts without a definition.
    if (!RD->hasDefinition())
      return 1;
    const CXXRecordDecl *Def = RD->getDefinition();
    // A deleted or absent default constructor keeps the triviality bit, but
    // makes default-initialization ill-formed rather than a no-op: the entity
    // can never be left default-initialized, so the marker is unsatisfiable.
    // Only a declared deleted constructor is visible here; a lazily
    // *implicitly* deleted one of an otherwise trivial type escapes this
    // scan -- a missed diagnostic (never a false positive), like the
    // framework's other conservative omissions.
    bool Deleted = !Def->hasDefaultConstructor();
    for (const CXXConstructorDecl *Ctor : Def->ctors())
      if (Ctor->isDefaultConstructor() && Ctor->isDeleted())
        Deleted = true;
    if (Deleted)
      return 2;
    // A non-trivial default constructor (user-provided anywhere in the
    // subtree, a default member initializer, a virtual table pointer)
    // initializes something, contradicting an [[uninit]] marker (paper §4.2
    // rule 2, §5.3).
    if (!RD->hasTrivialDefaultConstructor()) {
      // The record-level bit is poisoned for a default constructor
      // explicitly defaulted after its first declaration (see the helpers
      // above), so recompute the definition's triviality from the class
      // shape: when it is trivial, the defaulted definition runs no code,
      // and the marker's factual question falls to the walk below with the
      // root's user-provided trust -- exactly the trust that misreports
      // this class -- bypassed. Subobject trust stays intact, and only the
      // marker rules use this recovery: the uninit_decl walk keeps trusting
      // the class, which is what makes the constructor-definition check the
      // single diagnosis point. A marker written *before* the '= default'
      // definition has been parsed still sees the unrecovered state and
      // stays rejected (ProfilesFrameworkInternals.rst,
      // "Known Gaps (std::init)").
      if (!hasOutOfLineDefaultedDefaultCtor(Def) ||
          !defaultedDefaultCtorIsTrivial(getASTContext(), Def))
        return 0;
      UntrustRoot = true;
    }
  }
  // The factual (HonorUninitMarkers=false) walk: an all-scalars-determinate
  // type (e.g. an empty struct) has nothing uninitialized, so the marker
  // contradicts it too, while a type whose only indeterminate scalars are
  // themselves marked members really is left uninitialized.
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
  if (defaultInitLeavesScalarIndeterminateImpl(getASTContext(), T,
                                               /*HonorUninitMarkers=*/false,
                                               Visited, UntrustRoot))
    return std::nullopt;
  // Nothing is left indeterminate. The recovered out-of-line-defaulted root
  // still reads as running a constructor, matching its poisoned triviality
  // bit.
  return UntrustRoot ? 0 : 1;
}

// Documented at the declaration: the shape of the language's own plain
// default-initialization, shared between the marker rules' initializer
// guard below and getTrackedLocalAggregate's declaration-ran-nothing test.
bool SemaProfiles::isDefaultInitShape(const Expr *Init) {
  if (!Init)
    return true;
  const auto *CCE = dyn_cast<CXXConstructExpr>(Init->IgnoreImplicit());
  return CCE && CCE->getConstructor()->isDefaultConstructor() &&
         !isa<CXXTemporaryObjectExpr>(CCE) &&
         !CCE->requiresZeroInitialization() && !CCE->isListInitialization() &&
         CCE->getParenOrBraceRange().isInvalid();
}

void SemaProfiles::checkInitProfileUninitDecl(const VarDecl *Var) {
  // std::init / uninit_decl: a definition without any initializer (after
  // attempted default-initialization) must either carry [[uninit]] or
  // be initialized by a language rule. Static / thread storage duration is
  // excluded -- those are zero-initialized; runtime-init concerns are R3's.
  static constexpr StringRef Profile = "std::init";
  // Hoisted first rung of the gate: the call site is ungated and the work
  // below is not free.
  if (!isProfileLive(Profile))
    return;
  // A synthesized variable is no user declaration: the user can neither
  // initialize it nor mark it [[uninit]] (a coroutine's __promise, an OpenMP
  // private/reduction/linear copy). Same policy as -Wuninitialized's
  // isTrackedVar. Every user-visible uninitialized shape stays checked:
  // range-for variables and init-captures always carry initializers,
  // structured bindings are not implicit, and the block-scope anonymous
  // union VarDecl is checked *before* BuildAnonymousStructOrUnion calls
  // setImplicit() -- that ordering is load-bearing (see the matching comment
  // there).
  if (Var->isImplicit())
    return;
  if (Var->isInvalidDecl() || Var->getStorageDuration() != SD_Automatic ||
      Var->hasAttr<UninitAttr>())
    return;
  QualType BaseTy = getASTContext().getBaseElementType(Var->getType());
  // std::byte may be left uninitialized (paper §4), so it -- and arrays
  // of it -- are exempt from this rule.
  if (!BaseTy->isStdByteType() &&
      shouldEmitProfileViolation(Profile, diag::err_init_uninit_decl,
                                 Var->getLocation(), Var) &&
      // A definition with no initializer (scalar / pointer / enum, or an
      // array of them), or a class/aggregate type -- possibly the element
      // type of an array -- whose default-init leaves a scalar subobject
      // indeterminate (its synthesized constructor call provides an
      // initializer, so the !getInit() test alone misses it). The gate
      // stays ahead of the recursive type walk.
      (!Var->getInit() ||
       (BaseTy->isRecordType() &&
        defaultInitLeavesScalarIndeterminate(Var->getType())))) {
    // A union variable cannot carry [[uninit]] (union_marker bans it),
    // so it must be initialized; use a message that does not suggest the
    // marker as a remedy. An *anonymous* union's variable has no name and
    // its declaration admits no attribute or initializer syntax at all
    // (this check runs before BuildAnonymousStructOrUnion's setImplicit();
    // see the load-bearing-ordering comments there and above), so name the
    // one real remedy: an NSDMI on a member activates that variant, and the
    // anonymous-variant handling then counts the union as initialized.
    bool IsUnion = BaseTy->isUnionType();
    if (IsUnion && !Var->getDeclName())
      Diag(Var->getLocation(), diag::err_init_uninit_anon_union) << Profile;
    else
      Diag(Var->getLocation(),
           IsUnion ? diag::err_init_uninit_union : diag::err_init_uninit_decl)
          << Profile << Var->getDeclName();
  }
}

// The pointer-like types pointer_marker bans [[uninit]] on (paper §4.1:
// pointers must never be uninitialized): object/function pointers, member
// pointers, and block pointers. Deliberately not nullptr_t (reading one
// produces null without touching storage). All three keyed sites --
// pointer_marker itself, static_marker's skip, and the field-marker
// check's skip -- share this predicate so the exactly-one-diagnostic
// invariants hold by construction. ObjC object pointers are a follow-on
// (needs ObjC++ scaffolding).
static bool isPointerMarkerBannedType(QualType T) {
  return T->isPointerType() || T->isMemberPointerType() ||
         T->isBlockPointerType();
}

/// True if \p T names the target's va_list type, or the Microsoft-ABI one,
/// through its typedef: an opaque handle that va_start initializes, whatever
/// its representation.
static bool isBuiltinVaListSugar(const ASTContext &Ctx, QualType T) {
  while (const auto *TT = T->getAs<TypedefType>()) {
    const TypedefNameDecl *TD = TT->getDecl();
    if (TD == Ctx.getBuiltinVaListDecl() ||
        (Ctx.getTargetInfo().hasBuiltinMSVaList() &&
         TD == Ctx.getBuiltinMSVaListDecl()))
      return true;
    T = TD->getUnderlyingType();
  }
  return false;
}

namespace {
/// Which std::init marker rule owns the [[uninit]] on an entity, in the
/// fixed precedence the exactly-one-diagnostic invariants encode: the
/// subject's type first (union_marker / pointer_marker fire regardless of
/// storage duration or initializer state and retain the marker), then a
/// vacuously default-initialized -- hence zero-initialized -- static- or
/// thread-storage object (static_marker), then an initializer or a
/// non-vacuous default-initialization contradicting the marker
/// (uninit_with_initializer, in its written-initializer or type wording).
/// Consistent means the marker stands.
enum class UninitMarkerVerdict {
  Consistent,
  Union,
  Pointer,
  StaticStorage,
  WrittenInitializer,
  NonVacuousDefault,
};

/// A verdict plus, for NonVacuousDefault, note_init_uninit_marker_type's
/// select index.
struct UninitMarkerClass {
  UninitMarkerVerdict Verdict;
  unsigned Reason = 0;
};
} // namespace

/// The subject-type half of the classification: union_marker and
/// pointer_marker key on the base element type (and union membership) alone,
/// so the attribute handler can consult it before any initializer exists.
static UninitMarkerVerdict classifyUninitMarkerSubject(ASTContext &Ctx,
                                                       const ValueDecl *D) {
  QualType BaseTy = Ctx.getBaseElementType(D->getType());
  bool UnionMember =
      isa<FieldDecl>(D) && cast<FieldDecl>(D)->getParent()->isUnion();
  if (BaseTy->isUnionType() || UnionMember)
    return UninitMarkerVerdict::Union;
  if (isPointerMarkerBannedType(BaseTy) &&
      !isBuiltinVaListSugar(Ctx, D->getType()))
    return UninitMarkerVerdict::Pointer;
  return UninitMarkerVerdict::Consistent;
}

/// Classify the [[uninit]] on \p D against its initializer state \p Init
/// (a variable's attached initializer or a data member's NSDMI; null when
/// none exists). Every funnel acts on exactly the verdicts it owns, which is
/// what makes exactly one diagnostic fire per marked entity.
static UninitMarkerClass
classifyUninitMarker(SemaProfiles &SP, const ValueDecl *D, const Expr *Init) {
  ASTContext &Ctx = SP.getASTContext();
  UninitMarkerVerdict Subject = classifyUninitMarkerSubject(Ctx, D);
  if (Subject != UninitMarkerVerdict::Consistent)
    return {Subject};
  // A RecoveryExpr is a placeholder for an initialization that already
  // failed, not an initializer the user wrote.
  if (Init && isa<RecoveryExpr>(Init->IgnoreParens()))
    return {UninitMarkerVerdict::Consistent};
  QualType T = D->getType();
  if (isa<FieldDecl>(D) && !Init) {
    // The initializer-less data member: the marker's claim is about the
    // member type's default-initialization alone. std::byte may be left
    // uninitialized (paper §4), mirroring checkInitProfileUninitDecl.
    if (Ctx.getBaseElementType(T)->isStdByteType())
      return {UninitMarkerVerdict::Consistent};
    if (std::optional<unsigned> Reason = SP.defaultInitNonVacuityReason(T))
      return {UninitMarkerVerdict::NonVacuousDefault, *Reason};
    return {UninitMarkerVerdict::Consistent};
  }
  // A vacuous default-initialization -- no initializer at all (a scalar
  // default-init synthesizes none), or a synthesized trivial
  // default-constructor call that runs no code -- leaves the object
  // factually uninitialized, consistent with the marker; at static or
  // thread storage duration the object is nonetheless zero-initialized by
  // language rule, which is static_marker's contradiction (paper §3, §4.2).
  if (SemaProfiles::isDefaultInitShape(Init) &&
      (!Init || !SP.defaultInitNonVacuityReason(T))) {
    if (const auto *Var = dyn_cast<VarDecl>(D);
        Var && (Var->getStorageDuration() == SD_Static ||
                Var->getStorageDuration() == SD_Thread))
      return {UninitMarkerVerdict::StaticStorage};
    return {UninitMarkerVerdict::Consistent};
  }
  // A default-initialization that is not a no-op initializes something
  // without the user writing anything; anything else is a written
  // initializer.
  if (SemaProfiles::isDefaultInitShape(Init))
    return {UninitMarkerVerdict::NonVacuousDefault,
            *SP.defaultInitNonVacuityReason(T)};
  return {UninitMarkerVerdict::WrittenInitializer};
}

void SemaProfiles::checkInitProfileStaticMarker(const VarDecl *Var) {
  // std::init / static_marker: a variable with static or thread storage
  // duration is zero-initialized by language rule (paper §3), so it is an
  // initialized object; marking it [[uninit]] contradicts paper §4.2 ("an
  // initialized object marked [[uninit]] is an error"). This funnel owns the
  // classifier's StaticStorage verdict; the case with a real initializer --
  // explicit, or a default-initialization that is not a no-op -- is
  // uninit_with_initializer's (R4, in CheckCompleteVariableDeclaration), so
  // exactly one of the pair fires (this one runs first, from
  // ActOnUninitializedDecl, after the synthesized default-initialization is
  // attached).
  static constexpr StringRef Profile = "std::init";
  // Hoisted first rung of the gate: the call site is ungated and the work
  // below is not free.
  if (!isProfileLive(Profile))
    return;
  if (Var->isInvalidDecl() || (Var->getStorageDuration() != SD_Static &&
                               Var->getStorageDuration() != SD_Thread))
    return;
  // An inherited marker was written -- and classified -- on a previous
  // declaration; diagnosing it again on every redeclaration would only
  // repeat the answer. An instantiated marker is a fresh clone, so a
  // template's static data members still classify.
  const auto *UA = Var->getAttr<UninitAttr>();
  if (!UA || UA->isInherited())
    return;
  // Act only on the classifier's StaticStorage verdict: a union or pointer
  // subject is union_marker / pointer_marker's, and a contradicting
  // initializer is uninit_with_initializer's.
  if (classifyUninitMarker(*this, Var, Var->getInit()).Verdict !=
      UninitMarkerVerdict::StaticStorage)
    return;
  if (shouldEmitProfileViolation(Profile, diag::err_init_uninit_static_marker,
                                 Var->getLocation(), Var)) {
    bool IsThread = Var->getStorageDuration() == SD_Thread;
    Diag(Var->getLocation(), diag::err_init_uninit_static_marker)
        << Profile << Var->getDeclName() << IsThread;
  }
}

bool SemaProfiles::isConsistentUninitMarker(const VarDecl *Var) {
  return classifyUninitMarker(*this, Var, Var->getInit()).Verdict ==
         UninitMarkerVerdict::Consistent;
}

bool SemaProfiles::checkInitProfileStaticRuntimeInit(
    const VarDecl *Var, llvm::function_ref<bool()> CheckConstInit) {
  static constexpr StringRef Profile = "std::init";
  if (!isProfileLive(Profile))
    return false;
  // Thread-locals have thread (not static) storage duration; paper §3 scopes
  // this rule to non-local *static* objects (uninit_decl likewise excludes
  // thread storage).
  if (Var->getTLSKind() != VarDecl::TLS_None)
    return false;
  // std::init / static_runtime_init: paper says non-local statics must be
  // initialized at compile or link time. CheckConstInit() permits trivial
  // default initialization (not a constant initializer but needs no global
  // constructor), so a zero-initialized aggregate such as
  // `struct S { int x; }; S g;` is not a violation. Runs before
  // -Wglobal-constructors so the profile error (when enforced) takes
  // precedence over the standalone warning.
  // Gate on enforcement before evaluating the initializer: this call site
  // sits ahead of -Wglobal-constructors' isIgnored guard, so evaluating
  // first would charge every global with a non-constant initializer for the
  // constant-initializer evaluation even with profiles disabled.
  if (!shouldEmitProfileViolation(Profile, diag::err_init_static_runtime_init,
                                  Var->getLocation(), Var))
    return false;
  if (CheckConstInit())
    return false;
  Diag(Var->getLocation(), diag::err_init_static_runtime_init)
      << Profile << Var->getDeclName();
  return true;
}

void SemaProfiles::checkInitProfileUninitWithInitializer(const ValueDecl *D,
                                                         const Expr *Init) {
  static constexpr StringRef Profile = "std::init";
  if (!isProfileLive(Profile))
    return;
  // [[uninit]] documents that the entity is intentionally left
  // uninitialized, so it contradicts an explicit initializer. A RecoveryExpr
  // is a placeholder for an initialization that already failed (e.g.
  // default-init of a const scalar), not an initializer the user wrote, so it
  // must not trigger this rule.
  if (!D->hasAttr<UninitAttr>() || !Init ||
      isa<RecoveryExpr>(Init->IgnoreParens()))
    return;
  SourceLocation Loc = D->getLocation();
  // Gate the (possibly recursive) type walk below on enforcement.
  if (!shouldEmitProfileViolation(
          Profile, diag::err_init_uninit_with_initializer, Loc, D))
    return;
  // Act only on the classifier's initializer verdicts: the subject-type and
  // static-storage contradictions belong to union_marker / pointer_marker /
  // static_marker, and a Consistent entity really is left uninitialized, to
  // be initialized later (e.g. via construct_at).
  UninitMarkerClass C = classifyUninitMarker(*this, D, Init);
  bool IsMember = isa<FieldDecl>(D);
  // The marker is contradicted for one of two reasons, which read very
  // differently to a user. A *default*-initialization that is not a no-op
  // initializes something without the user writing anything, so saying the
  // entity "has an initializer" would be wrong; report the type and why, as
  // the initializer-less data member flavor in
  // checkStdInitUninitFieldMarker does.
  switch (C.Verdict) {
  case UninitMarkerVerdict::NonVacuousDefault: {
    QualType BaseTy = getASTContext().getBaseElementType(D->getType());
    Diag(Loc, diag::err_init_uninit_not_left_uninitialized)
        << Profile << D->getDeclName() << D->getType() << IsMember;
    SourceLocation NoteLoc = Loc;
    if (const auto *DD = dyn_cast<DeclaratorDecl>(D))
      NoteLoc = DD->getTypeSpecStartLoc();
    Diag(NoteLoc, diag::note_init_uninit_marker_type) << BaseTy << C.Reason;
    return;
  }
  case UninitMarkerVerdict::WrittenInitializer:
    Diag(Loc, diag::err_init_uninit_with_initializer)
        << Profile << D->getDeclName() << IsMember;
    return;
  case UninitMarkerVerdict::Consistent:
  case UninitMarkerVerdict::Union:
  case UninitMarkerVerdict::Pointer:
  case UninitMarkerVerdict::StaticStorage:
    return;
  }
  llvm_unreachable("covered switch");
}

void SemaProfiles::checkInitProfileMarkerPlacement(const Decl *D) {
  if (!isProfileLive("std::init"))
    return;
  const auto *UA = D->getAttr<UninitAttr>();
  // An undeduced subject is checked once deduced (checkDeducedMarkers).
  if (!UA || cast<ValueDecl>(D)->getType()->isUndeducedType())
    return;
  SourceLocation Loc = UA->getLocation();

  // std::init / union_marker (paper §5.6): the marker is banned on a union
  // object or a union member, because delayed initialization by assigning a
  // member would be an erroneous assignment when compiled without the profile.
  // std::init / pointer_marker (paper §4.1): "a reference cannot be
  // uninitialized. The initialization profile requires the same for pointers."
  // A pointer must instead be initialized (e.g. to nullptr). Both are profile
  // policy (not a meaningless subject), so they are gated on enforcement; the
  // marker is left in place so uninit_decl / ctor_uninit_member treat the
  // entity as acknowledged rather than re-diagnosing it.
  //
  // Passing \p D makes shouldEmitProfileViolation defer on a templated pattern
  // (paper / P3589R2: a rule fires on the instantiation, not the template),
  // so the parse-time handler skips template members and the rule is re-run on
  // the instantiated entity (VisitFieldDecl / VisitVarDecl), once the
  // substituted type is known to be a pointer or union.
  //
  // Both rules key on the base element type: an array of unions or pointers
  // leaves the same uninitialized elements as a single one, and the marker
  // would otherwise slip past uninit_decl (which trusts marked declarations)
  // entirely. The union rule also covers a union-typed data member of a
  // non-union class -- delayed initialization by assigning its member is just
  // as erroneous there (paper §5.6).
  switch (classifyUninitMarkerSubject(getASTContext(), cast<ValueDecl>(D))) {
  case UninitMarkerVerdict::Union:
    if (shouldEmitProfileViolation("std::init", diag::err_init_union_marker,
                                   Loc, D)) {
      bool UnionMember =
          isa<FieldDecl>(D) && cast<FieldDecl>(D)->getParent()->isUnion();
      unsigned Select = UnionMember ? 1 : isa<FieldDecl>(D) ? 2 : 0;
      Diag(Loc, diag::err_init_union_marker) << "std::init" << Select;
    }
    break;
  case UninitMarkerVerdict::Pointer:
    if (shouldEmitProfileViolation(
            "std::init", diag::err_init_uninit_pointer_marker, Loc, D))
      Diag(Loc, diag::err_init_uninit_pointer_marker) << "std::init";
    break;
  default:
    break;
  }
}

bool SemaProfiles::diagnoseInvalidUninitMarker(const Decl *D,
                                               SourceLocation AttrLoc,
                                               bool Diagnose) {
  const auto *VD = dyn_cast<ValueDecl>(D);
  if (!VD)
    return false;
  QualType T = VD->getType();

  // A dependent or undeduced subject is validated once its type is known; see
  // the declaration.
  if (T->isDependentType() || T->isUndeducedType())
    return false;

  if (T->isReferenceType()) {
    if (Diagnose)
      Diag(AttrLoc, diag::err_uninit_attr_invalid_subject) << /*Reference=*/0u;
    return true;
  }
  return false;
}

bool SemaProfiles::isHandleType(QualType T) {
  if (T.isNull() || T->isDependentType() || T->isReferenceType())
    return false;
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  return RD && !RD->isUnion();
}

bool SemaProfiles::diagnoseInvalidRefToUninitMarker(const Decl *D,
                                                    SourceLocation AttrLoc,
                                                    bool Diagnose) {
  // A structured binding names the members it decomposes, which carry their
  // own markers, whatever its type.
  if (isa<DecompositionDecl>(D)) {
    if (Diagnose)
      Diag(AttrLoc, diag::err_ref_to_uninit_attr_structured_binding);
    return true;
  }

  QualType T;
  if (const auto *FD = dyn_cast<FunctionDecl>(D))
    T = FD->getReturnType();
  else
    T = cast<ValueDecl>(D)->getType();

  // A dependent or undeduced subject is validated once its type is known; see
  // the declaration.
  if (T->isDependentType() || T->isUndeducedType())
    return false;

  if (isHandleType(T)) {
    // An owning smart pointer with static or thread storage is destroyed at
    // program or thread exit, where no destroy of its referent is judged.
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!VD || VD->hasLocalStorage() || !isStdOwnerType(T))
      return false;
    if (Diagnose)
      Diag(AttrLoc, diag::err_ref_to_uninit_attr_static_owner)
          << (VD->getTLSKind() != VarDecl::TLS_None);
    return true;
  }
  if ((!T->isPointerType() && !T->isReferenceType()) ||
      T->isFunctionPointerType() || T->isFunctionReferenceType()) {
    if (Diagnose)
      Diag(AttrLoc, diag::err_ref_to_uninit_attr_invalid_type);
    return true;
  }
  return false;
}

void SemaProfiles::checkDeducedMarkers(Decl *D) {
  if (const auto *VD = dyn_cast<VarDecl>(D);
      VD && !VD->getType()->getContainedDeducedType())
    return;
  // Silently in a SFINAE context, like the instantiation re-check: the marker
  // must not affect overload resolution.
  bool Diagnose = !SemaRef.isSFINAEContext();
  if (const auto *A = D->getAttr<RefToUninitAttr>();
      A && diagnoseInvalidRefToUninitMarker(D, A->getLocation(), Diagnose))
    D->dropAttr<RefToUninitAttr>();
  if (const auto *A = D->getAttr<UninitAttr>()) {
    if (diagnoseInvalidUninitMarker(D, A->getLocation(), Diagnose))
      D->dropAttr<UninitAttr>();
    else
      checkInitProfileMarkerPlacement(D);
  }
}

void SemaProfiles::checkNowInitVacuity(FunctionDecl *FD) {
  // [[now_init]] asserts that the callee initializes the storage bound to
  // each of its [[ref_to_uninit]] parameters (P4222R2 §6.2), so a function
  // with no marked parameter would make it a vacuous promise; reject it. The
  // check runs from ActOnFunctionDeclarator, after CheckFunctionDeclaration
  // has merged the parameters' attributes from any previous declaration, so
  // a marker written on any declaration of the function counts. An inherited
  // [[now_init]] is skipped -- the declaration that wrote it was already
  // checked, and re-diagnosing a merged copy would blame the wrong
  // declaration. A dependent parameter's marker is attached to the pattern
  // unvalidated (its type check defers to instantiation), so a template with
  // a marked dependent parameter passes; should the instantiation drop that
  // marker, the inherited [[now_init]] goes inert rather than re-diagnosed,
  // like the dropped marker itself (instantiations never re-enter this
  // check). Like the other marker subject checks, this fires regardless of
  // -fprofiles.
  const auto *A = FD->getAttr<NowInitAttr>();
  if (!A || A->isInherited())
    return;

  if (llvm::any_of(FD->parameters(), [](const ParmVarDecl *P) {
        return P->hasAttr<RefToUninitAttr>();
      }))
    return;

  Diag(A->getLocation(), diag::err_now_init_attr_no_marked_parameter);
  FD->dropAttr<NowInitAttr>();
}

// std::init / ref_to_uninit (paper §5). Two mutually-recursive local
// recognizers over the syntactic form of a source expression -- no flow
// analysis and no type-system tracking. Uninitialized storage is only ever
// introduced by an explicit [[uninit]] / [[ref_to_uninit]] marker.
//
// A recognized form is Initialized or Uninitialized; an unrecognized one
// (pointer arithmetic, an integer-to-pointer cast, a call through a function
// pointer) is Unknown rather than assumed Initialized; and a conditional whose
// arms are Initialized and Uninitialized is Mixed (P4222R2 §4.9: mixing
// initialized and uninitialized memory in one expression requires
// suppression), which both binding directions reject and which the
// read-through, subobject-write, and destroy checks treat as Uninitialized.
// Callers wanting a plain "is it uninitialized?" answer treat Unknown as not
// uninitialized.
//
// How the classified expression is being accessed is carried by
// UninitAccessOpts below.
enum class UninitStorage { Initialized, Uninitialized, Unknown, Mixed };

/// True for the states a "may be uninitialized" consumer acts on:
/// Uninitialized, and Mixed (one arm is).
static bool isUninitializedOrMixed(UninitStorage S) {
  return S == UninitStorage::Uninitialized || S == UninitStorage::Mixed;
}

// How an expression is being used, for the uninit recognizers.
//
// DropTopLevelUninit: a *directly named* [[uninit]] entity does not count as
// uninitialized. A value access of such an entity is owned elsewhere: a named
// [[uninit]] object by the CFG uninit_read pass, a current-object member by
// the ctor-body pass, an [[uninit]] member of a constructor-less aggregate
// local by the local-aggregate pass (all three track assignments), and a
// marked member of an object with a user-provided constructor reached through
// any other object is trusted (paper §5.1: its constructor body
// may have assigned it, which local analysis cannot see). So the read-through
// check must not second-guess them -- and for a store, writing the whole
// named entity IS its initialization (paper §4.5: for a built-in type, a
// write is its initialization). The flag is cleared at the first *deeper*
// subobject step (a member's member, an array element), where no flow pass
// tracks the storage and only whole-object construct_at could re-initialize
// (paper §5.4).
//
// TrustRefToUninit: [[ref_to_uninit]] markers are ignored -- the storage
// reached through a marked pointer/reference (or returned by a marked
// function) classifies as Unknown rather than Uninitialized. Stores use
// this, and only at the top level: a scalar write through the marker is
// the whole pointee's initialization (paper §4.5), so it must be neither
// banned nor endorsed -- while below a member step only whole-object
// construct_at could initialize, which is unmodeled, so the member arm
// clears the trust and the marker counts again (§5.4's piecemeal ban).
//
// Alias: how a binding aliases a glvalue *of pointer type* (P4222R2 §4.3),
// set by the binding funnel from the bound type. A read-only alias (T *const
// &, T *&&) denotes whatever the pointer's value points to; a mutable alias
// (T *&) denotes the pointer object, whose marking the alias cannot carry, so
// it classifies Unknown. None everywhere else, including every recursion below
// the top level. See classifyPointerGlvalue.
//
// ThisUnderConstruction: `this` denotes an object under construction (the
// classification runs in a constructor body, a mem-initializer, or a default
// member initializer), so the current object's members may not be initialized
// yet and `this` classifies Unknown; otherwise `this` is Initialized. Set by
// the checking entry points from SemaProfiles::thisIsUnderConstruction.
struct UninitAccessOpts {
  enum class PointerAlias { None, ReadOnly, Mutable };

  bool DropTopLevelUninit = false;
  bool TrustRefToUninit = false;
  /// The [[uninit]] marker of a data member does not count at any level: a
  /// read or store below one is trusted where the CFG pass does not track it
  /// (P4222R2 §5 exposure is not analyzed; ProfilesFrameworkInternals.rst,
  /// "Known Gaps (std::init)").
  bool TrustUninitMembers = false;
  PointerAlias Alias = PointerAlias::None;
  bool ThisUnderConstruction = false;
  /// The arms a classified leaf takes at its nested conditionals
  /// (SemaProfiles::ShapeLeaf); a conditional with no listed arm combines
  /// both.
  ArrayRef<const Expr *> Arms;
  /// The locals the outermost handle classification has followed
  /// (classifyHandleSource), shared by the classifications it nests.
  SemaProfiles::HandleWalkMemo *HandleMemo = nullptr;

  // Copy-then-mutate, so each helper names only the field it changes and
  // adding a field cannot silently drop out of a positional rebuild.
  UninitAccessOpts withoutTopLevelDrop() const {
    UninitAccessOpts O = *this;
    O.DropTopLevelUninit = false;
    return O;
  }
  UninitAccessOpts withoutMarkerTrust() const {
    UninitAccessOpts O = *this;
    O.TrustRefToUninit = false;
    return O;
  }
  UninitAccessOpts withAlias(PointerAlias A) const {
    UninitAccessOpts O = *this;
    O.Alias = A;
    return O;
  }
  UninitAccessOpts withThisUnderConstruction(bool B) const {
    UninitAccessOpts O = *this;
    O.ThisUnderConstruction = B;
    return O;
  }
  UninitAccessOpts withArms(ArrayRef<const Expr *> A) const {
    UninitAccessOpts O = *this;
    O.Arms = A;
    return O;
  }
  UninitAccessOpts withUninitMemberTrust(bool B) const {
    UninitAccessOpts O = *this;
    O.TrustUninitMembers = B;
    return O;
  }
  UninitAccessOpts withHandleMemo(SemaProfiles::HandleWalkMemo *M) const {
    UninitAccessOpts O = *this;
    O.HandleMemo = M;
    return O;
  }
};

SemaProfiles::PointerAliasKind SemaProfiles::pointerAliasKind(QualType T) {
  if (T.isNull() || !T->isReferenceType())
    return PointerAliasKind::None;
  QualType Referent = T->getPointeeType();
  if (!Referent->isPointerType())
    return PointerAliasKind::None;
  return Referent.isConstQualified() || T->isRValueReferenceType()
             ? PointerAliasKind::ReadOnly
             : PointerAliasKind::Mutable;
}

/// SemaProfiles::pointerAliasKind as the recognizers' option value.
static UninitAccessOpts::PointerAlias pointerAliasOf(QualType T) {
  switch (SemaProfiles::pointerAliasKind(T)) {
  case SemaProfiles::PointerAliasKind::None:
    return UninitAccessOpts::PointerAlias::None;
  case SemaProfiles::PointerAliasKind::ReadOnly:
    return UninitAccessOpts::PointerAlias::ReadOnly;
  case SemaProfiles::PointerAliasKind::Mutable:
    return UninitAccessOpts::PointerAlias::Mutable;
  }
  llvm_unreachable("unknown PointerAliasKind");
}

// Presets: a binding source (markers count everywhere), a value read (the
// top-level drop applies, and data member markers are trusted), and a scalar
// store (additionally, storage reached through [[ref_to_uninit]] is trusted).
constexpr UninitAccessOpts UninitBindAccess{};
constexpr UninitAccessOpts UninitReadAccess{/*DropTopLevelUninit=*/true,
                                            /*TrustRefToUninit=*/false,
                                            /*TrustUninitMembers=*/true};
constexpr UninitAccessOpts UninitWriteAccess{/*DropTopLevelUninit=*/true,
                                             /*TrustRefToUninit=*/true,
                                             /*TrustUninitMembers=*/true};

// Combine the arms of a conditional: equal arms keep their state; Mixed
// absorbs everything; Initialized with Uninitialized is Mixed (P4222R2 §4.9);
// Uninitialized with Unknown is Uninitialized (either arm may be taken and one
// is known bad); Initialized with Unknown is Unknown.
static UninitStorage combineArms(UninitStorage A, UninitStorage B) {
  if (A == B)
    return A;
  if (A == UninitStorage::Mixed || B == UninitStorage::Mixed)
    return UninitStorage::Mixed;
  if (A == UninitStorage::Unknown)
    return B == UninitStorage::Uninitialized ? B : UninitStorage::Unknown;
  if (B == UninitStorage::Unknown)
    return A == UninitStorage::Uninitialized ? A : UninitStorage::Unknown;
  return UninitStorage::Mixed;
}

static UninitStorage
glvalueDenotesUninitStorage(ASTContext &Ctx, const Expr *E,
                            UninitAccessOpts Opts = UninitBindAccess);

/// The expression a structured binding decomposes -- the member (a member
/// binding, whose field carries the marker) or the get<> call (a tuple-like
/// binding) -- when \p E names a BindingDecl; null otherwise. The recognizers
/// classify that expression in the binding's place.
static const Expr *getStructuredBindingSource(const Expr *E) {
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    if (const auto *BD = dyn_cast<BindingDecl>(DRE->getDecl()))
      return BD->getBinding();
  return nullptr;
}

const ValueDecl *SemaProfiles::getDirectlyNamedDecl(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  // A structured binding names the member it decomposes, marker included.
  if (const Expr *Bound = getStructuredBindingSource(E))
    return getDirectlyNamedDecl(Bound);
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl();
  return nullptr;
}

/// True for the builtin ID of a standard identity function
/// (SemaProfiles::isIdentityCall).
static bool isIdentityBuiltin(unsigned BuiltinID) {
  switch (BuiltinID) {
  case Builtin::BImove:
  case Builtin::BImove_if_noexcept:
  case Builtin::BIforward:
  case Builtin::BIforward_like:
  case Builtin::BIas_const:
    return true;
  default:
    return false;
  }
}

bool SemaProfiles::isShapeTransparentBuiltin(unsigned BuiltinID) {
  return isIdentityBuiltin(BuiltinID) || BuiltinID == Builtin::BIaddressof ||
         BuiltinID == Builtin::BI__addressof;
}

bool SemaProfiles::isIdentityCall(const CallExpr *CE) {
  return isIdentityBuiltin(CE->getBuiltinCallee()) && CE->getNumArgs() == 1;
}

const Expr *SemaProfiles::addressOfOperand(const Expr *E) {
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_AddrOf)
    return UO->getSubExpr();
  if (const auto *CE = dyn_cast<CallExpr>(E); CE && CE->getNumArgs() == 1) {
    switch (CE->getBuiltinCallee()) {
    case Builtin::BIaddressof:
    case Builtin::BI__addressof:
    case Builtin::BI__builtin_addressof:
      return CE->getArg(0);
    default:
      break;
    }
  }
  return nullptr;
}

const BinaryOperator *SemaProfiles::asDataMemberPointerAccess(const Expr *E) {
  const auto *BO = dyn_cast<BinaryOperator>(E);
  return BO && BO->isPtrMemOp() &&
                 BO->getRHS()->getType()->isMemberDataPointerType()
             ? BO
             : nullptr;
}

/// The arms of the conditional \p CO; the GNU \c ?: form's true arm is
/// its written common operand.
static std::pair<const Expr *, const Expr *>
conditionalArms(const AbstractConditionalOperator *CO) {
  if (const auto *BCO = dyn_cast<BinaryConditionalOperator>(CO))
    return {BCO->getCommon(), BCO->getFalseExpr()};
  return {CO->getTrueExpr(), CO->getFalseExpr()};
}

/// The arm of the conditional \p E listed in \p Arms, or null.
static const Expr *chosenArm(const Expr *E, ArrayRef<const Expr *> Arms) {
  if (const auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
    auto [T, F] = conditionalArms(CO);
    if (llvm::is_contained(Arms, T))
      return T;
    if (llvm::is_contained(Arms, F))
      return F;
  }
  return nullptr;
}

/// The operand a pass-through form \p E yields its lvalue or value from, or
/// null: a comma's right operand, a single-element braced list's element, a
/// GNU statement expression's result, a pointer compound literal's
/// initializer, an opaque value's source, an identity call's argument
/// (SemaProfiles::isIdentityCall), and the arm of a conditional listed in
/// \p Arms.
static const Expr *passThroughOperand(const Expr *E,
                                      ArrayRef<const Expr *> Arms) {
  if (const auto *BO = dyn_cast<BinaryOperator>(E); BO && BO->isCommaOp())
    return BO->getRHS();
  if (const auto *ILE = dyn_cast<InitListExpr>(E);
      ILE && ILE->getNumInits() == 1)
    return ILE->getInit(0);
  if (const auto *SE = dyn_cast<StmtExpr>(E)) {
    const auto *Last =
        dyn_cast_or_null<ValueStmt>(SE->getSubStmt()->body_back());
    return Last ? Last->getExprStmt() : nullptr;
  }
  if (const auto *CLE = dyn_cast<CompoundLiteralExpr>(E);
      CLE && CLE->getType()->isPointerType())
    return CLE->getInitializer();
  if (const auto *OVE = dyn_cast<OpaqueValueExpr>(E))
    return OVE->getSourceExpr();
  if (const auto *CE = dyn_cast<CallExpr>(E);
      CE && SemaProfiles::isIdentityCall(CE))
    return CE->getArg(0);
  return chosenArm(E, Arms);
}

// Implicit casts are re-stripped after every peel -- a cast's operand may
// itself be parenthesized or implicitly converted -- so a single leading
// IgnoreParenImpCasts is not equivalent.
const Expr *SemaProfiles::ignoreTransparentCasts(const Expr *E,
                                                 ArrayRef<const Expr *> Arms) {
  while (true) {
    E = E->IgnoreParenImpCasts();
    if (const auto *CE = dyn_cast<ExplicitCastExpr>(E)) {
      const Expr *Sub = CE->getSubExpr();
      if (!Sub->getType()->isPointerType() && !Sub->isGLValue())
        return E;
      E = Sub;
      continue;
    }
    if (const Expr *Op = passThroughOperand(E, Arms)) {
      E = Op;
      continue;
    }
    return E;
  }
}

/// The pass-through forms (passThroughOperand) as the pointer and glvalue
/// recognizers classify them: a form yielding one operand classifies as that
/// operand (a single-element braced list { e } binds from e, modeling
/// MismatchingNewDeleteDetector::getNewExprFromInitListOrExpr), and a
/// conditional with no arm listed in \p Arms combines its arms (combineArms),
/// so a value that may be uninit forces a marked target. \p EmptyListState
/// classifies an empty braced list: {} value-initializes a pointer to null,
/// which the pointer recognizer classifies Unknown (the null policy at its
/// null arm), while a glvalue has no empty-list form (Unknown); a
/// multi-element list is Unknown for both. Returns std::nullopt when \p E is
/// not a pass-through form.
template <typename RecurseFn>
static std::optional<UninitStorage>
classifyUninitPassThrough(const Expr *E, UninitStorage EmptyListState,
                          ArrayRef<const Expr *> Arms, RecurseFn Recurse) {
  if (const auto *ILE = dyn_cast<InitListExpr>(E);
      ILE && ILE->getNumInits() != 1)
    return ILE->getNumInits() == 0 ? EmptyListState : UninitStorage::Unknown;
  if (const Expr *Op = passThroughOperand(E, Arms))
    return Recurse(Op);
  if (const auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
    auto [T, F] = conditionalArms(CO);
    return combineArms(Recurse(T), Recurse(F));
  }
  return std::nullopt;
}

/// What a known callee's return value is as an uninit source (P4222R2 §4.3:
/// functions like malloc "must be known to an analyzer enforcing the
/// initialization profile").
enum class KnownCalleeReturn {
  /// Returns freshly allocated, uninitialized storage -- classified like a
  /// [[ref_to_uninit]] return, including the trusted-marker degradation.
  ReturnsUninitialized,
  /// Returns initialized (zero-filled) storage.
  ReturnsInitialized,
  /// Returns storage affirmatively neither -- realloc's preserved prefix
  /// plus indeterminate tail.
  ReturnsUnknown,
  /// Returns what an unmarked direct callee returns: trusted as initialized
  /// (a release callee's void return, a std lifecycle function's result).
  ReturnsTrusted,
  /// Returns a handle owning a default-initialized object of the type of
  /// the callee's first template argument (std::make_unique_for_overwrite):
  /// uninitialized where that default-initialization leaves a scalar
  /// indeterminate.
  ReturnsDefaultInitialized,
};

// True for a direct declaration of replaceable global operator new /
// operator new[]: raw allocation returning uninitialized memory like malloc
// (a new-*expression* is the recognizers' CXXNewExpr arm instead). The
// operator-name check excludes operator delete, and replaceability excludes
// class-specific overloads, whose semantics belong to their class.
static bool isReplaceableGlobalOperatorNew(const FunctionDecl *FD) {
  OverloadedOperatorKind OO = FD->getDeclName().getCXXOverloadedOperator();
  return (OO == OO_New || OO == OO_Array_New) &&
         FD->isReplaceableGlobalAllocationFunction();
}

// True for replaceable global operator delete / operator delete[] -- sized
// and nothrow forms included, class-specific and destroying overloads
// excluded.
static bool isReplaceableGlobalOperatorDelete(const FunctionDecl *FD) {
  return FD->getDeclName().isAnyOperatorDelete() &&
         FD->isReplaceableGlobalAllocationFunction();
}

/// True for a member function named \p Name of a namespace-std class named
/// \p Class (any specialization).
static bool isStdClassMember(const FunctionDecl *FD, StringRef Class,
                             StringRef Name) {
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (!MD || !MD->getIdentifier() || MD->getName() != Name)
    return false;
  const CXXRecordDecl *RD = MD->getParent();
  return RD->getIdentifier() && RD->getName() == Class &&
         RD->isInStdNamespace();
}

/// std::allocator<T>::allocate and std::allocator_traits<A>::allocate with a
/// pointer result: storage for objects that are not yet constructed
/// ([allocator.members]p5).
static bool isStdAllocate(const FunctionDecl *FD) {
  return (isStdClassMember(FD, "allocator", "allocate") ||
          isStdClassMember(FD, "allocator_traits", "allocate")) &&
         FD->getReturnType()->isPointerType();
}

/// std::allocator<T>::deallocate(T *, size_t).
static bool isStdAllocatorDeallocate(const FunctionDecl *FD) {
  return isStdClassMember(FD, "allocator", "deallocate") &&
         FD->getNumParams() == 2 &&
         FD->getParamDecl(0)->getType()->isPointerType();
}

/// std::allocator_traits<A>::deallocate(A &, pointer, size_type) where
/// pointer is a pointer type.
static bool isStdAllocatorTraitsDeallocate(const FunctionDecl *FD) {
  return isStdClassMember(FD, "allocator_traits", "deallocate") &&
         FD->getNumParams() == 3 &&
         FD->getParamDecl(1)->getType()->isPointerType();
}

/// std::allocator_traits<A>::construct(A &, T *, Args &&...), the
/// allocator-aware construct_at ([allocator.traits.members]p5); a fancy
/// pointer is a class and falls outside.
static bool isStdAllocatorTraitsConstruct(const FunctionDecl *FD) {
  return isStdClassMember(FD, "allocator_traits", "construct") &&
         FD->getNumParams() >= 2 &&
         FD->getParamDecl(1)->getType()->isPointerType();
}

/// std::allocator_traits<A>::destroy(A &, T *), the allocator-aware
/// destroy_at ([allocator.traits.members]p6).
static bool isStdAllocatorTraitsDestroy(const FunctionDecl *FD) {
  return isStdClassMember(FD, "allocator_traits", "destroy") &&
         FD->getNumParams() == 2 &&
         FD->getParamDecl(1)->getType()->isPointerType();
}

/// True if the allocator a member \p FD of std::allocator_traits<A> serves,
/// or one of its bases, declares a member named \p Name outside namespace std:
/// the traits call it in place of construct_at or destroy_at
/// ([allocator.traits.members]p5, p6), and it may do anything.
static bool allocatorHasOwnMember(const FunctionDecl *FD, StringRef Name) {
  const auto *Traits = dyn_cast<ClassTemplateSpecializationDecl>(
      cast<CXXMethodDecl>(FD)->getParent());
  if (!Traits || Traits->getTemplateArgs().size() == 0 ||
      Traits->getTemplateArgs()[0].getKind() != TemplateArgument::Type)
    return false;
  const CXXRecordDecl *Alloc =
      Traits->getTemplateArgs()[0].getAsType()->getAsCXXRecordDecl();
  IdentifierInfo &II = FD->getASTContext().Idents.get(Name);
  auto InStd = [](const Decl *D) {
    for (const DeclContext *DC = D->getDeclContext(); DC; DC = DC->getParent())
      if (DC->isStdNamespace())
        return true;
    return false;
  };
  SmallVector<const CXXRecordDecl *, 4> Worklist;
  llvm::SmallPtrSet<const CXXRecordDecl *, 4> Seen;
  if (Alloc)
    Worklist.push_back(Alloc);
  while (!Worklist.empty()) {
    const CXXRecordDecl *RD = Worklist.pop_back_val()->getDefinition();
    if (!RD || !Seen.insert(RD).second)
      continue;
    if (!InStd(RD) && !RD->lookup(&II).empty())
      return true;
    for (const CXXBaseSpecifier &B : RD->bases())
      if (const CXXRecordDecl *Base = B.getType()->getAsCXXRecordDecl())
        Worklist.push_back(Base);
  }
  return false;
}

namespace {
using KnownParamRole = SemaProfiles::KnownParamRole;

/// How a row of the known-callee table (KnownCallees) is keyed.
enum class KnownCalleeKey : uint8_t {
  /// A Clang builtin ID: the C allocation, memory, and va_list families.
  Builtin,
  /// A form predicate: the operator new/delete families, which never carry a
  /// builtin ID, and the std::allocator and std::allocator_traits members.
  Form,
  /// A namespace-std function name and arity: the std lifecycle functions.
  StdName,
  /// A std::ranges customization point object's name and call arity.
  RangesCPO,
};

/// The shape a name-keyed row's role parameter must have in the called
/// declaration for the row to match.
enum class ParamShape : uint8_t {
  /// An object pointer: a pointer iterator.
  Pointer,
  /// A reference to an array: an array range.
  ArrayReference,
  /// A non-union class or a reference to one: a class-type iterator or range.
  Handle,
  /// Any type: a row whose role takes no storage of a particular shape.
  Any,
};

/// A row of the known-callee table (KnownCallees): what a library callee
/// returns and what it does to the storage bound to one role parameter. A row
/// is keyed by builtin ID, by a form predicate, by a std name and arity, or by
/// a std::ranges object's name and call arity (KnownCalleeKey) -- the library
/// knowledge P4222R2 §4.4 asks for, looked up at each call, so the library's
/// declarations carry no annotation and a module built without -fprofiles reads
/// the same. A builtin ID is absent under -fno-builtin / -ffreestanding (or on
/// a non-matching declaration), where an allocator falls back to the trusted
/// default and a release or initialization goes unrecognized -- a missed
/// diagnostic or a missed relaxation, never a false positive or a false
/// acceptance elsewhere. (getBuiltinFunctionEffects in SemaFunctionEffects.cpp
/// groups almost exactly the allocator set for the \c allocating effect.)
struct KnownCalleeEntry {
  /// The Arity of a name-keyed row matching any parameter count.
  static constexpr unsigned AnyArity = ~0u;
  /// The EndParam of a row whose role parameter has no end iterator.
  static constexpr unsigned NoEndParam = ~0u;

  /// What the row is keyed by.
  KnownCalleeKey Key = KnownCalleeKey::Builtin;
  /// The Clang builtin ID of a builtin row; 0 otherwise.
  unsigned BuiltinID = 0;
  /// The form predicate of a form row; null otherwise.
  bool (*Form)(const FunctionDecl *) = nullptr;
  /// A builtin row's library name, compared by the untrusted name-only
  /// fallback (matchKnownCallee) when the builtin ID is absent -- empty for
  /// the __builtin_* spellings, which always keep their IDs -- or a
  /// name-keyed row's name. A per-row literal rather than
  /// Builtin::Context::getName, which returns std::string by value.
  StringRef Name;
  /// A name-keyed row's parameter count, or AnyArity.
  unsigned Arity = AnyArity;
  KnownCalleeReturn Return = KnownCalleeReturn::ReturnsTrusted;
  /// The role parameter's index.
  unsigned Param = 0;
  /// The shape a name-keyed row's role parameter must have, so a
  /// specialization whose iterator is of another shape falls outside the row.
  ParamShape Shape = ParamShape::Pointer;
  /// The role parameter may also be a handle (ParamShape::Handle): a
  /// class-type iterator or range.
  bool AcceptsHandle = false;
  /// What the callee does to the storage bound to Param.
  KnownParamRole Role = KnownParamRole::None;
  /// A Construct role that applies only where default-initialization of
  /// Param's value type leaves no scalar indeterminate
  /// (defaultInitLeavesScalarIndeterminate); AssertUninit elsewhere.
  bool ConstructIfDefaultInitDeterminate = false;
  /// The index of the end iterator of the range Param begins, or NoEndParam
  /// (P4222R2 §4.4 annotates both iterators).
  unsigned EndParam = NoEndParam;
  /// A Derive row whose result is past its argument's start (std::end).
  bool DeriveOffsets = false;
};
} // namespace

/// A row keyed by builtin ID, with \p FallbackName for the name-only
/// fallback.
static constexpr KnownCalleeEntry
builtinRow(unsigned ID, KnownCalleeReturn Return, StringRef FallbackName = "",
           KnownParamRole Role = KnownParamRole::None, unsigned Param = 0) {
  KnownCalleeEntry E;
  E.Key = KnownCalleeKey::Builtin;
  E.BuiltinID = ID;
  E.Name = FallbackName;
  E.Return = Return;
  E.Role = Role;
  E.Param = Param;
  return E;
}

/// A row keyed by a form predicate.
static constexpr KnownCalleeEntry
formRow(bool (*Form)(const FunctionDecl *), KnownCalleeReturn Return,
        KnownParamRole Role = KnownParamRole::None, unsigned Param = 0) {
  KnownCalleeEntry E;
  E.Key = KnownCalleeKey::Form;
  E.Form = Form;
  E.Return = Return;
  E.Role = Role;
  E.Param = Param;
  return E;
}

/// A row keyed by namespace-std name and arity.
static constexpr KnownCalleeEntry
stdRow(StringRef Name, unsigned Arity, unsigned Param, KnownParamRole Role,
       unsigned EndParam = KnownCalleeEntry::NoEndParam,
       bool ConstructIfDefaultInitDeterminate = false) {
  KnownCalleeEntry E;
  E.Key = KnownCalleeKey::StdName;
  E.Name = Name;
  E.Arity = Arity;
  E.Param = Param;
  E.Role = Role;
  E.EndParam = EndParam;
  E.ConstructIfDefaultInitDeterminate = ConstructIfDefaultInitDeterminate;
  return E;
}

/// A row keyed by namespace-std name and arity whose role -- Derive (offset
/// by the call when \p Offsets) or AnyState -- takes its argument of any
/// shape.
static constexpr KnownCalleeEntry stdAnyShapeRow(StringRef Name, unsigned Arity,
                                                 KnownParamRole Role,
                                                 bool Offsets = false) {
  KnownCalleeEntry E = stdRow(Name, Arity, 0, Role);
  E.Shape = ParamShape::Any;
  E.DeriveOffsets = Offsets;
  return E;
}

/// \p E, whose role parameter may also be a handle.
static constexpr KnownCalleeEntry orHandle(KnownCalleeEntry E) {
  E.AcceptsHandle = true;
  return E;
}

/// A row keyed by namespace-std name alone, of any arity, describing only
/// what the callee returns.
static constexpr KnownCalleeEntry stdReturnRow(StringRef Name,
                                               KnownCalleeReturn Return) {
  KnownCalleeEntry E;
  E.Key = KnownCalleeKey::StdName;
  E.Name = Name;
  E.Return = Return;
  return E;
}

/// A row keyed by the name of a std::ranges customization point object and
/// the arity of its call operator, for the overload taking a pointer
/// iterator.
static constexpr KnownCalleeEntry
cpoRow(StringRef Name, unsigned Arity, unsigned Param, KnownParamRole Role,
       unsigned EndParam = KnownCalleeEntry::NoEndParam,
       bool ConstructIfDefaultInitDeterminate = false) {
  KnownCalleeEntry E = stdRow(Name, Arity, Param, Role, EndParam,
                              ConstructIfDefaultInitDeterminate);
  E.Key = KnownCalleeKey::RangesCPO;
  return E;
}

/// A RangesCPO row whose role -- Derive (offset by the call when \p Offsets)
/// or AnyState -- takes its argument of any shape.
static constexpr KnownCalleeEntry cpoAnyShapeRow(StringRef Name, unsigned Arity,
                                                 KnownParamRole Role,
                                                 bool Offsets = false) {
  KnownCalleeEntry E = cpoRow(Name, Arity, 0, Role);
  E.Shape = ParamShape::Any;
  E.DeriveOffsets = Offsets;
  return E;
}

/// A cpoRow for the overload taking a range, which matches where the range
/// is an array.
static constexpr KnownCalleeEntry
cpoRangeRow(StringRef Name, unsigned Arity, unsigned Param, KnownParamRole Role,
            bool ConstructIfDefaultInitDeterminate = false) {
  KnownCalleeEntry E =
      cpoRow(Name, Arity, Param, Role, KnownCalleeEntry::NoEndParam,
             ConstructIfDefaultInitDeterminate);
  E.Shape = ParamShape::ArrayReference;
  return E;
}

/// The known-callee table; see KnownCalleeEntry. An execution-policy
/// overload of a std function takes the policy first, shifting its role
/// parameter by one. A std::ranges overload takes an iterator and a sentinel,
/// or a range; the copy and move forms take an output sentinel.
static constexpr KnownCalleeEntry KnownCallees[] = {
    builtinRow(Builtin::BImalloc, KnownCalleeReturn::ReturnsUninitialized,
               "malloc"),
    builtinRow(Builtin::BI__builtin_malloc,
               KnownCalleeReturn::ReturnsUninitialized),
    builtinRow(Builtin::BIaligned_alloc,
               KnownCalleeReturn::ReturnsUninitialized, "aligned_alloc"),
    builtinRow(Builtin::BIalloca, KnownCalleeReturn::ReturnsUninitialized,
               "alloca"),
    builtinRow(Builtin::BI__builtin_alloca,
               KnownCalleeReturn::ReturnsUninitialized),
    builtinRow(Builtin::BI__builtin_alloca_uninitialized,
               KnownCalleeReturn::ReturnsUninitialized),
    builtinRow(Builtin::BI__builtin_alloca_with_align,
               KnownCalleeReturn::ReturnsUninitialized),
    builtinRow(Builtin::BI__builtin_alloca_with_align_uninitialized,
               KnownCalleeReturn::ReturnsUninitialized),
    builtinRow(Builtin::BI__builtin_operator_new,
               KnownCalleeReturn::ReturnsUninitialized),
    builtinRow(Builtin::BIcalloc, KnownCalleeReturn::ReturnsInitialized,
               "calloc"),
    builtinRow(Builtin::BI__builtin_calloc,
               KnownCalleeReturn::ReturnsInitialized),
    builtinRow(Builtin::BIrealloc, KnownCalleeReturn::ReturnsUnknown, "realloc",
               KnownParamRole::Release),
    builtinRow(Builtin::BI__builtin_realloc, KnownCalleeReturn::ReturnsUnknown,
               "", KnownParamRole::Release),
    builtinRow(Builtin::BIfree, KnownCalleeReturn::ReturnsTrusted, "free",
               KnownParamRole::Release),
    builtinRow(Builtin::BI__builtin_free, KnownCalleeReturn::ReturnsTrusted, "",
               KnownParamRole::Release),
    formRow(isReplaceableGlobalOperatorNew,
            KnownCalleeReturn::ReturnsUninitialized),
    formRow(isReplaceableGlobalOperatorDelete,
            KnownCalleeReturn::ReturnsTrusted, KnownParamRole::Release),
    builtinRow(Builtin::BImemset, KnownCalleeReturn::ReturnsInitialized,
               "memset", KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_memset,
               KnownCalleeReturn::ReturnsInitialized, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BImemcpy, KnownCalleeReturn::ReturnsInitialized,
               "memcpy", KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_memcpy,
               KnownCalleeReturn::ReturnsInitialized, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BImemmove, KnownCalleeReturn::ReturnsInitialized,
               "memmove", KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_memmove,
               KnownCalleeReturn::ReturnsInitialized, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin___memset_chk,
               KnownCalleeReturn::ReturnsInitialized, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin___memcpy_chk,
               KnownCalleeReturn::ReturnsInitialized, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin___memmove_chk,
               KnownCalleeReturn::ReturnsInitialized, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_va_start, KnownCalleeReturn::ReturnsTrusted,
               "", KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_c23_va_start,
               KnownCalleeReturn::ReturnsTrusted, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_stdarg_start,
               KnownCalleeReturn::ReturnsTrusted, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__va_start, KnownCalleeReturn::ReturnsTrusted, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_ms_va_start,
               KnownCalleeReturn::ReturnsTrusted, "",
               KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_va_copy, KnownCalleeReturn::ReturnsTrusted,
               "", KnownParamRole::InitializeAnyState),
    builtinRow(Builtin::BI__builtin_ms_va_copy,
               KnownCalleeReturn::ReturnsTrusted, "",
               KnownParamRole::InitializeAnyState),
    formRow(isStdAllocate, KnownCalleeReturn::ReturnsUninitialized),
    formRow(isStdAllocatorDeallocate, KnownCalleeReturn::ReturnsTrusted,
            KnownParamRole::Release),
    formRow(isStdAllocatorTraitsDeallocate, KnownCalleeReturn::ReturnsTrusted,
            KnownParamRole::Release, 1),
    formRow(isStdAllocatorTraitsConstruct, KnownCalleeReturn::ReturnsTrusted,
            KnownParamRole::Construct, 1),
    formRow(isStdAllocatorTraitsDestroy, KnownCalleeReturn::ReturnsTrusted,
            KnownParamRole::Destroy, 1),
    stdRow("construct_at", KnownCalleeEntry::AnyArity, 0,
           KnownParamRole::Construct),
    stdRow("destroy_at", KnownCalleeEntry::AnyArity, 0,
           KnownParamRole::Destroy),
    stdRow("now_init", 1, 0, KnownParamRole::AssertUninit),
    orHandle(stdRow("uninitialized_copy", 3, 2, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_copy", 4, 3, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_copy_n", 3, 2, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_copy_n", 4, 3, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_move", 3, 2, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_move", 4, 3, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_move_n", 3, 2, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_move_n", 4, 3, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_fill", 3, 0, KnownParamRole::Construct, 1)),
    orHandle(stdRow("uninitialized_fill", 4, 1, KnownParamRole::Construct, 2)),
    orHandle(stdRow("uninitialized_fill_n", 3, 0, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_fill_n", 4, 1, KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_value_construct", 2, 0,
                    KnownParamRole::Construct, 1)),
    orHandle(stdRow("uninitialized_value_construct", 3, 1,
                    KnownParamRole::Construct, 2)),
    orHandle(stdRow("uninitialized_value_construct_n", 2, 0,
                    KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_value_construct_n", 3, 1,
                    KnownParamRole::Construct)),
    orHandle(stdRow("uninitialized_default_construct", 2, 0,
                    KnownParamRole::Construct, 1,
                    /*ConstructIfDefaultInitDeterminate=*/true)),
    orHandle(stdRow("uninitialized_default_construct", 3, 1,
                    KnownParamRole::Construct, 2,
                    /*ConstructIfDefaultInitDeterminate=*/true)),
    orHandle(stdRow("uninitialized_default_construct_n", 2, 0,
                    KnownParamRole::Construct, KnownCalleeEntry::NoEndParam,
                    /*ConstructIfDefaultInitDeterminate=*/true)),
    orHandle(stdRow("uninitialized_default_construct_n", 3, 1,
                    KnownParamRole::Construct, KnownCalleeEntry::NoEndParam,
                    /*ConstructIfDefaultInitDeterminate=*/true)),
    orHandle(stdRow("destroy", 2, 0, KnownParamRole::Destroy, 1)),
    orHandle(stdRow("destroy", 3, 1, KnownParamRole::Destroy, 2)),
    orHandle(stdRow("destroy_n", 2, 0, KnownParamRole::Destroy)),
    orHandle(stdRow("destroy_n", 3, 1, KnownParamRole::Destroy)),
    stdAnyShapeRow("begin", 1, KnownParamRole::Derive),
    stdAnyShapeRow("cbegin", 1, KnownParamRole::Derive),
    stdAnyShapeRow("data", 1, KnownParamRole::Derive),
    stdAnyShapeRow("to_address", 1, KnownParamRole::Derive),
    stdAnyShapeRow("end", 1, KnownParamRole::Derive, /*Offsets=*/true),
    stdAnyShapeRow("cend", 1, KnownParamRole::Derive, /*Offsets=*/true),
    stdAnyShapeRow("next", 2, KnownParamRole::Derive, /*Offsets=*/true),
    stdAnyShapeRow("prev", 2, KnownParamRole::Derive, /*Offsets=*/true),
    stdAnyShapeRow("rbegin", 1, KnownParamRole::Derive),
    stdAnyShapeRow("crbegin", 1, KnownParamRole::Derive),
    stdAnyShapeRow("rend", 1, KnownParamRole::Derive, /*Offsets=*/true),
    stdAnyShapeRow("crend", 1, KnownParamRole::Derive, /*Offsets=*/true),
    stdAnyShapeRow("size", 1, KnownParamRole::AnyState),
    stdAnyShapeRow("ssize", 1, KnownParamRole::AnyState),
    stdAnyShapeRow("empty", 1, KnownParamRole::AnyState),
    stdAnyShapeRow("distance", 2, KnownParamRole::AnyState),
    stdReturnRow("make_unique_for_overwrite",
                 KnownCalleeReturn::ReturnsDefaultInitialized),
    stdReturnRow("make_shared_for_overwrite",
                 KnownCalleeReturn::ReturnsDefaultInitialized),
    stdReturnRow("allocate_shared_for_overwrite",
                 KnownCalleeReturn::ReturnsDefaultInitialized),
    cpoAnyShapeRow("begin", 1, KnownParamRole::Derive),
    cpoAnyShapeRow("cbegin", 1, KnownParamRole::Derive),
    cpoAnyShapeRow("data", 1, KnownParamRole::Derive),
    cpoAnyShapeRow("cdata", 1, KnownParamRole::Derive),
    cpoAnyShapeRow("rbegin", 1, KnownParamRole::Derive),
    cpoAnyShapeRow("crbegin", 1, KnownParamRole::Derive),
    cpoAnyShapeRow("end", 1, KnownParamRole::Derive, /*Offsets=*/true),
    cpoAnyShapeRow("cend", 1, KnownParamRole::Derive, /*Offsets=*/true),
    cpoAnyShapeRow("rend", 1, KnownParamRole::Derive, /*Offsets=*/true),
    cpoAnyShapeRow("crend", 1, KnownParamRole::Derive, /*Offsets=*/true),
    cpoAnyShapeRow("size", 1, KnownParamRole::AnyState),
    cpoAnyShapeRow("ssize", 1, KnownParamRole::AnyState),
    cpoAnyShapeRow("empty", 1, KnownParamRole::AnyState),
    cpoAnyShapeRow("distance", 1, KnownParamRole::AnyState),
    cpoAnyShapeRow("distance", 2, KnownParamRole::AnyState),
    cpoRow("construct_at", KnownCalleeEntry::AnyArity, 0,
           KnownParamRole::Construct),
    cpoRow("destroy_at", 1, 0, KnownParamRole::Destroy),
    orHandle(cpoRow("destroy", 2, 0, KnownParamRole::Destroy, 1)),
    orHandle(cpoRangeRow("destroy", 1, 0, KnownParamRole::Destroy)),
    orHandle(cpoRow("destroy_n", 2, 0, KnownParamRole::Destroy)),
    orHandle(cpoRow("uninitialized_fill", 3, 0, KnownParamRole::Construct, 1)),
    orHandle(
        cpoRangeRow("uninitialized_fill", 2, 0, KnownParamRole::Construct)),
    orHandle(cpoRow("uninitialized_fill_n", 3, 0, KnownParamRole::Construct)),
    orHandle(cpoRow("uninitialized_copy", 4, 2, KnownParamRole::Construct, 3)),
    orHandle(
        cpoRangeRow("uninitialized_copy", 2, 1, KnownParamRole::Construct)),
    orHandle(
        cpoRow("uninitialized_copy_n", 4, 2, KnownParamRole::Construct, 3)),
    orHandle(cpoRow("uninitialized_move", 4, 2, KnownParamRole::Construct, 3)),
    orHandle(
        cpoRangeRow("uninitialized_move", 2, 1, KnownParamRole::Construct)),
    orHandle(
        cpoRow("uninitialized_move_n", 4, 2, KnownParamRole::Construct, 3)),
    orHandle(cpoRow("uninitialized_value_construct", 2, 0,
                    KnownParamRole::Construct, 1)),
    orHandle(cpoRangeRow("uninitialized_value_construct", 1, 0,
                         KnownParamRole::Construct)),
    orHandle(cpoRow("uninitialized_value_construct_n", 2, 0,
                    KnownParamRole::Construct)),
    orHandle(cpoRow("uninitialized_default_construct", 2, 0,
                    KnownParamRole::Construct, 1,
                    /*ConstructIfDefaultInitDeterminate=*/true)),
    orHandle(cpoRangeRow("uninitialized_default_construct", 1, 0,
                         KnownParamRole::Construct,
                         /*ConstructIfDefaultInitDeterminate=*/true)),
    orHandle(cpoRow("uninitialized_default_construct_n", 2, 0,
                    KnownParamRole::Construct, KnownCalleeEntry::NoEndParam,
                    /*ConstructIfDefaultInitDeterminate=*/true)),
};

/// True if a parameter of type \p T has the shape \p Shape.
static bool hasParamShape(QualType T, ParamShape Shape) {
  switch (Shape) {
  case ParamShape::Pointer:
    return T->isPointerType();
  case ParamShape::ArrayReference:
    return T->isReferenceType() && T.getNonReferenceType()->isArrayType();
  case ParamShape::Handle: {
    const auto *RD = T.getNonReferenceType()->getAsCXXRecordDecl();
    return RD && !RD->isUnion();
  }
  case ParamShape::Any:
    return true;
  }
  llvm_unreachable("unknown ParamShape");
}

/// True if the name-keyed row \p Entry matches \p FD, a callee whose name
/// matched: its arity, and the shape of its role parameter if it has one.
static bool matchesNameKeyedRow(const KnownCalleeEntry &Entry,
                                const FunctionDecl *FD) {
  unsigned NumParams = FD->getNumParams();
  if (Entry.Arity != KnownCalleeEntry::AnyArity && Entry.Arity != NumParams)
    return false;
  if (Entry.Role == KnownParamRole::None)
    return true;
  if (Entry.Param >= NumParams)
    return false;
  QualType T = FD->getParamDecl(Entry.Param)->getType();
  return hasParamShape(T, Entry.Shape) ||
         (Entry.AcceptsHandle && hasParamShape(T, ParamShape::Handle));
}

/// The namespace named \p Name directly inside std that encloses \p D, or
/// null; std may be reached through an inline namespace (libc++'s __1).
static const NamespaceDecl *enclosingStdSubNamespace(const Decl *D,
                                                     StringRef Name) {
  for (const DeclContext *DC = D->getDeclContext();
       DC && !DC->isTranslationUnit(); DC = DC->getParent())
    if (const auto *NS = dyn_cast<NamespaceDecl>(DC);
        NS && NS->getIdentifier() && NS->getName() == Name &&
        NS->getParent()->isStdNamespace())
      return NS;
  return nullptr;
}

/// The RangesCPO row for \p FD, if any: \p FD is the call operator, static
/// or with an implicit object parameter, of a class that std::ranges
/// encloses, and the member of std::ranges named by the row -- an inline
/// namespace's member, such as libc++'s __cpo, or a using-declaration's
/// target included -- is a variable of that class.
static const KnownCalleeEntry *findRangesCPORow(const FunctionDecl *FD) {
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (!MD || MD->getOverloadedOperator() != OO_Call ||
      MD->isExplicitObjectMemberFunction())
    return nullptr;
  const CXXRecordDecl *RD = MD->getParent()->getCanonicalDecl();
  const NamespaceDecl *Ranges = enclosingStdSubNamespace(RD, "ranges");
  if (!Ranges)
    return nullptr;
  IdentifierTable &Idents = FD->getASTContext().Idents;
  for (const KnownCalleeEntry &Entry : KnownCallees) {
    if (Entry.Key != KnownCalleeKey::RangesCPO ||
        !matchesNameKeyedRow(Entry, FD))
      continue;
    for (const NamedDecl *ND : Ranges->lookup(&Idents.get(Entry.Name)))
      if (const auto *VD = dyn_cast<VarDecl>(ND->getUnderlyingDecl()))
        if (const CXXRecordDecl *VarClass = VD->getType()->getAsCXXRecordDecl();
            VarClass && VarClass->getCanonicalDecl() == RD)
          return &Entry;
  }
  return nullptr;
}

/// The table row for \p FD, if any -- the *trusted* match: the builtin ID,
/// the form, the namespace-std name and arity, or the std::ranges object's
/// name and arity guarantee the row's semantics. Builtin rows are consulted
/// first; no collision is possible -- plain operator new/delete never carry
/// the table's builtin IDs, __builtin_operator_new is not an operator name,
/// and no std row names a library builtin -- so the order is cosmetic.
static const KnownCalleeEntry *findKnownCallee(const FunctionDecl *FD) {
  if (unsigned ID = FD->getBuiltinID())
    for (const KnownCalleeEntry &Entry : KnownCallees)
      if (Entry.Key == KnownCalleeKey::Builtin && Entry.BuiltinID == ID)
        return &Entry;
  for (const KnownCalleeEntry &Entry : KnownCallees)
    if (Entry.Key == KnownCalleeKey::Form && Entry.Form(FD))
      return &Entry;
  if (const KnownCalleeEntry *Entry = findRangesCPORow(FD))
    return Entry;
  const IdentifierInfo *II = FD->getIdentifier();
  if (!II || !FD->isInStdNamespace())
    return nullptr;
  for (const KnownCalleeEntry &Entry : KnownCallees)
    if (Entry.Key == KnownCalleeKey::StdName && Entry.Name == II->getName() &&
        matchesNameKeyedRow(Entry, FD))
      return &Entry;
  return nullptr;
}

/// The table row for \p FD plus whether the match is trusted. A trusted
/// match is findKnownCallee's. The fallback recognizes a plain-named
/// declaration whose builtin ID is absent -- -fno-builtin / -ffreestanding,
/// or a non-matching signature -- as an *untrusted* match: the name says
/// what the function is meant to be, but its semantics cannot be assumed.
/// The gate mirrors clang's builtin-attachment condition (a file-scope
/// declaration with C language linkage, which a static member and ns::malloc
/// fail although isGlobal() holds for them), and a callee that still
/// carries any builtin ID is never re-matched by name (the __builtin_*
/// spellings keep their IDs everywhere).
struct KnownCalleeMatch {
  const KnownCalleeEntry *Entry = nullptr;
  bool Trusted = false;
};

static KnownCalleeMatch matchKnownCallee(const FunctionDecl *FD) {
  if (const KnownCalleeEntry *Entry = findKnownCallee(FD))
    return {Entry, /*Trusted=*/true};
  if (FD->getBuiltinID() != 0)
    return {};
  const IdentifierInfo *II = FD->getDeclName().getAsIdentifierInfo();
  if (!II || !FD->getDeclContext()->getRedeclContext()->isFileContext() ||
      FD->getLanguageLinkage() != CLanguageLinkage)
    return {};
  StringRef Name = II->getName();
  for (const KnownCalleeEntry &Entry : KnownCallees)
    if (Entry.Key == KnownCalleeKey::Builtin && !Entry.Name.empty() &&
        Entry.Name == Name)
      return {&Entry, /*Trusted=*/false};
  return {};
}

namespace {
/// What a class of the std handle table (getStdHandleKind) is.
enum class StdHandleKind {
  None,
  /// A view or iterator: std::span, std::ranges::subrange, the iterator
  /// adaptors, and the library-internal pointer wrappers.
  View,
  /// An owning smart pointer: std::unique_ptr, std::shared_ptr.
  Owner
};
} // namespace

/// The std handle table: the standard classes through whose construction raw
/// storage enters a handle (P4222R2 §4.7, §9.3), by name and namespace --
/// libc++'s std::__wrap_iter, libstdc++'s __gnu_cxx::__normal_iterator, and
/// libstdc++'s std::__shared_ptr, the base that declares std::shared_ptr's
/// reset, included.
static StdHandleKind getStdHandleKind(const CXXRecordDecl *RD) {
  const IdentifierInfo *II = RD ? RD->getIdentifier() : nullptr;
  if (!II)
    return StdHandleKind::None;
  StringRef Name = II->getName();
  if (RD->isInStdNamespace()) {
    if (Name == "unique_ptr" || Name == "shared_ptr" || Name == "__shared_ptr")
      return StdHandleKind::Owner;
    if (Name == "span" || Name == "reverse_iterator" ||
        Name == "move_iterator" || Name == "basic_const_iterator" ||
        Name == "__wrap_iter")
      return StdHandleKind::View;
    return StdHandleKind::None;
  }
  if (Name == "subrange")
    return enclosingStdSubNamespace(RD, "ranges") ? StdHandleKind::View
                                                  : StdHandleKind::None;
  if (Name == "__normal_iterator") {
    const auto *NS = dyn_cast<NamespaceDecl>(RD->getDeclContext());
    return NS && NS->getIdentifier() && NS->getName() == "__gnu_cxx" &&
                   NS->getParent()->isTranslationUnit()
               ? StdHandleKind::View
               : StdHandleKind::None;
  }
  return StdHandleKind::None;
}

/// How a transfer parameter of type \p T is classified: a pointer, or a
/// read-only alias of one, by its value; a reference to a class, or a class,
/// as a range; and any other reference as storage.
static SemaProfiles::HandleLeaf::Mode handleTransferModeOf(QualType T) {
  using Mode = SemaProfiles::HandleLeaf::Mode;
  if (T->isPointerType() || SemaProfiles::pointerAliasKind(T) ==
                                SemaProfiles::PointerAliasKind::ReadOnly)
    return Mode::PointerValue;
  if (T.getNonReferenceType()->getAsCXXRecordDecl())
    return Mode::Range;
  return Mode::Storage;
}

/// True if \p MD is the reset of a std-table owner.
static bool isStdOwnerReset(const CXXMethodDecl *MD) {
  return MD->getIdentifier() && MD->getName() == "reset" &&
         getStdHandleKind(MD->getParent()) == StdHandleKind::Owner;
}

/// The mode of \p P when it is a *transfer parameter* -- one through which
/// raw storage enters a std-table handle under construction, or an owner by
/// its reset (its pointer parameter) -- or none. Of
/// a view's constructors other than copy and move, every parameter not of
/// integral, enumeration, floating, or std::nullptr_t type (after its
/// reference and qualifiers). Of an owner's, the first object pointer taken
/// by value -- never a deleter or an allocator; only the pointer of the
/// aliasing constructor -- or, with none, each parameter whose class is an
/// owner (a converting constructor).
static std::optional<SemaProfiles::HandleLeaf::Mode>
handleTransferMode(const ParmVarDecl *P) {
  QualType T = P->getType();
  // An owner's reset takes its first parameter, a pointer.
  if (const auto *MD = dyn_cast<CXXMethodDecl>(P->getDeclContext());
      MD && isStdOwnerReset(MD))
    return P->getFunctionScopeIndex() == 0 && T->isPointerType() &&
                   !T->isFunctionPointerType()
               ? std::optional(SemaProfiles::HandleLeaf::Mode::PointerValue)
               : std::nullopt;
  const auto *Ctor = dyn_cast<CXXConstructorDecl>(P->getDeclContext());
  if (!Ctor || Ctor->isCopyOrMoveConstructor())
    return std::nullopt;
  QualType Bare = T.getNonReferenceType().getUnqualifiedType();
  switch (getStdHandleKind(Ctor->getParent())) {
  case StdHandleKind::None:
    return std::nullopt;
  case StdHandleKind::View:
    if (Bare->isIntegralOrEnumerationType() || Bare->isFloatingType() ||
        Bare->isNullPtrType())
      return std::nullopt;
    return handleTransferModeOf(T);
  case StdHandleKind::Owner: {
    auto IsObjectPointer = [](const ParmVarDecl *Q) {
      QualType QT = Q->getType();
      return QT->isPointerType() && !QT->isFunctionPointerType();
    };
    const auto *It = llvm::find_if(Ctor->parameters(), IsObjectPointer);
    if (It != Ctor->param_end())
      return *It == P ? std::optional(handleTransferModeOf(T)) : std::nullopt;
    if (getStdHandleKind(Bare->getAsCXXRecordDecl()) == StdHandleKind::Owner)
      return handleTransferModeOf(T);
    return std::nullopt;
  }
  }
  llvm_unreachable("unknown StdHandleKind");
}

/// True if \p Ctor constructs a std-table handle from raw storage: a
/// constructor of a table class other than its copy and move constructors.
static bool isStdHandleConstruction(const CXXConstructorDecl *Ctor) {
  return !Ctor->isCopyOrMoveConstructor() &&
         getStdHandleKind(Ctor->getParent()) != StdHandleKind::None;
}

bool SemaProfiles::isStdOwnerType(QualType T) {
  return getStdHandleKind(T->getAsCXXRecordDecl()) == StdHandleKind::Owner;
}

bool SemaProfiles::isUniqueOwnerType(QualType T) {
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  return RD && RD->getIdentifier() && RD->getName() == "unique_ptr" &&
         getStdHandleKind(RD) == StdHandleKind::Owner;
}

/// The declarations named \p Name in the class \p RD, or else in its nearest
/// bases that declare it, breadth first.
static SmallVector<const NamedDecl *, 2>
lookupInClassOrBases(const CXXRecordDecl *RD, DeclarationName Name) {
  SmallVector<const CXXRecordDecl *, 4> Level{RD};
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Seen{RD->getCanonicalDecl()};
  while (!Level.empty()) {
    SmallVector<const NamedDecl *, 2> Found;
    SmallVector<const CXXRecordDecl *, 4> Next;
    for (const CXXRecordDecl *C : Level) {
      llvm::append_range(Found, C->lookup(Name));
      for (const CXXBaseSpecifier &B : C->bases())
        if (const CXXRecordDecl *BD = B.getType()->getAsCXXRecordDecl();
            BD && BD->hasDefinition() &&
            Seen.insert(BD->getCanonicalDecl()).second)
          Next.push_back(BD->getDefinition());
    }
    if (!Found.empty())
      return Found;
    Level = std::move(Next);
  }
  return {};
}

/// The non-static member of \p RD or its bases named \p Name taking no
/// arguments whose return type is not dependent, a member template's
/// included; null when there is none.
static const CXXMethodDecl *nullaryMember(const CXXRecordDecl *RD,
                                          DeclarationName Name) {
  for (const NamedDecl *ND : lookupInClassOrBases(RD, Name)) {
    ND = ND->getUnderlyingDecl();
    if (const auto *FTD = dyn_cast<FunctionTemplateDecl>(ND))
      ND = FTD->getTemplatedDecl();
    if (const auto *MD = dyn_cast<CXXMethodDecl>(ND);
        MD && !MD->isStatic() && MD->getMinRequiredArguments() == 0 &&
        !MD->getReturnType()->isDependentType())
      return MD;
  }
  return nullptr;
}

/// handleValueType for the class \p RD; through its begin() only when
/// \p ThroughBegin.
static QualType classValueType(const CXXRecordDecl *RD, bool ThroughBegin) {
  if (!RD || !RD->hasDefinition())
    return QualType();
  RD = RD->getDefinition();
  ASTContext &Ctx = RD->getASTContext();
  for (StringRef Name : {"element_type", "value_type"})
    for (const NamedDecl *ND : lookupInClassOrBases(RD, &Ctx.Idents.get(Name)))
      if (const auto *TD = dyn_cast<TypeDecl>(ND))
        return Ctx.getTypeDeclType(TD).getUnqualifiedType();
  if (const CXXMethodDecl *MD =
          nullaryMember(RD, Ctx.DeclarationNames.getCXXOperatorName(OO_Star)))
    return MD->getReturnType().getNonReferenceType().getUnqualifiedType();
  // A range's elements are its iterator's.
  const CXXMethodDecl *Begin =
      ThroughBegin ? nullaryMember(RD, &Ctx.Idents.get("begin")) : nullptr;
  if (!Begin)
    return QualType();
  QualType It = Begin->getReturnType().getNonReferenceType();
  if (It->isPointerType())
    return It->getPointeeType().getUnqualifiedType();
  return classValueType(It->getAsCXXRecordDecl(), /*ThroughBegin=*/false);
}

QualType SemaProfiles::handleValueType(QualType T) {
  return classValueType(T.getNonReferenceType()->getAsCXXRecordDecl(),
                        /*ThroughBegin=*/true);
}

QualType SemaProfiles::rangeValueType(QualType T) {
  if (T->isPointerType() ||
      (T->isReferenceType() && T.getNonReferenceType()->isArrayType()))
    return T->getPointeeType();
  return handleValueType(T);
}

bool SemaProfiles::bindsNoArgument(const FunctionDecl *FD) {
  if (unsigned ID = FD->getBuiltinID(); ID && isShapeTransparentBuiltin(ID))
    return true;
  const KnownCalleeEntry *Entry = findKnownCallee(FD);
  return Entry && (Entry->Role == KnownParamRole::Derive ||
                   Entry->Role == KnownParamRole::AnyState);
}

const Expr *SemaProfiles::derivedArgument(const CallExpr *CE, bool &Partial) {
  const FunctionDecl *FD = CE->getDirectCallee();
  const KnownCalleeEntry *Entry = FD ? findKnownCallee(FD) : nullptr;
  if (!Entry || Entry->Role != KnownParamRole::Derive)
    return nullptr;
  // A std::ranges object's call takes the object as its first argument.
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  unsigned Arg = isa<CXXOperatorCallExpr>(CE) && MD &&
                         !MD->isExplicitObjectMemberFunction()
                     ? 1
                     : 0;
  if (CE->getNumArgs() <= Arg)
    return nullptr;
  Partial = Entry->DeriveOffsets;
  return CE->getArg(Arg);
}

// Releasing storage leaves it as uninitialized as ending the object's
// lifetime does, so the binding sites treat a release role like a destroy --
// except for the double-destroy check: destroy_at(p); free(p); is correct,
// because ending an object's lifetime and releasing its storage are
// different operations. The trusted/by-name split preserves the
// lenient/strict query pair: acceptance may read a name-only row (a declared
// free(p) should not reject its argument just because -fno-builtin stripped
// the ID), while the CFG pass's Kill and Write key on a trusted row only --
// killing on an untrusted name-only free could manufacture read-through
// false positives, while a missed kill is the documented missed-diagnostic
// direction.
SemaProfiles::CalleeLifecycleRoles
SemaProfiles::getCalleeLifecycleRoles(const FunctionDecl *FD) {
  SemaProfiles::CalleeLifecycleRoles Roles;
  Roles.InitializesRefToUninitParams = FD->hasAttr<NowInitAttr>();
  Roles.DestroysParams = FD->hasAttr<NowUninitAttr>();
  if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(FD))
    Roles.HandleTransfers = isStdHandleConstruction(Ctor);
  else if (const auto *MD = dyn_cast<CXXMethodDecl>(FD))
    Roles.HandleTransfers = isStdOwnerReset(MD);
  KnownCalleeMatch M = matchKnownCallee(FD);
  if (!M.Entry || M.Entry->Role == KnownParamRole::None ||
      M.Entry->Param >= FD->getNumParams())
    return Roles;
  // An allocator's own destroy may do anything.
  if (M.Entry->Form == isStdAllocatorTraitsDestroy &&
      allocatorHasOwnMember(FD, "destroy")) {
    Roles.OpaqueParam = M.Entry->Param;
    return Roles;
  }
  Roles.KnownParam = M.Entry->Param;
  Roles.KnownRole = M.Entry->Role;
  Roles.KnownTrusted = M.Trusted;
  if (M.Entry->EndParam < FD->getNumParams())
    Roles.EndParam = M.Entry->EndParam;
  // An allocator's own construct with no constructor arguments may
  // default-initialize, as uninitialized_default_construct does.
  if (M.Entry->ConstructIfDefaultInitDeterminate ||
      (M.Entry->Form == isStdAllocatorTraitsConstruct &&
       FD->getNumParams() == 2 && allocatorHasOwnMember(FD, "construct"))) {
    // The value type of the instantiated range parameter; an array stands for
    // its elements.
    QualType T = rangeValueType(FD->getParamDecl(M.Entry->Param)->getType());
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
    if (T.isNull() || T->isIncompleteType() ||
        defaultInitLeavesScalarIndeterminateImpl(
            FD->getASTContext(), T, /*HonorUninitMarkers=*/true, Visited))
      Roles.KnownRole = KnownParamRole::AssertUninit;
    else
      Roles.DefaultInitCredit = true;
  }
  return Roles;
}

SemaProfiles::KnownParamRole
SemaProfiles::CalleeLifecycleRoles::roleOf(const ParmVarDecl *P) const {
  return KnownParam == P->getFunctionScopeIndex() ? KnownRole
                                                  : KnownParamRole::None;
}

bool SemaProfiles::CalleeLifecycleRoles::isMarkedParam(
    const ParmVarDecl *P) const {
  KnownParamRole R = roleOf(P);
  return P->hasAttr<RefToUninitAttr>() || R == KnownParamRole::AssertUninit ||
         R == KnownParamRole::Construct;
}

bool SemaProfiles::CalleeLifecycleRoles::isInitializingParam(
    const ParmVarDecl *P) const {
  return roleOf(P) == KnownParamRole::InitializeAnyState;
}

bool SemaProfiles::CalleeLifecycleRoles::isEndParam(
    const ParmVarDecl *P) const {
  return EndParam == P->getFunctionScopeIndex();
}

bool SemaProfiles::CalleeLifecycleRoles::destroysParam(
    const ParmVarDecl *P) const {
  if (roleOf(P) == KnownParamRole::Destroy)
    return true;
  QualType T = P->getType();
  return DestroysParams &&
         (T->isPointerType() || T->isReferenceType() || isHandleType(T));
}

bool SemaProfiles::CalleeLifecycleRoles::releasesParam(const ParmVarDecl *P,
                                                       bool TrustedOnly) const {
  return roleOf(P) == KnownParamRole::Release && (KnownTrusted || !TrustedOnly);
}

bool SemaProfiles::CalleeLifecycleRoles::initializesParam(
    const ParmVarDecl *P) const {
  if (InitializesRefToUninitParams && isMarkedParam(P))
    return true;
  KnownParamRole R = roleOf(P);
  return KnownTrusted && (R == KnownParamRole::Construct ||
                          R == KnownParamRole::InitializeAnyState);
}

bool SemaProfiles::CalleeLifecycleRoles::transfersParam(
    const ParmVarDecl *P) const {
  return HandleTransfers && handleTransferMode(P);
}

/// The object a call to the non-static member function \p MD binds to its
/// object parameter in \p CE -- a dot call's object, a member operator's
/// first argument, or an explicit-object method's -- or null; an arrow call
/// reaches its object through a pointer and has none, and a call operator's
/// object is the callable (a std::ranges object), which none of its results
/// refers into.
static const Expr *memberCallObject(const CallExpr *CE,
                                    const CXXMethodDecl *MD) {
  if (MD->isStatic() || MD->getOverloadedOperator() == OO_Call)
    return nullptr;
  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE)) {
    const auto *ME = dyn_cast<MemberExpr>(MCE->getCallee()->IgnoreParens());
    return ME && !ME->isArrow() ? MCE->getImplicitObjectArgument() : nullptr;
  }
  if ((isa<CXXOperatorCallExpr>(CE) || MD->isExplicitObjectMemberFunction()) &&
      CE->getNumArgs() > 0)
    return CE->getArg(0);
  return nullptr;
}

/// True if \p T is \p RD's class or one of its bases.
static bool isClassOrBaseOf(QualType T, const CXXRecordDecl *RD) {
  const CXXRecordDecl *TD = T->getAsCXXRecordDecl();
  if (!TD || !RD->hasDefinition())
    return false;
  TD = TD->getCanonicalDecl();
  return TD == RD->getCanonicalDecl() || RD->isDerivedFrom(TD);
}

/// The object of the member call \p CE when the call derives a pointer or
/// reference from it: an unmarked dot call, member operator, or
/// explicit-object method returning a pointer to an object, or a reference
/// to an object of a type other than the method's own class and its bases
/// (P4222R2 §4.6: what a member of an uninitialized handle returns refers to
/// uninitialized memory); null otherwise.
static const Expr *derivingObject(const CallExpr *CE) {
  const auto *MD = dyn_cast_or_null<CXXMethodDecl>(CE->getDirectCallee());
  if (!MD || MD->hasAttr<RefToUninitAttr>())
    return nullptr;
  QualType R = MD->getReturnType();
  if (R->isPointerType()) {
    if (R->isFunctionPointerType())
      return nullptr;
  } else if (R->isReferenceType()) {
    QualType Referent = R.getNonReferenceType();
    if (Referent->isFunctionType() ||
        isClassOrBaseOf(Referent, MD->getParent()))
      return nullptr;
  } else {
    return nullptr;
  }
  return memberCallObject(CE, MD);
}

/// True if the class \p R a member of \p RD returns by value is a handle to
/// what an object of \p RD refers to: \p RD itself, another specialization of
/// its class template, a std-table class of \p RD's kind (getStdHandleKind),
/// or a class of its value type (SemaProfiles::handleValueType) --
/// s.subspan(1), s.first<2>(), s.begin() -- and not any other class a
/// member returns (a name, a duration).
static bool isHandleResultOf(QualType R, const CXXRecordDecl *RD) {
  const CXXRecordDecl *RRD = R->getAsCXXRecordDecl();
  if (!RRD)
    return false;
  if (RRD->getCanonicalDecl() == RD->getCanonicalDecl())
    return true;
  auto TemplateOf = [](const CXXRecordDecl *D) -> const ClassTemplateDecl * {
    const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(D);
    return CTSD ? CTSD->getSpecializedTemplate()->getCanonicalDecl() : nullptr;
  };
  if (const ClassTemplateDecl *T = TemplateOf(RRD); T && T == TemplateOf(RD))
    return true;
  if (StdHandleKind K = getStdHandleKind(RD);
      K != StdHandleKind::None && K == getStdHandleKind(RRD))
    return true;
  ASTContext &Ctx = RD->getASTContext();
  QualType RV = SemaProfiles::handleValueType(R);
  QualType OV = SemaProfiles::handleValueType(Ctx.getCanonicalTagType(RD));
  return !RV.isNull() && !OV.isNull() && Ctx.hasSameUnqualifiedType(RV, OV);
}

/// The object of the call \p CE when the call returns the handle itself, from
/// an unmarked function no known-callee row describes: a member returning a
/// handle to what its object refers to by value (isHandleResultOf), or a
/// reference to the method's own class or a base (s.begin(),
/// s.subspan(1), ++it); or the operand of a non-member + or - returning
/// that operand's class (n + it). Null otherwise. \p Partial is set when
/// the call refers to part of the handle: it has explicit arguments or
/// explicit template arguments (s.last<2>()), is ++, --, +=, -=, or a
/// non-member + or -, or is a member named for a position past the start
/// (s.end()) or a part (\c subspan, \c first, \c last, \c next, \c prev,
/// \c advance). \p PastStart, if non-null, is set when the result is also
/// positioned past the start: every such call but a prefix (\c first, a
/// \c subspan at offset 0).
static const Expr *handleSelfReturnObject(const CallExpr *CE, bool &Partial,
                                          bool *PastStart = nullptr) {
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD || FD->hasAttr<RefToUninitAttr>() || matchKnownCallee(FD).Entry)
    return nullptr;
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (!MD) {
    OverloadedOperatorKind OO = FD->getOverloadedOperator();
    const CXXRecordDecl *RD = FD->getReturnType()->getAsCXXRecordDecl();
    if ((OO != OO_Plus && OO != OO_Minus) || !isa<CXXOperatorCallExpr>(CE) ||
        !SemaProfiles::isHandleType(FD->getReturnType()))
      return nullptr;
    for (unsigned I = 0, N = std::min(CE->getNumArgs(), FD->getNumParams());
         I != N; ++I)
      if (const CXXRecordDecl *PD = FD->getParamDecl(I)
                                        ->getType()
                                        .getNonReferenceType()
                                        ->getAsCXXRecordDecl();
          PD && PD->getCanonicalDecl() == RD->getCanonicalDecl()) {
        Partial = true;
        if (PastStart)
          *PastStart = true;
        return CE->getArg(I);
      }
    return nullptr;
  }
  QualType R = MD->getReturnType();
  if (R->isReferenceType()) {
    if (!isClassOrBaseOf(R.getNonReferenceType(), MD->getParent()))
      return nullptr;
  } else if (!SemaProfiles::isHandleType(R) ||
             !isHandleResultOf(R, MD->getParent())) {
    return nullptr;
  }
  const Expr *Obj = memberCallObject(CE, MD);
  if (!Obj)
    return nullptr;
  SmallVector<const Expr *, 2> Args;
  for (const Expr *Arg : CE->arguments())
    if (Arg != Obj && !isa<CXXDefaultArgExpr>(Arg))
      Args.push_back(Arg);
  const auto *ME = dyn_cast<MemberExpr>(CE->getCallee()->IgnoreParens());
  bool Explicit = !Args.empty() || (ME && ME->hasExplicitTemplateArgs());
  StringRef Name = MD->getIdentifier() ? MD->getName() : StringRef();
  // A prefix of the handle starts where the handle does: first(n),
  // first<N>(), subspan(0, n), subspan<0, N>().
  auto IsZero = [&](const Expr *E) {
    std::optional<llvm::APSInt> V =
        E->getIntegerConstantExpr(MD->getASTContext());
    return V && V->isZero();
  };
  bool Prefix = Name == "first";
  if (Name == "subspan") {
    if (ME && ME->getNumTemplateArgs() != 0) {
      const TemplateArgument &A = ME->getTemplateArgs()[0].getArgument();
      Prefix = (A.getKind() == TemplateArgument::Integral &&
                A.getAsIntegral().isZero()) ||
               (A.getKind() == TemplateArgument::Expression &&
                IsZero(A.getAsExpr()));
    } else {
      Prefix = !Args.empty() && IsZero(Args.front());
    }
  }
  switch (MD->getOverloadedOperator()) {
  case OO_PlusPlus:
  case OO_MinusMinus:
  case OO_PlusEqual:
  case OO_MinusEqual:
    Partial = true;
    break;
  default:
    Partial =
        Explicit ||
        llvm::is_contained(ArrayRef<StringRef>{"end", "cend", "rend", "crend",
                                               "last", "subspan", "first",
                                               "next", "prev", "advance"},
                           Name);
    break;
  }
  if (PastStart)
    *PastStart = Partial && !Prefix;
  return Obj;
}

/// Peel what forEachHandleSourceLeaf looks through from \p E: parens, full
/// expressions, temporaries, the no-op, constructor, derived-to-base, and
/// user-defined conversions, and a comma's right operand, a statement
/// expression's result, an opaque value's source, and an identity call's
/// argument.
static const Expr *peelHandleSource(const Expr *E) {
  while (true) {
    E = E->IgnoreParens();
    if (const auto *FE = dyn_cast<FullExpr>(E)) {
      E = FE->getSubExpr();
      continue;
    }
    if (const auto *BTE = dyn_cast<CXXBindTemporaryExpr>(E)) {
      E = BTE->getSubExpr();
      continue;
    }
    if (const auto *MTE = dyn_cast<MaterializeTemporaryExpr>(E)) {
      E = MTE->getSubExpr();
      continue;
    }
    if (const auto *CE = dyn_cast<CastExpr>(E)) {
      switch (CE->getCastKind()) {
      case CK_NoOp:
      case CK_ConstructorConversion:
      case CK_DerivedToBase:
      case CK_UncheckedDerivedToBase:
      case CK_UserDefinedConversion:
        E = CE->getSubExpr();
        continue;
      default:
        return E;
      }
    }
    if (isa<InitListExpr>(E))
      return E;
    if (const Expr *Op = passThroughOperand(E, /*Arms=*/{})) {
      E = Op;
      continue;
    }
    return E;
  }
}

namespace {
/// The walk of SemaProfiles::forEachHandleSourceLeaf.
struct HandleSourceWalk {
  using HandleLeaf = SemaProfiles::HandleLeaf;
  llvm::function_ref<void(const HandleLeaf &)> F;
  SmallVector<const Expr *, 4> Arms;
  /// The locals whose initializers the walk follows.
  SemaProfiles::HandleWalkMemo &Followed;

  void emit(const Expr *E, HandleLeaf::Mode M, bool Partial) {
    F(HandleLeaf{E, M, Partial, Arms});
  }

  void walk(const Expr *E, bool Partial) {
    E = peelHandleSource(E);
    if (const auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
      auto [T, Fa] = conditionalArms(CO);
      for (const Expr *Arm : {T, Fa}) {
        Arms.push_back(Arm);
        walk(Arm, Partial);
        Arms.pop_back();
      }
      return;
    }
    // A pointer an owner's reset takes is its new state.
    if (E->getType()->isPointerType()) {
      emit(E, HandleLeaf::Mode::PointerValue, Partial);
      return;
    }
    if (const auto *CCE = dyn_cast<CXXConstructExpr>(E)) {
      const CXXConstructorDecl *Ctor = CCE->getConstructor();
      if (Ctor->isCopyOrMoveConstructor() && CCE->getNumArgs() >= 1) {
        walk(CCE->getArg(0), Partial);
        return;
      }
      // A std-table construction's state is its transfer arguments'.
      bool Any = false;
      if (isStdHandleConstruction(Ctor))
        for (unsigned I = 0,
                      N = std::min(CCE->getNumArgs(), Ctor->getNumParams());
             I != N; ++I) {
          const Expr *Arg = CCE->getArg(I);
          if (isa<CXXDefaultArgExpr>(Arg))
            continue;
          const ParmVarDecl *P = Ctor->getParamDecl(I);
          std::optional<HandleLeaf::Mode> M = handleTransferMode(P);
          if (!M)
            continue;
          // A pointer's value is classified through the operand a binding
          // of it classifies (SemaProfiles::bindingSourceOperand).
          bool AsPointerValue;
          if (*M == HandleLeaf::Mode::PointerValue)
            Arg = SemaProfiles::bindingSourceOperand(Arg, P->getType(),
                                                     AsPointerValue);
          if (!Arg)
            continue;
          emit(Arg, *M, Partial);
          Any = true;
          // A later transfer argument -- a sentinel or last iterator --
          // bounds the range.
          break;
        }
      if (!Any)
        emit(E, HandleLeaf::Mode::Handle, Partial);
      return;
    }
    if (const auto *CE = dyn_cast<CallExpr>(E)) {
      // A Derive call with a class argument refers to what that range does.
      bool DerivePartial = false;
      if (const Expr *Arg = SemaProfiles::derivedArgument(CE, DerivePartial);
          Arg && SemaProfiles::isHandleType(Arg->getType())) {
        emit(Arg, HandleLeaf::Mode::Range, Partial || DerivePartial);
        return;
      }
      // A std::array's begin or data designates its element storage, or,
      // called through a pointer, the pointer's.
      bool PastStart = false;
      if (const Expr *Arr =
              SemaProfiles::stdArrayAddressObject(CE, PastStart)) {
        emit(Arr,
             Arr->getType()->isPointerType() ? HandleLeaf::Mode::PointerValue
                                             : HandleLeaf::Mode::Range,
             Partial || PastStart);
        return;
      }
      bool StepPartial = false;
      if (const Expr *Obj = handleSelfReturnObject(CE, StepPartial)) {
        walk(Obj, Partial || StepPartial);
        return;
      }
      emit(E, HandleLeaf::Mode::Handle, Partial);
      return;
    }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (const auto *BD = dyn_cast<BindingDecl>(DRE->getDecl());
          BD && BD->getBinding()) {
        walk(BD->getBinding(), Partial);
        return;
      }
      // An implicit local (range-for's __range and __begin) or an unmarked
      // local reference stands for its initializer, followed once.
      if (const auto *V = dyn_cast<VarDecl>(DRE->getDecl());
          V && V->hasLocalStorage() && !isa<ParmVarDecl>(V) && V->getInit() &&
          (V->isImplicit() || (V->getType()->isReferenceType() &&
                               !V->hasAttr<RefToUninitAttr>()))) {
        if (Followed.insert(V, Partial))
          walk(V->getInit(), Partial);
        return;
      }
    }
    emit(E, HandleLeaf::Mode::Handle, Partial);
  }
};
} // namespace

void SemaProfiles::forEachHandleSourceLeaf(
    const Expr *Src, llvm::function_ref<void(const HandleLeaf &)> F,
    HandleWalkMemo *Memo) {
  if (!Src)
    return;
  HandleWalkMemo Own;
  HandleSourceWalk Walk{F, {}, Memo ? *Memo : Own};
  Walk.walk(Src, /*Partial=*/false);
}

/// True if \p RD is a std::array specialization.
static bool isStdArrayClass(const CXXRecordDecl *RD) {
  return RD && RD->getIdentifier() && RD->getName() == "array" &&
         RD->isInStdNamespace();
}

/// True if \p T is a std::array specialization.
static bool isStdArrayType(QualType T) {
  return isStdArrayClass(T->getAsCXXRecordDecl());
}

bool SemaProfiles::isStdArrayAddressMember(const CXXMethodDecl *MD) {
  if (!MD->getIdentifier() || !isStdArrayClass(MD->getParent()))
    return false;
  return llvm::is_contained(
      ArrayRef<StringRef>{"begin", "end", "cbegin", "cend", "data"},
      MD->getName());
}

const Expr *SemaProfiles::stdArrayAddressObject(const Expr *E,
                                                bool &PastStart) {
  const auto *CE = dyn_cast<CallExpr>(E);
  const auto *MD =
      CE ? dyn_cast_or_null<CXXMethodDecl>(CE->getDirectCallee()) : nullptr;
  if (!MD || !isStdArrayAddressMember(MD))
    return nullptr;
  PastStart = MD->getName() == "end" || MD->getName() == "cend";
  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE))
    return MCE->getImplicitObjectArgument();
  return memberCallObject(CE, MD);
}

bool SemaProfiles::isStdArrayElementMember(const CXXMethodDecl *MD) {
  if (!isStdArrayClass(MD->getParent()))
    return false;
  if (MD->getOverloadedOperator() == OO_Subscript)
    return true;
  return MD->getIdentifier() &&
         llvm::is_contained(ArrayRef<StringRef>{"at", "front", "back"},
                            MD->getName());
}

bool SemaProfiles::isFlowTrackedHandleVar(const VarDecl *V) {
  return V && V->hasLocalStorage() && isHandleType(V->getType()) &&
         V->hasAttr<RefToUninitAttr>();
}

const DeclRefExpr *SemaProfiles::handleChainBase(const Expr *E,
                                                 ArrayRef<const Expr *> Arms,
                                                 bool &Partial,
                                                 bool *PastStart) {
  llvm::SmallPtrSet<const VarDecl *, 4> Followed;
  while (E) {
    E = peelHandleSource(E);
    if (const Expr *Arm = chosenArm(E, Arms)) {
      E = Arm;
      continue;
    }
    if (const auto *CCE = dyn_cast<CXXConstructExpr>(E)) {
      const CXXConstructorDecl *Ctor = CCE->getConstructor();
      if (Ctor->isCopyOrMoveConstructor() && CCE->getNumArgs() != 0) {
        E = CCE->getArg(0);
        continue;
      }
      // A std-table construction stands for its first transfer argument
      // (forEachHandleSourceLeaf), a pointer by its value.
      if (!isStdHandleConstruction(Ctor))
        return nullptr;
      E = nullptr;
      for (unsigned I = 0,
                    N = std::min(CCE->getNumArgs(), Ctor->getNumParams());
           I != N && !E; ++I) {
        const ParmVarDecl *P = Ctor->getParamDecl(I);
        std::optional<HandleLeaf::Mode> M = handleTransferMode(P);
        if (!M || isa<CXXDefaultArgExpr>(CCE->getArg(I)))
          continue;
        bool AsPointerValue;
        E = *M == HandleLeaf::Mode::PointerValue
                ? bindingSourceOperand(CCE->getArg(I), P->getType(),
                                       AsPointerValue)
                : CCE->getArg(I);
        if (!E)
          return nullptr;
        if (*M == HandleLeaf::Mode::PointerValue)
          E = E->IgnoreParenImpCasts();
      }
      continue;
    }
    if (const auto *CE = dyn_cast<CallExpr>(E)) {
      // A Derive call (std::begin(s)) stands for its argument; an offsetting
      // one (std::end(s)) is past the start.
      bool StepPartial = false, StepPastStart = false;
      if (const Expr *Arg = derivedArgument(CE, StepPartial)) {
        E = Arg;
        StepPastStart = StepPartial;
      } else {
        E = handleSelfReturnObject(CE, StepPartial, &StepPastStart);
      }
      Partial |= StepPartial;
      if (PastStart)
        *PastStart |= StepPastStart;
      continue;
    }
    const auto *DRE = dyn_cast<DeclRefExpr>(E);
    if (!DRE)
      return nullptr;
    // An implicit local or an unmarked local reference stands for its
    // initializer, as in forEachHandleSourceLeaf -- unless a lambda or block
    // reaches it by capture, whose initializer is the enclosing function's.
    if (const auto *V = dyn_cast<VarDecl>(DRE->getDecl());
        V && V->hasLocalStorage() && !isa<ParmVarDecl>(V) && V->getInit() &&
        (V->isImplicit() ||
         (V->getType()->isReferenceType() && !V->hasAttr<RefToUninitAttr>())) &&
        !DRE->refersToEnclosingVariableOrCapture() &&
        Followed.insert(V).second) {
      E = V->getInit();
      continue;
    }
    return DRE;
  }
  return nullptr;
}

bool SemaProfiles::isHandleDerefCall(const Expr *E, const Expr *&Obj,
                                     bool &Element) {
  const auto *CE = dyn_cast<CallExpr>(E);
  if (!CE)
    return false;
  Obj = derivingObject(CE);
  if (!Obj)
    return false;
  const auto *MD = cast<CXXMethodDecl>(CE->getDirectCallee());
  OverloadedOperatorKind OO = MD->getOverloadedOperator();
  bool Explicit = llvm::any_of(CE->arguments(), [&](const Expr *Arg) {
    return Arg != Obj && !isa<CXXDefaultArgExpr>(Arg);
  });
  Element = !(OO == OO_Star || OO == OO_Arrow ||
              (!Explicit && MD->getReturnType()->isPointerType()));
  return true;
}

static UninitStorage
pointerRefersToUninitStorage(ASTContext &Ctx, const Expr *E,
                             UninitAccessOpts Opts = UninitBindAccess);
/// The handle recognizer: classify whether the handle \p E refers to
/// uninitialized memory, combining its leaves (forEachHandleSourceLeaf) as
/// conditional arms combine. Markers count everywhere; \p Opts supplies
/// whether \c this is under construction.
static UninitStorage classifyHandleSource(ASTContext &Ctx, const Expr *E,
                                          UninitAccessOpts Opts);

/// The state of what a ReturnsDefaultInitialized callee \p FD returns: the
/// default-initialization of its first template argument's base element
/// type, [[uninit]] members counted (pointerRefersToUninitStorage's
/// new-expression arm); Unknown for a dependent or incomplete type.
static UninitStorage classifyDefaultInitializedResult(ASTContext &Ctx,
                                                      const FunctionDecl *FD) {
  const TemplateArgumentList *Args = FD->getTemplateSpecializationArgs();
  if (!Args || Args->size() == 0 ||
      Args->get(0).getKind() != TemplateArgument::Type)
    return UninitStorage::Unknown;
  QualType T = Ctx.getBaseElementType(Args->get(0).getAsType());
  if (T->isDependentType() || T->isIncompleteType())
    return UninitStorage::Unknown;
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
  return defaultInitLeavesScalarIndeterminateImpl(
             Ctx, T, /*HonorUninitMarkers=*/false, Visited)
             ? UninitStorage::Uninitialized
             : UninitStorage::Initialized;
}

/// Classify a terminal of the handle walk: a named variable, parameter, or
/// member is Uninitialized when marked and Initialized otherwise, except
/// that a marked reference to a class is Unknown (it refers to the handle's
/// own uninitialized storage); a call takes its marked callee's state, then
/// its known-callee row's (classifyDefaultInitializedResult), and is
/// otherwise Initialized, or Unknown with no direct callee; a std-table
/// construction with no transfer argument is Unknown, and any other
/// construction is Uninitialized when its constructor has an author-marked
/// parameter; an initializer list is Unknown; and any other glvalue -- a
/// handle reached through storage -- is Initialized when that storage is,
/// and Unknown otherwise.
static UninitStorage classifyHandleTerminal(ASTContext &Ctx, const Expr *E,
                                            UninitAccessOpts Opts) {
  E = E->IgnoreParenImpCasts();
  if (isa<InitListExpr, CXXParenListInitExpr, RecoveryExpr>(E))
    return UninitStorage::Unknown;
  if (const auto *CCE = dyn_cast<CXXConstructExpr>(E)) {
    const CXXConstructorDecl *Ctor = CCE->getConstructor();
    if (isStdHandleConstruction(Ctor))
      return UninitStorage::Unknown;
    return llvm::any_of(Ctor->parameters(),
                        [](const ParmVarDecl *P) {
                          return P->hasAttr<RefToUninitAttr>();
                        })
               ? UninitStorage::Uninitialized
               : UninitStorage::Initialized;
  }
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    const FunctionDecl *FD = CE->getDirectCallee();
    if (!FD)
      return UninitStorage::Unknown;
    if (FD->hasAttr<RefToUninitAttr>())
      return UninitStorage::Uninitialized;
    if (const KnownCalleeEntry *Entry = findKnownCallee(FD);
        Entry && Entry->Return == KnownCalleeReturn::ReturnsDefaultInitialized)
      return classifyDefaultInitializedResult(Ctx, FD);
    return UninitStorage::Initialized;
  }
  if (const ValueDecl *VD = SemaProfiles::getDirectlyNamedDecl(E);
      VD && isa<VarDecl, FieldDecl, IndirectFieldDecl>(VD)) {
    bool Marked = VD->hasAttr<RefToUninitAttr>();
    if (VD->getType()->isReferenceType())
      return Marked ? UninitStorage::Unknown : UninitStorage::Initialized;
    return Marked ? UninitStorage::Uninitialized : UninitStorage::Initialized;
  }
  if (E->isGLValue())
    return glvalueDenotesUninitStorage(Ctx, E, Opts) ==
                   UninitStorage::Initialized
               ? UninitStorage::Initialized
               : UninitStorage::Unknown;
  return UninitStorage::Initialized;
}

/// Classify one leaf of a handle source by its mode
/// (SemaProfiles::HandleLeaf::Mode).
static UninitStorage classifyHandleLeaf(ASTContext &Ctx,
                                        const SemaProfiles::HandleLeaf &L,
                                        UninitAccessOpts Opts) {
  using Mode = SemaProfiles::HandleLeaf::Mode;
  switch (L.M) {
  case Mode::PointerValue:
    return pointerRefersToUninitStorage(Ctx, L.E, Opts);
  case Mode::Storage:
    return glvalueDenotesUninitStorage(Ctx, L.E, Opts);
  case Mode::Range:
    if (L.E->isGLValue())
      if (UninitStorage S = glvalueDenotesUninitStorage(Ctx, L.E, Opts);
          S != UninitStorage::Initialized)
        return S;
    return classifyHandleSource(Ctx, L.E, Opts);
  case Mode::Handle:
    return classifyHandleTerminal(Ctx, L.E, Opts);
  }
  llvm_unreachable("unknown HandleLeaf mode");
}

static UninitStorage classifyHandleSource(ASTContext &Ctx, const Expr *E,
                                          UninitAccessOpts Opts) {
  SemaProfiles::HandleWalkMemo Own;
  SemaProfiles::HandleWalkMemo *Memo = Opts.HandleMemo ? Opts.HandleMemo : &Own;
  UninitAccessOpts LeafOpts =
      UninitBindAccess.withThisUnderConstruction(Opts.ThisUnderConstruction)
          .withHandleMemo(Memo);
  std::optional<UninitStorage> Acc;
  SemaProfiles::forEachHandleSourceLeaf(
      E,
      [&](const SemaProfiles::HandleLeaf &L) {
        UninitStorage S = classifyHandleLeaf(Ctx, L, LeafOpts);
        Acc = Acc ? combineArms(*Acc, S) : S;
      },
      Memo);
  return Acc.value_or(UninitStorage::Unknown);
}

// A call to a [[ref_to_uninit]]-returning function yields uninitialized
// storage (the pointed-to memory, or the returned referent) -- deferred to
// Unknown when the marker is trusted (a store). Known allocator callees
// (the table above) are classified the same way without a marker, and so is
// what a member call derives from an uninitialized handle (derivingObject).
// Any other unmarked direct callee is trusted Initialized (paper §4.3); a
// call with no direct callee (through a function pointer) is Unknown. Shared
// by both recognizers.
static UninitStorage classifyRefToUninitCallee(ASTContext &Ctx,
                                               const CallExpr *CE,
                                               UninitAccessOpts Opts) {
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return UninitStorage::Unknown;
  bool RefersToUninit = FD->hasAttr<RefToUninitAttr>();
  if (KnownCalleeMatch M = matchKnownCallee(FD); M.Entry) {
    if (!M.Trusted) {
      // Recognized by name but not the builtin (-fno-builtin,
      // -ffreestanding, a non-matching declaration): the trusted default
      // below is right for an arbitrary callee, wrong for one whose name
      // says "allocator" -- and the row's semantics cannot be assumed
      // either. Unclassified: neither binding direction diagnoses. An
      // explicit [[ref_to_uninit]] marker on the declaration still
      // classifies below -- the user declared the semantics themselves.
      if (!RefersToUninit)
        return UninitStorage::Unknown;
    } else {
      switch (M.Entry->Return) {
      case KnownCalleeReturn::ReturnsUninitialized:
        RefersToUninit = true;
        break;
      case KnownCalleeReturn::ReturnsInitialized:
        return UninitStorage::Initialized;
      case KnownCalleeReturn::ReturnsUnknown:
        return UninitStorage::Unknown;
      case KnownCalleeReturn::ReturnsTrusted:
      // A handle result reaches neither recognizer (classifyHandleTerminal).
      case KnownCalleeReturn::ReturnsDefaultInitialized:
        break;
      }
    }
  }
  if (!RefersToUninit) {
    // A Derive call refers to what its argument does: an array as it
    // decays, a pointer by its value, a class as a range.
    bool Partial = false;
    if (const Expr *Arg = SemaProfiles::derivedArgument(CE, Partial)) {
      QualType AT = Arg->getType();
      if (AT->isArrayType())
        return glvalueDenotesUninitStorage(Ctx, Arg,
                                           Opts.withoutTopLevelDrop());
      if (AT->isPointerType())
        return pointerRefersToUninitStorage(Ctx, Arg, Opts);
      if (SemaProfiles::isHandleType(AT))
        return classifyHandleLeaf(
            Ctx, {Arg, SemaProfiles::HandleLeaf::Mode::Range, Partial}, Opts);
      return UninitStorage::Unknown;
    }
    // A std::array's begin or data points into its element storage, as a
    // decayed array does -- through a pointer, into the pointer's.
    bool PastStart = false;
    if (const Expr *Arr = SemaProfiles::stdArrayAddressObject(CE, PastStart))
      return Arr->getType()->isPointerType()
                 ? pointerRefersToUninitStorage(Ctx, Arr, Opts)
                 : glvalueDenotesUninitStorage(Ctx, Arr,
                                               Opts.withoutTopLevelDrop());
    if (const Expr *Obj = derivingObject(CE))
      if (UninitStorage S = classifyHandleSource(Ctx, Obj, Opts);
          isUninitializedOrMixed(S))
        return Opts.TrustRefToUninit ? UninitStorage::Unknown : S;
    return UninitStorage::Initialized;
  }
  return Opts.TrustRefToUninit ? UninitStorage::Unknown
                               : UninitStorage::Uninitialized;
}

// \p E is a pointer prvalue. Classifies whether it points to uninitialized
// storage.
static UninitStorage pointerRefersToUninitStorage(ASTContext &Ctx,
                                                  const Expr *E,
                                                  UninitAccessOpts Opts) {
  if (!E)
    return UninitStorage::Unknown;
  E = E->IgnoreParenImpCasts();

  // An empty braced list value-initializes a pointer to null, so it takes the
  // null classification below (Unknown), keeping `= {}` and `= nullptr`
  // consistent.
  if (auto PassThrough = classifyUninitPassThrough(
          E, /*EmptyListState=*/UninitStorage::Unknown, Opts.Arms,
          [&](const Expr *Sub) {
            return pointerRefersToUninitStorage(Ctx, Sub, Opts);
          }))
    return *PassThrough;

  // A null pointer refers to no object, so it is consistent with both a
  // marked target (the marker means "zero or more uninitialized objects",
  // paper §8) and an unmarked one (paper §4.3's f1(p2) example): Unknown,
  // which neither direction diagnoses. A dedicated UninitStorage::Null state
  // was considered and deferred -- behaviorally identical today; add it only
  // when a future rule (e.g. construct_at on null) needs to distinguish null
  // from unclassifiable.
  if (E->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull) !=
      Expr::NPCK_NotNull)
    return UninitStorage::Unknown;

  // Array-to-pointer decay has been stripped above, leaving the array glvalue.
  // Clear the top-level drop here, like the member-access arm: neither the CFG
  // uninit_read pass nor the ctor-body pass tracks array elements, and
  // element-wise delayed initialization of an [[uninit]] array is itself
  // banned (paper §5.5), so below an element access the marker counts even
  // for a value access.
  if (E->getType()->isArrayType())
    return glvalueDenotesUninitStorage(Ctx, E, Opts.withoutTopLevelDrop());

  // &G or std::addressof(G), where G denotes uninitialized storage.
  if (const Expr *G = SemaProfiles::addressOfOperand(E))
    return glvalueDenotesUninitStorage(Ctx, G, Opts);

  // A structured binding classifies as the member or element it decomposes.
  if (const Expr *Bound = getStructuredBindingSource(E))
    return pointerRefersToUninitStorage(Ctx, Bound, Opts);

  // A value of a [[ref_to_uninit]] pointer is Uninitialized (Unknown when the
  // marker is trusted); an unmarked named pointer is a trusted Initialized
  // pointer (paper §4.3). A named *function* is excluded: function-to-pointer
  // decay hands this arm the FunctionDecl, whose [[ref_to_uninit]] describes
  // its RETURN value, not the function pointer -- and a marked
  // function-pointer target cannot legally exist (the marker is rejected on
  // function pointers and references), so the value falls through to the
  // remaining arms and ends Unknown: accepted for both directions, without
  // turning a cast of it ((void *)f) into a trusted-Initialized source.
  if (const ValueDecl *VD = SemaProfiles::getDirectlyNamedDecl(E);
      VD && !isa<FunctionDecl>(VD)) {
    if (!VD->hasAttr<RefToUninitAttr>()) {
      // An unmarked *local* whose declaration initializer is null -- a null
      // pointer constant, or an empty braced list, which value-initializes to
      // null -- is a null source like the literal (paper §4.3's f1(p2)
      // example): Unknown. Reassignment after the null init is a documented,
      // accepted missed diagnostic (an unmarked pointer is not flow-tracked).
      // Excluded: globals/extern (an extern pointer may be initialized
      // elsewhere, and keeping them Initialized preserves the
      // marked-direction diagnostics), null-NSDMI fields, and parameters --
      // a ParmVarDecl's getInit() is its *default argument*, which is not
      // the parameter's value on most calls. A *marked* decl keeps its
      // marker classification below (respect the explicit marker).
      if (const auto *Var = dyn_cast<VarDecl>(VD);
          Var && Var->hasLocalStorage() && !isa<ParmVarDecl>(Var)) {
        if (const Expr *Init = Var->getInit()) {
          const Expr *InnerInit = Init->IgnoreParenImpCasts();
          const auto *ILE = dyn_cast<InitListExpr>(InnerInit);
          if ((ILE && ILE->getNumInits() == 0) ||
              InnerInit->isNullPointerConstant(
                  Ctx, Expr::NPC_ValueDependentIsNotNull) != Expr::NPCK_NotNull)
            return UninitStorage::Unknown;
        }
      }
      return UninitStorage::Initialized;
    }
    return Opts.TrustRefToUninit ? UninitStorage::Unknown
                                 : UninitStorage::Uninitialized;
  }
  // `this` is the current object: Initialized in a member function, so a
  // marked pointer or reference to an initialized member is rejected (P4222R2
  // §4.5: a marked accessor returning `x` is legal only if `x` is [[uninit]]);
  // Unknown while the object is under construction (see UninitAccessOpts).
  if (isa<CXXThisExpr>(E))
    return Opts.ThisUnderConstruction ? UninitStorage::Unknown
                                      : UninitStorage::Initialized;
  if (const auto *CE = dyn_cast<CallExpr>(E))
    return classifyRefToUninitCallee(Ctx, CE, Opts);

  // A default-initialized new-expression (none init style: no initializer
  // written) whose allocated type's default-initialization leaves a scalar
  // subobject indeterminate produces uninitialized free-store memory (paper
  // §1.2/§4.3), like a [[ref_to_uninit]] allocator. The style gates this
  // rather
  // than hasInitializer(), which is also true for new Agg -- default-
  // initializing a class synthesizes a (possibly trivial) constructor call.
  // new T(...) / new T{} are value- or list-initialized; a user-provided
  // default constructor is trusted by defaultInitLeavesScalarIndeterminate.
  // An [[uninit]] member is left indeterminate too: its marker acknowledges
  // the member, it does not initialize it.
  if (const auto *NE = dyn_cast<CXXNewExpr>(E)) {
    if (NE->getInitializationStyle() != CXXNewInitializationStyle::None)
      return UninitStorage::Initialized;
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
    return defaultInitLeavesScalarIndeterminateImpl(
               Ctx, NE->getAllocatedType(),
               /*HonorUninitMarkers=*/false, Visited)
               ? UninitStorage::Uninitialized
               : UninitStorage::Initialized;
  }

  // Paper §4.3: a [[ref_to_uninit]] pointer cast to another pointer type is
  // itself [[ref_to_uninit]]. Implicit casts were already stripped above, so
  // this only looks through an explicit pointer-to-pointer cast; a pointer
  // manufactured from an integer (operand not a pointer) is Unknown.
  if (const auto *CE = dyn_cast<ExplicitCastExpr>(E))
    if (CE->getSubExpr()->getType()->isPointerType())
      return pointerRefersToUninitStorage(Ctx, CE->getSubExpr(), Opts);

  return UninitStorage::Unknown;
}

// IgnoreParenImpCasts, except it stops at a MaterializeTemporaryExpr
// (IgnoreParenImpCasts strips MTEs -- see the FIXME in IgnoreExpr.h). The
// glvalue recognizer must see the MTE: the materialized temporary is its own
// object, whose state is independent of the expression it was converted from.
static const Expr *ignoreParenImpCastsKeepMTE(const Expr *E) {
  return IgnoreExprNodes(E, IgnoreParensSingleStep, [](Expr *Node) {
    if (isa<MaterializeTemporaryExpr>(Node))
      return Node;
    return IgnoreImplicitCastsExtraSingleStep(Node);
  });
}

/// A glvalue of pointer type bound as a read-only alias (T *const &, T *&&,
/// or a materialized pointer temporary) denotes whatever its value points
/// to, so it classifies as that value; a mutable alias (T *&) denotes the
/// pointer object, whose marking the alias cannot carry: Unknown (P4222R2
/// §4.3). The alias applies to this glvalue only, so the value
/// classification runs alias-free.
static UninitStorage classifyPointerGlvalue(ASTContext &Ctx, const Expr *E,
                                            UninitAccessOpts Opts) {
  using PointerAlias = UninitAccessOpts::PointerAlias;
  if (Opts.Alias == PointerAlias::Mutable)
    return UninitStorage::Unknown;
  return pointerRefersToUninitStorage(Ctx, E,
                                      Opts.withAlias(PointerAlias::None));
}

// \p E is a glvalue. Classifies whether it denotes uninitialized storage.
static UninitStorage glvalueDenotesUninitStorage(ASTContext &Ctx, const Expr *E,
                                                 UninitAccessOpts Opts) {
  if (!E)
    return UninitStorage::Unknown;
  E = ignoreParenImpCastsKeepMTE(E);

  // A pointer glvalue bound as an alias is classified by the alias kind.
  if (Opts.Alias != UninitAccessOpts::PointerAlias::None)
    return classifyPointerGlvalue(Ctx, E, Opts);

  // A materialized temporary is a fresh object initialized from its
  // subexpression's *value*: whatever storage that value was loaded from, the
  // temporary itself is initialized (e.g. `const long &r = u;` binds a new
  // long temporary, not `u`). A *pointer-typed* temporary is the exception:
  // its value still refers to the same storage, so it is a read-only alias of
  // whatever the value points to (`const int *const &rp = alloc();`).
  if (const auto *MTE = dyn_cast<MaterializeTemporaryExpr>(E)) {
    if (MTE->getType()->isPointerType())
      return classifyPointerGlvalue(
          Ctx, MTE->getSubExpr(),
          Opts.withAlias(UninitAccessOpts::PointerAlias::ReadOnly));
    return UninitStorage::Initialized;
  }

  if (auto PassThrough = classifyUninitPassThrough(
          E, /*EmptyListState=*/UninitStorage::Unknown, Opts.Arms,
          [&](const Expr *Sub) {
            return glvalueDenotesUninitStorage(Ctx, Sub, Opts);
          }))
    return *PassThrough;

  // A named entity denotes uninitialized storage if it is [[uninit]], or
  // if it is a reference marked [[ref_to_uninit]] (the glvalue is its referent,
  // which is uninitialized). A [[ref_to_uninit]] *pointer* named here denotes
  // the pointer object itself -- which is initialized -- so it does not count
  // (a pointer glvalue bound as an alias was classified by
  // classifyPointerGlvalue above and never reaches this arm).
  // Under the top-level drop the [[uninit]] arm is skipped: a value access of
  // a directly named [[uninit]] object is the flow-based passes' territory, so
  // only a [[ref_to_uninit]] reference (or indirection, handled below) still
  // counts. The flow state of a tracked entity -- a whole-entity store is
  // the [[uninit]] entity's initialization (paper §4.2/§4.5), a store through
  // a marked reference its referent's (§4.3) -- is the CFG pass's, which
  // judges every access with a tracked leaf instead of this recognizer.
  auto DeclDenotesUninit = [&](const ValueDecl *VD) {
    return (!Opts.DropTopLevelUninit && VD->hasAttr<UninitAttr>() &&
            !(Opts.TrustUninitMembers &&
              isa<FieldDecl, IndirectFieldDecl>(VD))) ||
           (!Opts.TrustRefToUninit && VD->getType()->isReferenceType() &&
            VD->hasAttr<RefToUninitAttr>());
  };
  // A structured binding classifies as the member or element it decomposes.
  if (const Expr *Bound = getStructuredBindingSource(E))
    return glvalueDenotesUninitStorage(Ctx, Bound, Opts);
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DeclDenotesUninit(DRE->getDecl()) ? UninitStorage::Uninitialized
                                             : UninitStorage::Initialized;
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    // a->m reaches m through the pointer a (object *a); a.m through the
    // glvalue a. When m does not itself denote uninit storage, the subobject is
    // uninit exactly when its base is. The base recursion clears the top-level
    // drop: the drop exists because a directly named [[uninit]] entity's value
    // accesses are owned by the flow passes or trusted (see the
    // UninitAccessOpts comment above), but nothing tracks a subobject reached
    // through a *further* member access -- and member-wise delayed
    // initialization of an [[uninit]] object is itself banned (paper §5.4;
    // only whole-object construct_at re-initializes, which is uniformly
    // unmodeled) -- so below the top level the marker counts for every access.
    // The base recursion clears the marker trust for the same reason: the
    // write preset trusts the marker only at the top level, where a scalar
    // write is the whole pointee's initialization (§4.5) -- below a member
    // step the write initializes nothing, so [[ref_to_uninit]] markers,
    // marked callees, and the allocator tail all count again. A tracked
    // member of a directly named local or of the current object (`a.m`,
    // `this->m`) never reaches this arm: the CFG pass judges its accesses by
    // flow state (paper §4.2: "After initialization, the object is no longer
    // [[uninit]]").
    const ValueDecl *MD = ME->getMemberDecl();
    if (DeclDenotesUninit(MD))
      return UninitStorage::Uninitialized;
    // Only a field (or an indirect field through an anonymous union/struct)
    // is a subobject of the base; a non-field member -- a static data member,
    // an enumerator, a member function -- names static storage (or no storage
    // at all) whose state is independent of the base object, so it classifies
    // exactly as the DeclRefExpr arm would classify the member itself:
    // Initialized. The DeclDenotesUninit check above still runs first so a
    // [[ref_to_uninit]]-marked static reference member classifies
    // Uninitialized, and returning Initialized (not Unknown) preserves the
    // marked-direction rejection of `int *mp [[ref_to_uninit]] = &s.sm;`
    // (a static's zero-init is a language guarantee) and keeps `&S::sm` and
    // `&s.sm` classifying alike.
    if (!isa<FieldDecl, IndirectFieldDecl>(MD))
      return UninitStorage::Initialized;
    return ME->isArrow() ? pointerRefersToUninitStorage(
                               Ctx, ME->getBase(),
                               Opts.withoutTopLevelDrop().withoutMarkerTrust())
                         : glvalueDenotesUninitStorage(
                               Ctx, ME->getBase(),
                               Opts.withoutTopLevelDrop().withoutMarkerTrust());
  }
  // a.*pm and p->*pm: a member access whose member is not named, so it
  // classifies like the member arm above for an unmarked member.
  if (const BinaryOperator *BO = SemaProfiles::asDataMemberPointerAccess(E))
    return BO->getOpcode() == BO_PtrMemI
               ? pointerRefersToUninitStorage(
                     Ctx, BO->getLHS(),
                     Opts.withoutTopLevelDrop().withoutMarkerTrust())
               : glvalueDenotesUninitStorage(
                     Ctx, BO->getLHS(),
                     Opts.withoutTopLevelDrop().withoutMarkerTrust());
  // A call to a [[ref_to_uninit]]-returning reference function: the referent
  // it returns is uninitialized.
  if (const auto *CE = dyn_cast<CallExpr>(E))
    return classifyRefToUninitCallee(Ctx, CE, Opts);
  // An element access classifies like its base (an element is never
  // flow-tracked: `*p = 5;` never legalizes `p[1]` -- the pointee may be an
  // array with only element 0 written, and element-wise state is
  // untrackable by design, paper §5.4/§5.5).
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return pointerRefersToUninitStorage(Ctx, ASE->getBase(), Opts);

  // *p, where p points to uninitialized storage.
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    if (UO->getOpcode() == UO_Deref)
      return pointerRefersToUninitStorage(Ctx, UO->getSubExpr(), Opts);

  // A reference cast (an explicit cast yielding a glvalue) denotes the same
  // storage as its operand; propagate. Symmetric to the pointer-cast arm.
  if (const auto *CE = dyn_cast<ExplicitCastExpr>(E))
    if (CE->getSubExpr()->isGLValue())
      return glvalueDenotesUninitStorage(Ctx, CE->getSubExpr(), Opts);

  return UninitStorage::Unknown;
}

/// Dispatch a binding source bound as \p T to the glvalue recognizer for a
/// reference, the handle recognizer for a handle (SemaProfiles::isHandleType)
/// or an \p AsRange source (a range a role parameter takes by reference), and
/// the pointer recognizer otherwise.
static UninitStorage
classifyUninitSource(ASTContext &Ctx, const Expr *E, QualType T,
                     UninitAccessOpts Opts = UninitBindAccess,
                     bool AsRange = false) {
  if (AsRange)
    return classifyHandleLeaf(
        Ctx, {E, SemaProfiles::HandleLeaf::Mode::Range, false, Opts.Arms},
        Opts);
  if (T->isReferenceType())
    return glvalueDenotesUninitStorage(Ctx, E, Opts);
  if (SemaProfiles::isHandleType(T))
    return classifyHandleSource(Ctx, E, Opts);
  return pointerRefersToUninitStorage(Ctx, E, Opts);
}

/// True for the binding kinds whose target declaration can carry
/// [[ref_to_uninit]]; every other kind reaching the funnel is judged
/// unmarked. PointerAssignment, ByRefCapture, and ObjectArgument resolve
/// their marking in their own derive steps and never reach the funnel.
static bool bindingTargetCanCarryMarker(SemaProfiles::InitBindingKind Kind) {
  using K = SemaProfiles::InitBindingKind;
  switch (Kind) {
  case K::Variable:
  case K::DataMember:
  case K::Parameter:
  case K::Return:
    return true;
  case K::AggregateElement:
  case K::PointerAssignment:
  case K::Throw:
  case K::NewInitializer:
  case K::VariadicArgument:
  case K::ByCopyCapture:
  case K::ByRefCapture:
  case K::ObjectArgument:
    return false;
  }
  llvm_unreachable("unknown InitBindingKind");
}

/// The verdict step of the binding funnel: diagnose binding a source in
/// \p SrcState as \p T to a target whose marking is \p TargetMarked, in
/// \p Kind's wording. A marked target rejects an affirmatively Initialized
/// source and an unmarked one an affirmatively Uninitialized source; a Mixed
/// source is rejected by both (P4222R2 §4.9), and an Unknown source (pointer
/// arithmetic, an integer-to-pointer cast, a call through a function pointer)
/// by neither. \p Subject is the entity the ByRefCapture and ObjectArgument
/// wordings name.
static void diagnoseBindingVerdict(SemaProfiles &SP,
                                   SemaProfiles::InitBindingKind Kind,
                                   SourceLocation Loc, bool TargetMarked,
                                   QualType T, UninitStorage SrcState,
                                   const NamedDecl *Subject) {
  using K = SemaProfiles::InitBindingKind;
  static constexpr StringRef Profile = "std::init";
  // The shape select of the binding diagnostics: pointer, reference, handle.
  unsigned Shape = T->isReferenceType()            ? 1
                   : SemaProfiles::isHandleType(T) ? 2
                                                   : 0;
  if (TargetMarked) {
    if (SrcState == UninitStorage::Initialized ||
        SrcState == UninitStorage::Mixed)
      SP.Diag(Loc, diag::err_init_ref_to_uninit_requires_uninit)
          << Profile << Shape << T;
    return;
  }
  if (!isUninitializedOrMixed(SrcState))
    return;
  switch (Kind) {
  case K::ByRefCapture:
    SP.Diag(Loc, diag::err_init_uninit_ref_capture) << Profile << Subject;
    return;
  case K::ObjectArgument:
    SP.Diag(Loc, diag::err_init_member_call_on_uninit) << Profile << Subject;
    return;
  default:
    SP.Diag(Loc, diag::err_init_uninit_requires_ref_to_uninit)
        << Profile << Shape << T;
    return;
  }
}

bool SemaProfiles::inRebuiltOrSynthesizedCode() const {
  if (SemaRef.needsRebuildOfDefaultArgOrInit() ||
      SemaRef.currentEvaluationContext()
          .DelayedDefaultInitializationContext.has_value())
    return true;
  const auto *FD =
      dyn_cast_or_null<FunctionDecl>(flowAnalysisOwner(SemaRef.CurContext));
  return FD && FD->isDefaulted();
}

bool SemaProfiles::thisIsUnderConstruction(const Decl *AfterFlowPass) const {
  if (AfterFlowPass)
    return isa_and_nonnull<CXXConstructorDecl>(
        enclosingInstanceMethod(dyn_cast<DeclContext>(AfterFlowPass)));
  // The enclosing non-lambda context: a constructor (body or
  // mem-initializer), or the class itself while a default member initializer
  // is parsed or instantiated.
  return isa<CXXConstructorDecl, CXXRecordDecl>(
      SemaRef.getFunctionLevelDeclContext());
}

bool SemaProfiles::trustsUninitMembers(const Decl *AfterFlowPass) const {
  return AfterFlowPass ||
         !isa<CXXRecordDecl>(SemaRef.getFunctionLevelDeclContext());
}

const Decl *SemaProfiles::flowAnalysisOwner(const DeclContext *DC) {
  while (isa_and_nonnull<CapturedDecl>(DC))
    DC = DC->getParent();
  return DC && DC->isFunctionOrMethod() ? cast<Decl>(DC) : nullptr;
}

void SemaProfiles::recordDeferredInitSite(DeferredInitSite Site) {
  const Sema::ExpressionEvaluationContextRecord &EC =
      SemaRef.currentEvaluationContext();
  if ((EC.Context != Sema::ExpressionEvaluationContext::PotentiallyEvaluated &&
       EC.Context !=
           Sema::ExpressionEvaluationContext::PotentiallyEvaluatedIfUsed) ||
      EC.isDiscardedStatementContext() || EC.isImmediateFunctionContext() ||
      SemaRef.isSFINAEContext() || SemaRef.CurContext->isDependentContext())
    return;
  const Decl *Owner = flowAnalysisOwner(SemaRef.CurContext);
  if (!Owner)
    return;
  DeferredInitSites[Owner].push_back(std::move(Site));
}

SmallVector<SemaProfiles::DeferredInitSite, 4>
SemaProfiles::takeDeferredInitSites(const Decl *Body) {
  auto It = DeferredInitSites.find(Body);
  if (It == DeferredInitSites.end())
    return {};
  SmallVector<DeferredInitSite, 4> Sites = std::move(It->second);
  DeferredInitSites.erase(It);
  return Sites;
}

void SemaProfiles::discardDeferredInitSites(const Decl *Body) {
  DeferredInitSites.erase(Body);
}

void SemaProfiles::judgeDeferredInitSiteByForm(const DeferredInitSite &Site,
                                               const Decl *Body) {
  switch (Site.K) {
  case DeferredInitSite::Kind::Binding:
    judgeInitProfileBinding(Site.BindingKind, Site.Loc, Site.Flag, Site.T,
                            Site.Src, /*D=*/nullptr, Site.Subject, Body,
                            Site.ValueCopy, Site.AsRange);
    return;
  case DeferredInitSite::Kind::Destroy:
    if (Site.Flag)
      judgeDestroyUninitByForm(Site.Loc, Site.T, Site.Src, /*D=*/nullptr, Body,
                               Site.Destroyer, Site.AsRange);
    return;
  case DeferredInitSite::Kind::ReadThrough:
    checkInitProfileReadThrough(Site.Loc, Site.Src, Site.T, Body);
    return;
  case DeferredInitSite::Kind::SubobjectWrite:
    checkInitProfileSubobjectWrite(Site.Loc, Site.Src, Body);
    return;
  }
  llvm_unreachable("unknown DeferredInitSite kind");
}

void SemaProfiles::checkInitProfileVariadicArgument(const Expr *Arg) {
  if (!isProfileLive("std::init"))
    return;
  checkInitProfileBinding(InitBindingKind::VariadicArgument, Arg->getExprLoc(),
                          /*Target=*/nullptr, Arg->getType(), Arg);
}

void SemaProfiles::checkInitProfileBinding(const InitializedEntity &Entity,
                                           const InitializationKind &Kind,
                                           const Expr *Init) {
  if (!isProfileLive("std::init") || !Init)
    return;
  // Each element of a list initializing an object is bound by a sequence of
  // its own; a reference list-initializes from its lone element here.
  if (isa<InitListExpr, CXXParenListInitExpr>(Init) &&
      !Entity.getType()->isReferenceType())
    return;
  // A SFINAE trap is not a use; see also inRebuiltOrSynthesizedCode.
  if (SemaRef.isSFINAEContext() || inRebuiltOrSynthesizedCode())
    return;
  if (Entity.isImplicitMemberInitializer())
    return;
  const Expr *Src = Init;
  SourceLocation Loc =
      Kind.getLocation().isValid() ? Kind.getLocation() : Src->getExprLoc();
  switch (Entity.getKind()) {
  case InitializedEntity::EK_Variable: {
    const ValueDecl *Var = Entity.getDecl();
    // An implicit handle local (range-for's __begin) stands for its
    // initializer, which the handle walk sees through.
    if (Var->isImplicit() && isHandleType(Entity.getType()))
      return;
    checkInitProfileBinding(InitBindingKind::Variable, Loc, Var,
                            Entity.getType(), Src, Var);
    return;
  }
  case InitializedEntity::EK_Member:
  case InitializedEntity::EK_ParenAggInitMember: {
    // The deferral anchor: an aggregate element (a parent entity) has none
    // and defers on an instantiation-dependent source; a mem-initializer
    // anchors on its constructor, a default member initializer on its field.
    const auto *Field = cast<FieldDecl>(Entity.getDecl());
    const Decl *D = nullptr;
    if (!Entity.getParent())
      D = isa<CXXConstructorDecl>(SemaRef.CurContext)
              ? cast<Decl>(SemaRef.CurContext)
              : Field;
    checkInitProfileBinding(InitBindingKind::DataMember, Loc, Field,
                            Entity.getType(), Src, D);
    return;
  }
  case InitializedEntity::EK_Base:
    // A base subobject -- of a written base initializer or an aggregate's
    // base element -- cannot carry the marker.
    checkInitProfileBinding(InitBindingKind::DataMember, Loc,
                            /*Target=*/nullptr, Entity.getType(), Src,
                            !Entity.getParent() &&
                                    isa<CXXConstructorDecl>(SemaRef.CurContext)
                                ? cast<Decl>(SemaRef.CurContext)
                                : nullptr);
    return;
  case InitializedEntity::EK_ArrayElement:
  case InitializedEntity::EK_VectorElement:
  case InitializedEntity::EK_ComplexElement:
    checkInitProfileBinding(InitBindingKind::AggregateElement, Loc,
                            /*Target=*/nullptr, Entity.getType(), Src);
    return;
  case InitializedEntity::EK_Result: {
    // The returned value binds against the returning function's own marking
    // (P4222R2 §8.2): a lambda's call operator carries it, a block cannot
    // carry one. A block or lambda body anchors on its declaration, so a
    // call operator in a templated entity defers to the rebuild at
    // instantiation.
    const FunctionDecl *Target =
        SemaRef.getCurBlock()
            ? nullptr
            : SemaRef.getCurFunctionDecl(/*AllowLambda=*/true);
    const Decl *D = nullptr;
    if (isa<BlockDecl>(SemaRef.CurContext) ||
        isLambdaCallOperator(SemaRef.CurContext))
      D = cast<Decl>(SemaRef.CurContext);
    checkInitProfileBinding(InitBindingKind::Return, Loc, Target,
                            Entity.getType(), Src, D);
    return;
  }
  case InitializedEntity::EK_Exception:
    // The thrown value copy-initializes the exception object, which cannot
    // carry the marker.
    checkInitProfileBinding(InitBindingKind::Throw, Loc, /*Target=*/nullptr,
                            Entity.getType(), Src);
    return;
  case InitializedEntity::EK_New:
    // The written initializer of a scalar allocation binds the allocated
    // pointer, which cannot carry the marker; an array allocation is
    // array-typed here, and its elements arrive as array elements.
    checkInitProfileBinding(InitBindingKind::NewInitializer, Loc,
                            /*Target=*/nullptr, Entity.getType(), Src);
    return;
  case InitializedEntity::EK_LambdaCapture: {
    // A variable capture or an init-capture initializes a closure field,
    // which cannot carry the marker. A by-reference capture of a named
    // variable keeps its own wording; the capture anchors on the enclosing
    // context, so one in a templated entity defers to the rebuild at
    // instantiation.
    const Decl *D = cast<Decl>(SemaRef.CurContext);
    if (Entity.getType()->isReferenceType()) {
      if (const ValueDecl *Var = getDirectlyNamedDecl(Src)) {
        judgeInitProfileBinding(InitBindingKind::ByRefCapture, Loc,
                                /*TargetMarked=*/false, Entity.getType(), Src,
                                D, Var);
        if (bindsHandleByReference(InitBindingKind::ByRefCapture,
                                   Entity.getType(), /*Target=*/nullptr))
          judgeInitProfileBinding(
              InitBindingKind::ByRefCapture, Loc, /*TargetMarked=*/false,
              Entity.getType().getNonReferenceType().getUnqualifiedType(), Src,
              D, Var);
        return;
      }
      checkInitProfileBinding(InitBindingKind::Variable, Loc,
                              /*Target=*/nullptr, Entity.getType(), Src, D);
      return;
    }
    checkInitProfileBinding(InitBindingKind::ByCopyCapture, Loc,
                            /*Target=*/nullptr, Entity.getType(), Src, D);
    return;
  }
  case InitializedEntity::EK_Parameter:
  case InitializedEntity::EK_Parameter_CF_Audited:
    // A call argument, or a default argument at its declaration
    // (ConvertParamDefaultArgument); a type-only parameter entity (a call
    // with no declared callee) has no declaration to carry the marker.
    checkInitProfileBinding(InitBindingKind::Parameter, Loc,
                            dyn_cast_or_null<ParmVarDecl>(Entity.getDecl()),
                            Entity.getType(), Src);
    return;
  default:
    return;
  }
}

void SemaProfiles::judgeInitProfileBinding(
    InitBindingKind Kind, SourceLocation Loc, bool TargetMarked, QualType T,
    const Expr *Src, const Decl *D, const NamedDecl *Subject,
    const Decl *AfterFlowPass, bool ValueCopy, bool AsRange) {
  // A RecoveryExpr is a placeholder for an initialization that already failed,
  // not a source the user wrote, so it must not drive this rule; see also
  // inRebuiltOrSynthesizedCode.
  if (!Src || isa<RecoveryExpr>(Src->IgnoreParens()) ||
      (!AfterFlowPass && inRebuiltOrSynthesizedCode()))
    return;
  // A source with a flow-tracked leaf is the CFG pass's: judged at its
  // program point, on each instantiation's own CFG (see
  // ProfilesFrameworkInternals.rst, "Flow-Tracked Storage").
  if (SmallVector<const Expr *, 2> Leaves;
      !AfterFlowPass &&
      hasFlowTrackedLeaf(Src, T, &Leaves,
                         TargetMarked ? FlowLeafUse::MarkedTarget
                         : ValueCopy  ? FlowLeafUse::ValueCopy
                                      : FlowLeafUse::Plain,
                         AsRange)) {
    DeferredInitSite Site{DeferredInitSite::Kind::Binding,
                          Kind,
                          Loc,
                          TargetMarked,
                          T,
                          Src,
                          Subject,
                          std::move(Leaves)};
    Site.ValueCopy = ValueCopy;
    Site.AsRange = AsRange;
    recordDeferredInitSite(std::move(Site));
    return;
  }
  // The expression-check template policy (ProfilesFrameworkInternals.rst,
  // "Check Sites"): without a Decl an instantiation-dependent source defers to
  // the rebuild; with one, the gate's templated rung defers instead.
  if (!D && Src->isInstantiationDependent())
    return;
  if (!shouldEmitProfileViolation("std::init",
                                  diag::err_init_uninit_requires_ref_to_uninit,
                                  Loc, D, /*PostParse=*/AfterFlowPass))
    return;
  UninitAccessOpts Opts =
      UninitBindAccess.withAlias(pointerAliasOf(T))
          .withThisUnderConstruction(thisIsUnderConstruction(AfterFlowPass))
          .withUninitMemberTrust(ValueCopy &&
                                 trustsUninitMembers(AfterFlowPass));
  // A range is named as the handle it is.
  diagnoseBindingVerdict(
      *this, Kind, Loc, TargetMarked, AsRange ? T.getNonReferenceType() : T,
      classifyUninitSource(getASTContext(), Src, T, Opts, AsRange), Subject);
}

bool SemaProfiles::bindsHandleByReference(InitBindingKind Kind, QualType T,
                                          const ValueDecl *Target) {
  if (Kind == InitBindingKind::ObjectArgument || !T->isReferenceType() ||
      !isHandleType(T.getNonReferenceType()))
    return false;
  const auto *V = dyn_cast_or_null<VarDecl>(Target);
  if (V && V->isImplicit())
    return false;
  const auto *P = dyn_cast_or_null<ParmVarDecl>(V);
  // A comparison, a + or -, and std::advance compare or position their
  // handles.
  if (const auto *FD =
          P ? dyn_cast<FunctionDecl>(P->getDeclContext()) : nullptr) {
    if (isStdCallable(FD, "advance"))
      return false;
    switch (FD->getOverloadedOperator()) {
    case OO_EqualEqual:
    case OO_ExclaimEqual:
    case OO_Less:
    case OO_Greater:
    case OO_LessEqual:
    case OO_GreaterEqual:
    case OO_Spaceship:
    case OO_Plus:
    case OO_Minus:
      return false;
    default:
      break;
    }
  }
  // The source of a copy or move, which binds where the copy lands.
  return !isCopySource(P);
}

bool SemaProfiles::isDefaultedComparison(const FunctionDecl *FD) {
  return FD && FD->isDefaulted() && FD->getOverloadedOperator() != OO_None &&
         FD->getOverloadedOperator() != OO_Equal;
}

bool SemaProfiles::isValueCopySource(const ParmVarDecl *P) {
  const auto *FD = P ? dyn_cast<FunctionDecl>(P->getDeclContext()) : nullptr;
  if (!FD)
    return false;
  if (isDefaultedComparison(FD))
    return true;
  unsigned Index = P->getFunctionScopeIndex();
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD);
      MD && Index == (MD->isExplicitObjectMemberFunction() ? 1u : 0u)) {
    if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(MD))
      return Ctor->isCopyOrMoveConstructor();
    if (MD->isCopyAssignmentOperator() || MD->isMoveAssignmentOperator())
      return true;
  }
  if (FD->isInStdNamespace() && FD->getIdentifier() &&
      FD->getName() == "bit_cast")
    return Index == 0;
  switch (FD->getBuiltinID()) {
  case Builtin::BImemcpy:
  case Builtin::BI__builtin_memcpy:
  case Builtin::BI__builtin___memcpy_chk:
  case Builtin::BImemmove:
  case Builtin::BI__builtin_memmove:
  case Builtin::BI__builtin___memmove_chk:
    return Index == 1;
  default:
    break;
  }
  // A Construct row's argument of the constructed class type is copied into
  // the constructed object.
  CalleeLifecycleRoles Roles = getCalleeLifecycleRoles(FD);
  if (Roles.KnownRole != KnownParamRole::Construct || !Roles.KnownParam ||
      *Roles.KnownParam == Index || *Roles.KnownParam >= FD->getNumParams() ||
      Roles.isEndParam(P))
    return false;
  QualType Constructed =
      rangeValueType(FD->getParamDecl(*Roles.KnownParam)->getType());
  QualType Arg = P->getType().getNonReferenceType();
  return !Constructed.isNull() && Arg->isRecordType() &&
         Constructed.getCanonicalType().getUnqualifiedType() ==
             Arg.getCanonicalType().getUnqualifiedType();
}

SemaProfiles::InitSourceState
SemaProfiles::classifyInitBindingLeaf(const ShapeLeaf &Leaf, QualType T,
                                      const Decl *Body, bool ValueCopy) const {
  const CXXMethodDecl *MD =
      enclosingInstanceMethod(dyn_cast_or_null<DeclContext>(Body));
  UninitAccessOpts Opts =
      UninitBindAccess.withAlias(pointerAliasOf(T))
          .withThisUnderConstruction(isa_and_nonnull<CXXConstructorDecl>(MD))
          .withArms(Leaf.Arms)
          .withUninitMemberTrust(ValueCopy);
  switch (classifyUninitSource(getASTContext(), Leaf.E, T, Opts)) {
  case UninitStorage::Initialized:
    return InitSourceState::Initialized;
  case UninitStorage::Uninitialized:
    return InitSourceState::Uninitialized;
  case UninitStorage::Unknown:
    return InitSourceState::Unknown;
  case UninitStorage::Mixed:
    return InitSourceState::Mixed;
  }
  llvm_unreachable("unknown UninitStorage");
}

SemaProfiles::InitSourceState
SemaProfiles::classifyInitHandleLeaf(const HandleLeaf &Leaf,
                                     const Decl *Body) const {
  const CXXMethodDecl *MD =
      enclosingInstanceMethod(dyn_cast_or_null<DeclContext>(Body));
  UninitAccessOpts Opts =
      UninitBindAccess
          .withThisUnderConstruction(isa_and_nonnull<CXXConstructorDecl>(MD))
          .withArms(Leaf.Arms);
  switch (classifyHandleLeaf(getASTContext(), Leaf, Opts)) {
  case UninitStorage::Initialized:
    return InitSourceState::Initialized;
  case UninitStorage::Uninitialized:
    return InitSourceState::Uninitialized;
  case UninitStorage::Unknown:
    return InitSourceState::Unknown;
  case UninitStorage::Mixed:
    return InitSourceState::Mixed;
  }
  llvm_unreachable("unknown UninitStorage");
}

SemaProfiles::InitSourceState
SemaProfiles::classifyInitAccessLeaf(const ShapeLeaf &Leaf, InitAccessKind Kind,
                                     const Decl *Body) const {
  const CXXMethodDecl *MD =
      enclosingInstanceMethod(dyn_cast_or_null<DeclContext>(Body));
  UninitAccessOpts Opts =
      (Kind == InitAccessKind::Read ? UninitReadAccess : UninitWriteAccess)
          .withThisUnderConstruction(isa_and_nonnull<CXXConstructorDecl>(MD))
          .withArms(Leaf.Arms);
  switch (glvalueDenotesUninitStorage(getASTContext(), Leaf.E, Opts)) {
  case UninitStorage::Initialized:
    return InitSourceState::Initialized;
  case UninitStorage::Uninitialized:
    return InitSourceState::Uninitialized;
  case UninitStorage::Unknown:
    return InitSourceState::Unknown;
  case UninitStorage::Mixed:
    return InitSourceState::Mixed;
  }
  llvm_unreachable("unknown UninitStorage");
}

const Expr *SemaProfiles::peelBaseCasts(const Expr *E, BasePath &Path,
                                        ArrayRef<const Expr *> Arms) {
  SmallVector<const CastExpr *, 4> Casts;
  E = E->IgnoreParens();
  while (true) {
    if (const auto *FE = dyn_cast<FullExpr>(E)) {
      E = FE->getSubExpr()->IgnoreParens();
      continue;
    }
    if (const auto *CE = dyn_cast<CastExpr>(E)) {
      if (isa<ExplicitCastExpr>(CE)) {
        const Expr *Sub = CE->getSubExpr();
        if (!Sub->getType()->isPointerType() && !Sub->isGLValue())
          break;
      }
      Casts.push_back(CE);
      E = CE->getSubExpr()->IgnoreParens();
      continue;
    }
    if (const Expr *Op = passThroughOperand(E, Arms)) {
      E = Op->IgnoreParens();
      continue;
    }
    break;
  }
  // The innermost cast applies first, and this step's casts sit closer to
  // the object than the caller's.
  BasePath Steps;
  for (const CastExpr *CE : llvm::reverse(Casts))
    if (CE->getCastKind() == CK_DerivedToBase ||
        CE->getCastKind() == CK_UncheckedDerivedToBase)
      for (const CXXBaseSpecifier *BS : CE->path())
        Steps.push_back(
            BS->getType()->getAsCXXRecordDecl()->getCanonicalDecl());
  Path.insert(Path.begin(), Steps.begin(), Steps.end());
  return E;
}

std::optional<SemaProfiles::FlowLeafShape>
SemaProfiles::flowLeafShape(const Expr *E, ArrayRef<const Expr *> Arms) {
  FlowLeafShape S;
  // The named steps, leaf first, and how many of them lie on the leaf side
  // of the outermost element step seen so far.
  MemberPath LeafFirst;
  unsigned NamedBelowElement = 0;
  // The casts peeled below a step belong to the object the step accesses:
  // the latest named step's, or the leaf's own before any.
  auto Bases = [&]() -> BasePath & {
    return LeafFirst.empty() ? S.LeafBases : LeafFirst.back().Bases;
  };
  const Expr *Cur = peelBaseCasts(E, Bases(), Arms);
  // Step through a member call deriving a pointer or reference from a
  // handle (isHandleDerefCall) to the variable its chain bottoms out at
  // (handleChainBase): a handle reaches its referent, a pointer -- a
  // std-table handle built over it -- its referent as *p does, and storage
  // is reached at an element; an element call or a handle past its
  // referent's start names an element, and a prefix (first(n)) the referent.
  auto StepThroughHandleObject = [&](const Expr *Obj, bool Element) {
    bool Partial = false, PastStart = false;
    const DeclRefExpr *Base = handleChainBase(Obj, Arms, Partial, &PastStart);
    if (!Base)
      return false;
    QualType BT = Base->getType();
    S.ThroughPointer = BT->isPointerType() || isHandleType(BT);
    S.ThroughHandle = isHandleType(BT);
    if (Element || PastStart || !S.ThroughPointer) {
      S.Subscript = true;
      NamedBelowElement = LeafFirst.size();
    }
    Cur = Base;
    return true;
  };
  auto StepThroughHandle = [&](const Expr *Call) {
    const Expr *Obj;
    bool Element;
    return isHandleDerefCall(Call, Obj, Element) &&
           StepThroughHandleObject(Obj, Element);
  };
  // Step through a std::array's begin or data on an object to the array,
  // at an element, as through a decayed array.
  auto StepThroughArray = [&](const Expr *Call) {
    bool PastStart;
    const Expr *Arr = stdArrayAddressObject(Call, PastStart);
    if (!Arr || Arr->getType()->isPointerType())
      return false;
    S.Subscript = true;
    NamedBelowElement = LeafFirst.size();
    Cur = peelBaseCasts(Arr, Bases(), Arms);
    return true;
  };
  // Step through the pointer value Cur: a std::array's begin or data, or a
  // Derive call (std::begin, std::data, std::next) on an array, continues the
  // chain at an element (true); a Derive call on a pointer stands for the
  // pointer, on a handle for the handle's referent, and an offsetting one
  // names an element; any other pointer reaches its storage through the
  // pointer, which ends the chain.
  auto StepPointerValue = [&]() {
    while (true) {
      if (StepThroughArray(Cur))
        return true;
      bool Partial = false;
      const auto *CE = dyn_cast<CallExpr>(Cur);
      const Expr *Arg = CE ? derivedArgument(CE, Partial) : nullptr;
      if (!Arg)
        break;
      if (Partial) {
        S.Subscript = true;
        NamedBelowElement = LeafFirst.size();
      }
      QualType AT = Arg->getType();
      if (AT->isPointerType()) {
        Cur = peelBaseCasts(Arg, Bases(), Arms);
        continue;
      }
      if (AT->isArrayType() || isStdArrayType(AT)) {
        S.Subscript = true;
        NamedBelowElement = LeafFirst.size();
        Cur = peelBaseCasts(Arg, Bases(), Arms);
        return true;
      }
      S.ThroughPointer = true;
      if (isHandleType(AT))
        StepThroughHandleObject(Arg, /*Element=*/false);
      return false;
    }
    S.ThroughPointer = true;
    StepThroughHandle(Cur);
    return false;
  };
  // Step to the pointer operand of a * or ->: an address-of step's operand
  // continues the chain (true), and so may the pointer's value
  // (StepPointerValue).
  auto StepThroughPointer = [&](const Expr *Ptr) {
    Cur = peelBaseCasts(Ptr, Bases(), Arms);
    if (const Expr *Op = addressOfOperand(Cur)) {
      Cur = peelBaseCasts(Op, Bases(), Arms);
      return true;
    }
    return StepPointerValue();
  };
  while (true) {
    if (const auto *ME = dyn_cast<MemberExpr>(Cur)) {
      const auto *F = dyn_cast<FieldDecl>(ME->getMemberDecl());
      if (!F)
        return std::nullopt;
      if (!F->isAnonymousStructOrUnion())
        LeafFirst.push_back({BasePath(), F});
      if (!ME->isArrow()) {
        Cur = peelBaseCasts(ME->getBase(), Bases(), Arms);
        continue;
      }
      if (StepThroughPointer(ME->getBase()))
        continue;
      break;
    }
    // a.*pm, p->*pm: a member step that names no member.
    if (const BinaryOperator *BO = asDataMemberPointerAccess(Cur)) {
      LeafFirst.push_back({BasePath(), nullptr});
      if (BO->getOpcode() == BO_PtrMemD) {
        Cur = peelBaseCasts(BO->getLHS(), Bases(), Arms);
        continue;
      }
      if (StepThroughPointer(BO->getLHS()))
        continue;
      break;
    }
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Cur)) {
      S.Subscript = true;
      NamedBelowElement = LeafFirst.size();
      Cur = peelBaseCasts(ASE->getBase(), Bases(), Arms);
      // p[i] indexes the pointer's value; arr[i] the array glvalue its
      // peeled decay leaves behind.
      if (Cur->getType()->isPointerType()) {
        if (StepPointerValue())
          continue;
        break;
      }
      continue;
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(Cur);
        UO && UO->getOpcode() == UO_Deref) {
      if (StepThroughPointer(UO->getSubExpr()))
        continue;
      break;
    }
    if (Cur->isGLValue())
      StepThroughHandle(Cur);
    break;
  }
  S.Members.assign(LeafFirst.rbegin(), LeafFirst.rend());
  S.ElementFreePrefix =
      S.Subscript ? LeafFirst.size() - NamedBelowElement : LeafFirst.size();
  // A pointer member of the current object, reached through members by value.
  if (S.ThroughPointer && isa<MemberExpr>(Cur)) {
    std::optional<FlowLeafShape> Ptr = flowLeafShape(Cur, Arms);
    if (!Ptr || !Ptr->IsThis || !Ptr->PointerMembers.empty() ||
        Ptr->Subscript || Ptr->Members.empty() || !Ptr->LeafBases.empty() ||
        llvm::any_of(Ptr->Members,
                     [](const MemberStep &M) { return !M.Field; }))
      return std::nullopt;
    S.IsThis = true;
    S.PointerMembers = std::move(Ptr->Members);
    return S;
  }
  if (isa<CXXThisExpr>(Cur)) {
    if (!S.ThroughPointer)
      return std::nullopt;
    S.IsThis = true;
    return S;
  }
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Cur))
    if (const auto *V = dyn_cast<VarDecl>(DRE->getDecl())) {
      S.Local = V;
      S.LocalRef = DRE;
      return S;
    }
  return std::nullopt;
}

bool SemaProfiles::hasUserProvidedCtor(const CXXRecordDecl *RD) {
  return llvm::any_of(RD->ctors(), [](const CXXConstructorDecl *C) {
    return C->isUserProvided();
  });
}

/// True if \p T is a built-in scalar (arithmetic or enum) type the flow
/// analysis tracks, std::byte excepted (P4222R2 §4.6).
static bool isFlowTrackedScalarType(QualType T) {
  return (T->isIntegralOrEnumerationType() || T->isFloatingType()) &&
         !T->isStdByteType();
}

/// True if \p F is a member the flow analysis tracks as a scalar: an
/// [[uninit]] member of a tracked scalar type (isFlowTrackedScalarType) with
/// no default member initializer.
static bool isFlowTrackedScalarMember(const FieldDecl *F) {
  return F->hasAttr<UninitAttr>() && F->getDeclName() &&
         !F->hasInClassInitializer() && isFlowTrackedScalarType(F->getType());
}

/// True if \p F is a member the flow analysis tracks as a whole object
/// (SemaProfiles::forEachFlowTrackedMember).
static bool isFlowTrackedWholeMember(const ASTContext &Ctx,
                                     const FieldDecl *F) {
  if (!F->hasAttr<UninitAttr>() || !F->getDeclName() ||
      F->hasInClassInitializer())
    return false;
  QualType T = F->getType();
  if (T->isArrayType()) {
    if (!Ctx.getAsConstantArrayType(T))
      return false;
    T = Ctx.getBaseElementType(T);
    if (isFlowTrackedScalarType(T))
      return true;
  }
  return SemaProfiles::getDefaultInitTrackableClass(T);
}

/// The class of \p F when it is a container the flow-tracked member walk
/// enters (SemaProfiles::forEachFlowTrackedMember); null otherwise.
static const CXXRecordDecl *flowTrackedContainerClass(const FieldDecl *F) {
  if (!F->getDeclName() || F->hasAttr<UninitAttr>() ||
      F->hasInClassInitializer() || F->getType()->isArrayType())
    return nullptr;
  return SemaProfiles::getDefaultInitTrackableClass(F->getType());
}

namespace {
/// The depth-first walk of SemaProfiles::forEachFlowTrackedMember.
struct FlowTrackedMemberWalk {
  using MemberStep = SemaProfiles::MemberStep;
  /// The most member steps a path has.
  static constexpr unsigned MaxDepth = 8;
  /// The paths visited and containers entered after which no further
  /// container is entered.
  static constexpr unsigned MaxPaths = 64;
  const ASTContext &Ctx;
  llvm::function_ref<void(ArrayRef<MemberStep>, bool)> Visit;
  /// The steps to the object being walked.
  SemaProfiles::MemberPath Prefix;
  /// The records on the walk's stack.
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> InProgress;
  /// The paths visited and containers entered so far.
  unsigned Paths = 0;

  /// Visit the members of the object of class \p RD reached through
  /// \p Prefix and the derived-to-base path \p Bases.
  void walk(const CXXRecordDecl *RD, SemaProfiles::BasePath &Bases) {
    if (!InProgress.insert(RD).second)
      return;
    walkFields(RD, Bases);
    for (const CXXBaseSpecifier &BS : RD->bases()) {
      if (BS.isVirtual())
        continue;
      const CXXRecordDecl *BRD = BS.getType()->getAsCXXRecordDecl();
      if (!BRD || !BRD->hasDefinition() ||
          SemaProfiles::hasUserProvidedCtor(BRD->getDefinition()))
        continue;
      Bases.push_back(BRD->getCanonicalDecl());
      walk(BRD->getDefinition(), Bases);
      Bases.pop_back();
    }
    InProgress.erase(RD);
  }

  /// Visit the fields of \p RD as walk does; an anonymous struct's fields
  /// are the enclosing object's (its step is transparent), and an anonymous
  /// union is never entered.
  void walkFields(const RecordDecl *RD, SemaProfiles::BasePath &Bases) {
    for (const FieldDecl *F : RD->fields()) {
      if (F->isAnonymousStructOrUnion()) {
        if (const RecordDecl *Anon = F->getType()->getAsRecordDecl();
            Anon && !Anon->isUnion())
          walkFields(Anon, Bases);
        continue;
      }
      bool Scalar = isFlowTrackedScalarMember(F);
      if (Scalar || isFlowTrackedWholeMember(Ctx, F)) {
        Prefix.push_back({Bases, F});
        Visit(Prefix, /*Whole=*/!Scalar);
        ++Paths;
        Prefix.pop_back();
        continue;
      }
      const CXXRecordDecl *Inner = flowTrackedContainerClass(F);
      if (!Inner || Prefix.size() + 1 >= MaxDepth || Paths >= MaxPaths)
        continue;
      ++Paths;
      Prefix.push_back({Bases, F});
      SemaProfiles::BasePath InnerBases;
      walk(Inner, InnerBases);
      Prefix.pop_back();
    }
  }
};
} // namespace

namespace {
/// The depth-first walk of SemaProfiles::forEachMarkedPointerMember.
struct MarkedPointerMemberWalk {
  using MemberStep = SemaProfiles::MemberStep;
  llvm::function_ref<void(ArrayRef<MemberStep>)> Visit;
  SemaProfiles::MemberPath Prefix;
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> InProgress;
  unsigned Paths = 0;

  /// Visit the marked pointer members of the object of class \p RD reached
  /// through \p Prefix and the derived-to-base path \p Bases.
  void walk(const CXXRecordDecl *RD, SemaProfiles::BasePath &Bases) {
    if (!InProgress.insert(RD).second)
      return;
    walkFields(RD, Bases);
    for (const CXXBaseSpecifier &BS : RD->bases()) {
      if (BS.isVirtual())
        continue;
      const CXXRecordDecl *BRD = BS.getType()->getAsCXXRecordDecl();
      if (!BRD || !BRD->hasDefinition())
        continue;
      Bases.push_back(BRD->getCanonicalDecl());
      walk(BRD->getDefinition(), Bases);
      Bases.pop_back();
    }
    InProgress.erase(RD);
  }

  void walkFields(const RecordDecl *RD, SemaProfiles::BasePath &Bases) {
    for (const FieldDecl *F : RD->fields()) {
      if (F->isAnonymousStructOrUnion()) {
        if (const RecordDecl *Anon = F->getType()->getAsRecordDecl();
            Anon && !Anon->isUnion())
          walkFields(Anon, Bases);
        continue;
      }
      if (!F->getDeclName())
        continue;
      QualType T = F->getType();
      if (T->isPointerType() && F->hasAttr<RefToUninitAttr>()) {
        Prefix.push_back({Bases, F});
        Visit(Prefix);
        ++Paths;
        Prefix.pop_back();
        continue;
      }
      const CXXRecordDecl *Inner = T->getAsCXXRecordDecl();
      if (!Inner || T->isReferenceType() || T->isDependentType() ||
          !Inner->hasDefinition() || Inner->isUnion() ||
          Prefix.size() + 1 >= FlowTrackedMemberWalk::MaxDepth ||
          Paths >= FlowTrackedMemberWalk::MaxPaths)
        continue;
      ++Paths;
      Prefix.push_back({Bases, F});
      SemaProfiles::BasePath InnerBases;
      walk(Inner->getDefinition(), InnerBases);
      Prefix.pop_back();
    }
  }
};
} // namespace

void SemaProfiles::forEachMarkedPointerMember(
    const CXXRecordDecl *RD,
    llvm::function_ref<void(ArrayRef<MemberStep>)> Visit) {
  if (!RD || !RD->hasDefinition())
    return;
  MarkedPointerMemberWalk Walk{Visit, {}, {}};
  BasePath Bases;
  Walk.walk(RD->getDefinition(), Bases);
}

ArrayRef<SemaProfiles::MemberPath>
SemaProfiles::markedPointerMembers(const CXXRecordDecl *RD) const {
  if (!RD || !RD->hasDefinition())
    return {};
  RD = RD->getDefinition();
  if (RD->isBeingDefined())
    return {};
  if (auto It = MarkedPointerMemberTables.find(RD);
      It != MarkedPointerMemberTables.end())
    return *It->second;
  auto Table = std::make_unique<SmallVector<MemberPath, 2>>();
  forEachMarkedPointerMember(RD, [&](ArrayRef<MemberStep> Path) {
    Table->emplace_back(Path.begin(), Path.end());
  });
  return *(MarkedPointerMemberTables[RD] = std::move(Table));
}

std::optional<unsigned>
SemaProfiles::findMarkedPointerMember(const CXXRecordDecl *RD,
                                      ArrayRef<MemberStep> Path) const {
  ArrayRef<MemberPath> Table = markedPointerMembers(RD);
  for (unsigned I = 0, N = Table.size(); I != N; ++I)
    if (ArrayRef(Table[I]) == Path)
      return I;
  return std::nullopt;
}

void SemaProfiles::forEachFlowTrackedMember(
    const ASTContext &Ctx, const CXXRecordDecl *RD,
    llvm::function_ref<void(ArrayRef<MemberStep>, bool Whole)> Visit) {
  if (!RD || !RD->hasDefinition())
    return;
  FlowTrackedMemberWalk Walk{Ctx, Visit, {}, {}};
  BasePath Bases;
  Walk.walk(RD->getDefinition(), Bases);
}

ArrayRef<SemaProfiles::FlowTrackedMember>
SemaProfiles::flowTrackedMembers(const CXXRecordDecl *RD) const {
  if (!RD || !RD->hasDefinition())
    return {};
  RD = RD->getDefinition();
  if (RD->isBeingDefined())
    return {};
  std::unique_ptr<SmallVector<FlowTrackedMember, 4>> &Table =
      FlowTrackedMemberTables[RD];
  if (!Table) {
    Table = std::make_unique<SmallVector<FlowTrackedMember, 4>>();
    forEachFlowTrackedMember(
        getASTContext(), RD, [&](ArrayRef<MemberStep> Path, bool Whole) {
          Table->push_back({MemberPath(Path.begin(), Path.end()), Whole});
        });
  }
  return *Table;
}

std::optional<SemaProfiles::FlowMemberMatch>
SemaProfiles::matchFlowTrackedMember(const CXXRecordDecl *RD,
                                     const FlowLeafShape &Shape) const {
  ArrayRef<MemberStep> Steps =
      ArrayRef(Shape.Members).take_front(Shape.ElementFreePrefix);
  if (Steps.empty())
    return std::nullopt;
  ArrayRef<FlowTrackedMember> Table = flowTrackedMembers(RD);
  for (unsigned I = 0, N = Table.size(); I != N; ++I) {
    ArrayRef<MemberStep> Path = Table[I].first;
    if (Path.size() <= Steps.size() && Steps.take_front(Path.size()) == Path)
      return FlowMemberMatch{I, (unsigned)Path.size()};
  }
  return std::nullopt;
}

std::pair<unsigned, unsigned> SemaProfiles::flowTrackedMemberRange(
    const CXXRecordDecl *RD, ArrayRef<MemberStep> Prefix,
    ArrayRef<const CXXRecordDecl *> LeafBases) const {
  ArrayRef<FlowTrackedMember> Table = flowTrackedMembers(RD);
  std::optional<unsigned> First;
  unsigned Last = 0;
  for (unsigned I = 0, N = Table.size(); I != N; ++I) {
    ArrayRef<MemberStep> Path = Table[I].first;
    if (Path.size() <= Prefix.size() ||
        Path.take_front(Prefix.size()) != Prefix)
      continue;
    ArrayRef<const CXXRecordDecl *> Next = Path[Prefix.size()].Bases;
    if (Next.size() < LeafBases.size() ||
        Next.take_front(LeafBases.size()) != LeafBases)
      continue;
    if (!First)
      First = I;
    Last = I + 1;
  }
  return First ? std::pair{*First, Last} : std::pair{0u, 0u};
}

const CXXRecordDecl *SemaProfiles::getTrackableSlotClass(QualType T) {
  if (T->isReferenceType() || T->isDependentType())
    return nullptr;
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return nullptr;
  RD = RD->getDefinition();
  if (RD->isUnion() || RD->isDependentType() || hasUserProvidedCtor(RD))
    return nullptr;
  return RD;
}

const CXXRecordDecl *SemaProfiles::getDefaultInitTrackableClass(QualType T) {
  if (T->isReferenceType() || T->isDependentType())
    return nullptr;
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return nullptr;
  RD = RD->getDefinition();
  if (RD->isUnion() || RD->isDependentType() ||
      RD->hasUserProvidedDefaultConstructor())
    return nullptr;
  return RD;
}

const CXXRecordDecl *SemaProfiles::getTrackableLocalClass(const VarDecl *V) {
  if (!V->hasLocalStorage() || isa<ParmVarDecl>(V) || isa<DecompositionDecl>(V))
    return nullptr;
  if (V->isInvalidDecl() || V->hasAttr<UninitAttr>())
    return nullptr;
  return getTrackableSlotClass(V->getType());
}

const CXXRecordDecl *SemaProfiles::flowTrackedAggregateClass(const VarDecl *V) {
  if (!V->hasLocalStorage() || isa<DecompositionDecl>(V) ||
      V->isInvalidDecl() || V->hasAttr<UninitAttr>())
    return nullptr;
  QualType T = V->getType();
  if (T->isReferenceType() || T->isDependentType())
    return nullptr;
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return nullptr;
  RD = RD->getDefinition();
  return RD->isDependentType() ? nullptr : RD;
}

/// True if \p DC lies inside a function body -- the one place the std::init
/// CFG pass runs; a namespace-scope initializer, a default member
/// initializer (parsed in the class), or a default argument has no flow.
static bool insideFunctionBody(const DeclContext *DC) {
  while (DC && !DC->isFunctionOrMethod())
    DC = DC->getParent();
  return DC != nullptr;
}

const CXXMethodDecl *
SemaProfiles::enclosingInstanceMethod(const DeclContext *DC) {
  while (DC) {
    if (const auto *MD = dyn_cast<CXXMethodDecl>(DC)) {
      if (MD->getParent()->isLambda()) {
        DC = MD->getParent()->getDeclContext();
        continue;
      }
      return MD->isStatic() ? nullptr : MD;
    }
    if (isa<BlockDecl, CapturedDecl, EnumDecl, RequiresExprBodyDecl>(DC)) {
      DC = DC->getParent();
      continue;
    }
    return nullptr;
  }
  return nullptr;
}

bool SemaProfiles::isFlowTrackedLeaf(const ShapeLeaf &Leaf, bool AsPointerValue,
                                     const DeclContext *CurContext,
                                     FlowLeafUse Use) const {
  if (!insideFunctionBody(CurContext))
    return false;
  // The current object as a whole, outside its constructors.
  auto CurrentObjectTracked = [&] {
    const CXXMethodDecl *MD = enclosingInstanceMethod(CurContext);
    return Use != FlowLeafUse::Plain && MD && !isa<CXXConstructorDecl>(MD);
  };
  const Expr *E = ignoreTransparentCasts(Leaf.E, Leaf.Arms);
  if (AsPointerValue) {
    if (const Expr *Op = addressOfOperand(E))
      return isFlowTrackedLeaf(ShapeLeaf(Op, Leaf.Arms),
                               /*AsPointerValue=*/false, CurContext, Use);
    // A decayed array is the array it decays from.
    if (E->getType()->isArrayType())
      return isFlowTrackedLeaf(ShapeLeaf(E, Leaf.Arms),
                               /*AsPointerValue=*/false, CurContext, Use);
    if (isa<CXXThisExpr>(E))
      return CurrentObjectTracked();
    // A std::array's begin or data: the array it points into, or the
    // pointer's referent through which it is called.
    bool PastStart;
    if (const Expr *Arr = stdArrayAddressObject(E, PastStart))
      return isFlowTrackedLeaf(ShapeLeaf(Arr, Leaf.Arms),
                               Arr->getType()->isPointerType(), CurContext,
                               Use);
    // A Derive call: its argument, an array or a range as storage, a
    // pointer as a value, or a range's handle state.
    if (const auto *CE = dyn_cast<CallExpr>(E)) {
      bool Partial;
      if (const Expr *Arg = derivedArgument(CE, Partial)) {
        QualType AT = Arg->getType();
        if (AT->isPointerType())
          return isFlowTrackedLeaf(ShapeLeaf(Arg, Leaf.Arms),
                                   /*AsPointerValue=*/true, CurContext, Use);
        if (isFlowTrackedLeaf(ShapeLeaf(Arg, Leaf.Arms),
                              /*AsPointerValue=*/false, CurContext, Use))
          return true;
        bool ChainPartial = false;
        const DeclRefExpr *Base = handleChainBase(Arg, Leaf.Arms, ChainPartial);
        return Base &&
               isFlowTrackedHandleVar(dyn_cast<VarDecl>(Base->getDecl()));
      }
    }
    // A pointer a tracked handle's member returns to its whole referent
    // (up.get(), s.data()) or through a prefix of it (s.first(n).data());
    // an element's, or one past the referent's start, is never tracked.
    const Expr *Obj;
    bool Element;
    if (E->getType()->isPointerType() && isHandleDerefCall(E, Obj, Element)) {
      bool Partial = false, PastStart = false;
      const DeclRefExpr *Base =
          handleChainBase(Obj, Leaf.Arms, Partial, &PastStart);
      return !Element && !PastStart && Base &&
             isFlowTrackedHandleVar(dyn_cast<VarDecl>(Base->getDecl()));
    }
    // The value of a marked pointer member of the current object.
    if (E->getType()->isPointerType() && isa<MemberExpr>(E)) {
      std::optional<FlowLeafShape> Ptr = flowLeafShape(E, Leaf.Arms);
      const CXXMethodDecl *MD = enclosingInstanceMethod(CurContext);
      return Ptr && Ptr->IsThis && Ptr->PointerMembers.empty() &&
             !Ptr->Subscript && Ptr->LeafBases.empty() && MD &&
             findMarkedPointerMember(MD->getParent(), Ptr->Members);
    }
    // The value of a marked pointer.
    const auto *VD = dyn_cast_or_null<VarDecl>(getDirectlyNamedDecl(E));
    return VD && VD->hasLocalStorage() && VD->getType()->isPointerType() &&
           VD->hasAttr<RefToUninitAttr>();
  }
  std::optional<FlowLeafShape> Shape = flowLeafShape(E, Leaf.Arms);
  if (!Shape)
    return false;
  if (Shape->IsThis) {
    const CXXMethodDecl *MD = enclosingInstanceMethod(CurContext);
    // *elem, elem->m: a marked pointer member's referent or a subobject of
    // it; an element (elem[i]) is never tracked.
    if (!Shape->PointerMembers.empty())
      return MD && !Shape->Subscript &&
             findMarkedPointerMember(MD->getParent(), Shape->PointerMembers);
    if (Shape->Members.empty() && !Shape->Subscript)
      return CurrentObjectTracked();
    if (!MD)
      return false;
    if (matchFlowTrackedMember(MD->getParent(), *Shape))
      return true;
    // In a constructor body, a subobject enclosing tracked members, as for a
    // local.
    const auto *Ctor =
        dyn_cast_or_null<CXXConstructorDecl>(flowAnalysisOwner(CurContext));
    if (!Ctor || Ctor->isDelegatingConstructor() ||
        Ctor->getParent()->isUnion())
      return false;
    return isFlowTrackedSubobject(MD->getParent(), E, *Shape, Use);
  }
  const VarDecl *V = Shape->Local;
  if (!V->hasLocalStorage())
    return false;
  // *up, up->m: a marked handle's referent or a subobject of it; an element
  // (s[i], or through an offset handle) is never tracked, as for a pointer.
  if (Shape->ThroughHandle)
    return !Shape->Subscript && isFlowTrackedHandleVar(V);
  // *p, p->m, (*p).m: a marked pointer's referent or a subobject of it; an
  // element (p[i]) is never tracked (P4222R2 §5.4).
  if (Shape->ThroughPointer)
    return V->getType()->isPointerType() && V->hasAttr<RefToUninitAttr>() &&
           !Shape->Subscript;
  // u, u.x, arr[i]: an [[uninit]] local as a whole or a subobject of it.
  if (V->hasAttr<UninitAttr>())
    return true;
  // r, r.m: a marked reference's referent or a subobject of it.
  if (V->hasAttr<RefToUninitAttr>() && V->getType()->isReferenceType())
    return !Shape->Subscript;
  // a.m: a tracked member of an aggregate local or by-value parameter; for a
  // marked target, also a, or a subobject enclosing tracked members.
  const CXXRecordDecl *RD = flowTrackedAggregateClass(V);
  if (!RD)
    return false;
  if (matchFlowTrackedMember(RD, *Shape))
    return true;
  return isFlowTrackedSubobject(RD, E, *Shape, Use);
}

bool SemaProfiles::isFlowTrackedSubobject(const CXXRecordDecl *RD,
                                          const Expr *E,
                                          const FlowLeafShape &Shape,
                                          FlowLeafUse Use) const {
  if (Shape.Subscript)
    return false;
  // A value copy through a pointer to data member reads every tracked member
  // the step may name: at any depth on the current object, as the last step
  // on a local.
  auto Opaque = llvm::find_if(Shape.Members,
                              [](const MemberStep &S) { return !S.Field; });
  if (Opaque != Shape.Members.end()) {
    if (Use != FlowLeafUse::ValueCopy ||
        (!Shape.IsThis && Opaque + 1 != Shape.Members.end()))
      return false;
    std::pair<unsigned, unsigned> Range = flowTrackedMemberRange(
        RD, ArrayRef(Shape.Members.begin(), Opaque), Opaque->Bases);
    return Range.first != Range.second;
  }
  if (Use == FlowLeafUse::MarkedTarget) {
    // An object whose default-initialization runs code was constructed
    // whole.
    const CXXRecordDecl *Named = E->getType()->getAsCXXRecordDecl();
    if (!Named || !Named->hasDefinition() ||
        !Named->hasTrivialDefaultConstructor())
      return false;
  } else if (Use != FlowLeafUse::ValueCopy || Shape.Members.empty()) {
    return false;
  }
  std::pair<unsigned, unsigned> Range =
      flowTrackedMemberRange(RD, Shape.Members, Shape.LeafBases);
  return Range.first != Range.second;
}

bool SemaProfiles::isFlowTrackedHandleLeaf(
    const HandleLeaf &L, const DeclContext *CurContext) const {
  if (L.M != HandleLeaf::Mode::Handle || !insideFunctionBody(CurContext))
    return false;
  const auto *DRE = dyn_cast<DeclRefExpr>(L.E->IgnoreParenImpCasts());
  return DRE && isFlowTrackedHandleVar(dyn_cast<VarDecl>(DRE->getDecl()));
}

const Expr *SemaProfiles::bindingSourceOperand(const Expr *Src, QualType T,
                                               bool &AsPointerValue) {
  AsPointerValue = !T->isReferenceType() ||
                   pointerAliasKind(T) == PointerAliasKind::ReadOnly;
  if (AsPointerValue)
    return Src;
  const auto *MTE =
      dyn_cast<MaterializeTemporaryExpr>(ignoreParenImpCastsKeepMTE(Src));
  if (!MTE)
    return Src;
  if (!MTE->getType()->isPointerType())
    return nullptr;
  AsPointerValue = true;
  return MTE->getSubExpr();
}

bool SemaProfiles::hasFlowTrackedGlvalueLeaf(
    const Expr *G, SmallVectorImpl<const Expr *> *Leaves) const {
  if (!G)
    return false;
  bool Any = false;
  forEachTargetLeaf(G, [&](const ShapeLeaf &Leaf) {
    if (!isFlowTrackedLeaf(Leaf, /*AsPointerValue=*/false, SemaRef.CurContext))
      return;
    Any = true;
    if (Leaves && !llvm::is_contained(*Leaves, Leaf.E))
      Leaves->push_back(Leaf.E);
  });
  return Any;
}

bool SemaProfiles::hasFlowTrackedLeaf(const Expr *Src, QualType T,
                                      SmallVectorImpl<const Expr *> *Leaves,
                                      FlowLeafUse Use, bool AsRange) const {
  if (!Src || T.isNull())
    return false;
  bool Any = false;
  auto Collect = [&](const Expr *E, bool AsPointerValue) {
    forEachTargetLeaf(E, [&](const ShapeLeaf &Leaf) {
      if (!isFlowTrackedLeaf(Leaf, AsPointerValue, SemaRef.CurContext, Use))
        return;
      Any = true;
      if (Leaves && !llvm::is_contained(*Leaves, Leaf.E))
        Leaves->push_back(Leaf.E);
    });
  };
  // A handle's tracked leaves are those of the storage its walk reaches; a
  // range's storage comes first, and an untracked range's handle state is
  // walked in turn.
  if (isHandleType(T) || AsRange) {
    HandleWalkMemo Memo;
    auto Visit = [&](auto &Self, const HandleLeaf &L) -> void {
      switch (L.M) {
      case HandleLeaf::Mode::Handle:
        if (isFlowTrackedHandleLeaf(L, SemaRef.CurContext)) {
          Any = true;
          if (Leaves && !llvm::is_contained(*Leaves, L.E))
            Leaves->push_back(L.E);
        }
        return;
      case HandleLeaf::Mode::PointerValue:
      case HandleLeaf::Mode::Storage:
        Collect(L.E, L.M == HandleLeaf::Mode::PointerValue);
        return;
      case HandleLeaf::Mode::Range:
        forEachTargetLeaf(L.E, [&](const ShapeLeaf &Leaf) {
          if (isFlowTrackedLeaf(Leaf, /*AsPointerValue=*/false,
                                SemaRef.CurContext, Use)) {
            Any = true;
            if (Leaves && !llvm::is_contained(*Leaves, Leaf.E))
              Leaves->push_back(Leaf.E);
            return;
          }
          forEachHandleSourceLeaf(
              Leaf.E, [&](const HandleLeaf &Inner) { Self(Self, Inner); },
              &Memo);
        });
        return;
      }
    };
    if (AsRange)
      Visit(Visit, HandleLeaf{Src, HandleLeaf::Mode::Range});
    else
      forEachHandleSourceLeaf(
          Src, [&](const HandleLeaf &L) { Visit(Visit, L); }, &Memo);
    return Any;
  }
  bool AsPointerValue;
  Src = bindingSourceOperand(Src, T, AsPointerValue);
  if (!Src)
    return false;
  Collect(Src, AsPointerValue);
  return Any;
}

void SemaProfiles::checkInitProfileBinding(InitBindingKind Kind,
                                           SourceLocation Loc,
                                           const ValueDecl *Target, QualType T,
                                           const Expr *Src, const Decl *D) {
  // See inRebuiltOrSynthesizedCode.
  if (!isProfileLive("std::init") || T.isNull() || T->isDependentType() ||
      (!T->isPointerType() && !T->isReferenceType() && !isHandleType(T)) ||
      inRebuiltOrSynthesizedCode())
    return;
  const auto *Parm = dyn_cast_or_null<ParmVarDecl>(Target);
  const auto *Callee =
      Parm ? dyn_cast<FunctionDecl>(Parm->getDeclContext()) : nullptr;
  // A builtin that never evaluates its arguments binds nothing, nor does a
  // call the checks see through (bindsNoArgument).
  if (unsigned ID = Callee ? Callee->getBuiltinID() : 0;
      ID && getASTContext().BuiltinInfo.isUnevaluated(ID))
    return;
  if (Callee && bindsNoArgument(Callee))
    return;
  // The callee's lifecycle roles (see CalleeLifecycleRoles).
  CalleeLifecycleRoles Roles =
      Callee ? getCalleeLifecycleRoles(Callee) : CalleeLifecycleRoles();
  bool TargetMarked =
      Target && bindingTargetCanCarryMarker(Kind) &&
      (Parm ? Roles.isMarkedParam(Parm) : Target->hasAttr<RefToUninitAttr>());
  // An initializing parameter or a range's end iterator accepts storage in
  // any state; a transfer parameter's storage is judged where the handle
  // under construction lands, and a handle an assignment takes by value
  // where it is assigned (checkInitProfileHandleReseat).
  if (Parm && (Roles.isInitializingParam(Parm) || Roles.isEndParam(Parm) ||
               Roles.transfersParam(Parm) ||
               (isHandleType(T) && isAssignmentSource(Parm))))
    return;
  // A role parameter taking a class-type range by reference judges the
  // range, not the range object's storage.
  bool AsRange = Parm && T->isReferenceType() &&
                 isHandleType(T.getNonReferenceType()) &&
                 Roles.roleOf(Parm) != KnownParamRole::None;
  bool Destroys = Parm && Roles.destroysParam(Parm);
  if (Destroys || (Parm && Roles.releasesParam(Parm, /*TrustedOnly=*/false))) {
    // A [[now_uninit]] callee's pointer, reference, or handle parameter -- and
    // a known storage-release callee's (free, realloc's pointer, replaceable
    // global operator delete), which is [[now_uninit]]-equivalent here --
    // accepts storage in any live state instead of the marker-consistency
    // check: the attribute declares destruction, whose operand is initialized
    // memory for a plain destructor-like callee and *any* state for a
    // raw-release one (free takes storage that may never have been
    // constructed), and initialized storage is precisely what a dual-attributed
    // reinitializer's destroy half exists for. Acceptance never diagnoses, so
    // it reads the union of the release bits -- an untrusted name-only free
    // still relaxes. Two states a [[now_uninit]] callee must not take: storage
    // already destroyed (P4222R2 §1's double_destroy, a reinitializer included
    // -- its destroy half is invalid on destroyed storage; construct_at is the
    // sanctioned recovery path) and storage still or again uninitialized
    // (destroy_uninit: destruction makes an object uninitialized, so a first
    // destroy of never-constructed storage is as much an access to raw memory
    // as a second one, "Lifetimes", p4222r2.md:922-927). Both key on the
    // destroy role alone: a storage-release callee is exempt from both
    // (destroy_at(p); free(p); is correct, and free takes never-constructed
    // storage by contract), a reinitializer from destroy_uninit call-wide (its
    // marked parameter positively legalizes uninitialized sources; its unmarked
    // destroy-only parameters ride along -- a missed diagnostic, not a rule),
    // and a parameter carrying [[ref_to_uninit]] per parameter (the annotation
    // spelling for an unrecognized storage-release function such as
    // _aligned_free; ProfilesFrameworkInternals.rst, "Known Gaps (std::init)").
    // A release callee's argument is a Kill or nothing in the CFG pass, never a
    // judgment.
    if (Destroys)
      checkInitProfileDestroy(
          Loc, T, Src, D,
          /*DestroyUninitApplies=*/!Roles.InitializesRefToUninitParams &&
              !TargetMarked,
          InitDestroyer::Function, TargetMarked, AsRange);
  } else {
    judgeInitProfileBinding(
        Kind, Loc, TargetMarked, T, Src, D,
        /*Subject=*/nullptr, /*AfterFlowPass=*/nullptr,
        /*ValueCopy=*/!TargetMarked && isValueCopySource(Parm), AsRange);
    if (!TargetMarked && !AsRange && bindsHandleByReference(Kind, T, Target))
      judgeInitProfileBinding(Kind, Loc, /*TargetMarked=*/false,
                              T.getNonReferenceType().getUnqualifiedType(), Src,
                              D);
  }
}

void SemaProfiles::checkInitProfileDestroy(SourceLocation Loc, QualType T,
                                           const Expr *Src, const Decl *D,
                                           bool DestroyUninitApplies,
                                           InitDestroyer Destroyer,
                                           bool TargetMarked, bool AsRange) {
  // An instantiation-dependent source defers exactly like
  // judgeInitProfileBinding's; see also inRebuiltOrSynthesizedCode.
  if (!Src || isa<RecoveryExpr>(Src->IgnoreParens()) ||
      (!D && Src->isInstantiationDependent()) || inRebuiltOrSynthesizedCode())
    return;
  if (SmallVector<const Expr *, 2> Leaves; hasFlowTrackedLeaf(
          Src, T, &Leaves,
          TargetMarked ? FlowLeafUse::MarkedTarget : FlowLeafUse::Destroy,
          AsRange)) {
    DeferredInitSite Site{DeferredInitSite::Kind::Destroy,
                          InitBindingKind::Parameter,
                          Loc,
                          DestroyUninitApplies,
                          T,
                          Src,
                          /*Subject=*/nullptr,
                          std::move(Leaves)};
    Site.Destroyer = Destroyer;
    Site.AsRange = AsRange;
    recordDeferredInitSite(std::move(Site));
    return;
  }
  if (DestroyUninitApplies)
    judgeDestroyUninitByForm(Loc, T, Src, D, /*AfterFlowPass=*/nullptr,
                             Destroyer, AsRange);
}

void SemaProfiles::judgeDestroyUninitByForm(SourceLocation Loc, QualType T,
                                            const Expr *Src, const Decl *D,
                                            const Decl *AfterFlowPass,
                                            InitDestroyer Destroyer,
                                            bool AsRange) {
  if (shouldEmitProfileViolation("std::init", diag::err_init_destroy_uninit,
                                 Loc, D, /*PostParse=*/AfterFlowPass) &&
      isUninitializedOrMixed(
          classifyUninitSource(getASTContext(), Src, T,
                               UninitBindAccess.withThisUnderConstruction(
                                   thisIsUnderConstruction(AfterFlowPass)),
                               AsRange)))
    Diag(Loc, diag::err_init_destroy_uninit)
        << "std::init" << static_cast<unsigned>(Destroyer);
}

void SemaProfiles::checkInitProfileObjectArgument(const Expr *Object,
                                                  const CXXMethodDecl *Method) {
  if (!isProfileLive("std::init") || !Object)
    return;
  if (isa<CXXDestructorDecl>(Method)) {
    checkInitProfileDestructorCall(Object);
    return;
  }
  // A static call operator (C++23) has no implicit object parameter: its
  // object argument is evaluated but its value is never used, exactly like a
  // static member function named through an object -- whose call path never
  // reaches this funnel. BuildCallToObjectOfClassType converts the object
  // argument for static call operators all the same, so skip them here.
  if (Method->isStatic())
    return;
  // A std::array's address members only take its address.
  if (isStdArrayAddressMember(Method))
    return;
  // An arrow call's object argument arrives as the pointer expression, a dot
  // call's as the object glvalue; the implicit object parameter's type is the
  // pointer, or a reference to the object.
  QualType ObjectTy = Object->getType();
  QualType T = ObjectTy->isPointerType()
                   ? ObjectTy
                   : getASTContext().getLValueReferenceType(ObjectTy);
  judgeInitProfileBinding(InitBindingKind::ObjectArgument, Object->getExprLoc(),
                          /*TargetMarked=*/false, T, Object, /*D=*/nullptr,
                          Method);
}

void SemaProfiles::checkInitProfileDestructorCall(const Expr *Object) {
  if (!isProfileLive("std::init") || !Object)
    return;
  // The pointer of an arrow call, or the object glvalue of a dot call bound
  // as a reference, as for checkInitProfileObjectArgument.
  QualType ObjectTy = Object->getType();
  QualType T = ObjectTy->isPointerType()
                   ? ObjectTy
                   : getASTContext().getLValueReferenceType(ObjectTy);
  checkInitProfileDestroy(Object->getExprLoc(), T, Object, /*D=*/nullptr,
                          /*DestroyUninitApplies=*/true,
                          InitDestroyer::DestructorCall);
}

// The read-through diagnostic distinguishes indirection through a
// [[ref_to_uninit]] pointer/reference from a subobject read of a named
// [[uninit]] object, which involves no [[ref_to_uninit]] entity. Approximate
// but sufficient for phrasing: walk up the dot member / array-element chain;
// the read is the latter form iff the chain reaches an [[uninit]]-marked
// member (e.g. the class-type member in this->agg.f) or bottoms out at a
// named [[uninit]] declaration. An arrow access or a subscript on a pointer
// reaches its object through a pointer, so the pointer wording applies from
// there on. The walk need not distinguish field from non-field members: the
// semantic recognizer classifies a non-field member access Initialized, so no
// such chain ever reaches this phrasing helper.
static bool isMemberChainOfUninitObject(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  while (true) {
    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (ME->getMemberDecl()->hasAttr<UninitAttr>())
        return true;
      if (ME->isArrow())
        return false;
      E = ME->getBase()->IgnoreParenImpCasts();
      continue;
    }
    if (const BinaryOperator *BO = SemaProfiles::asDataMemberPointerAccess(E)) {
      if (BO->getOpcode() == BO_PtrMemI)
        return false;
      E = BO->getLHS()->IgnoreParenImpCasts();
      continue;
    }
    // a[i] and *a on an array glvalue (decay stripped below) are subobject
    // accesses like a dot member access; on a pointer base they reach the
    // object through the pointer.
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
      const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
      if (!Base->getType()->isArrayType())
        return false;
      E = Base;
      continue;
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(E);
        UO && UO->getOpcode() == UO_Deref) {
      const Expr *Sub = UO->getSubExpr()->IgnoreParenImpCasts();
      if (!Sub->getType()->isArrayType())
        return false;
      E = Sub;
      continue;
    }
    break;
  }
  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  return DRE && DRE->getDecl()->hasAttr<UninitAttr>();
}

// The write diagnostic's remaining provenance split: uninitialized storage
// reached through a [[ref_to_uninit]] entity (a marked pointer, reference,
// member, or callee return) versus storage with no marker in source (an
// allocator result, a raw new-expression). Approximate but sufficient for
// phrasing, like isMemberChainOfUninitObject above: walk the store target's
// chain; the marker wording applies iff a marked entity appears anywhere
// along it. As there, non-field members need no special handling: the
// semantic recognizer never fires on a chain through one.
static bool uninitWriteChainSeesMarker(const Expr *E) {
  while (true) {
    E = E->IgnoreParenCasts();
    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (ME->getMemberDecl()->hasAttr<RefToUninitAttr>())
        return true;
      E = ME->getBase();
      continue;
    }
    if (const BinaryOperator *BO = SemaProfiles::asDataMemberPointerAccess(E)) {
      E = BO->getLHS();
      continue;
    }
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
      E = ASE->getBase();
      continue;
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(E);
        UO && UO->getOpcode() == UO_Deref) {
      E = UO->getSubExpr();
      continue;
    }
    if (const auto *CE = dyn_cast<CallExpr>(E)) {
      const FunctionDecl *FD = CE->getDirectCallee();
      if (FD && FD->hasAttr<RefToUninitAttr>())
        return true;
      // What a member call derives from a handle is reached through the
      // handle's marker when one of its leaves carries it.
      const Expr *Obj = derivingObject(CE);
      if (!Obj)
        return false;
      bool Marked = false;
      SemaProfiles::forEachHandleSourceLeaf(
          Obj, [&](const SemaProfiles::HandleLeaf &L) {
            if (L.M == SemaProfiles::HandleLeaf::Mode::Handle)
              Marked |= uninitWriteChainSeesMarker(L.E);
          });
      return Marked;
    }
    if (const auto *CCE = dyn_cast<CXXConstructExpr>(E))
      return llvm::any_of(
          CCE->getConstructor()->parameters(),
          [](const ParmVarDecl *P) { return P->hasAttr<RefToUninitAttr>(); });
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return DRE->getDecl()->hasAttr<RefToUninitAttr>();
    return false;
  }
}

void SemaProfiles::checkInitProfileReadThrough(SourceLocation Loc,
                                               const Expr *Glvalue,
                                               QualType ValueType,
                                               const Decl *AfterFlowPass) {
  // Hoisted first rung of the gate: the call site is ungated and the work
  // below is not free.
  if (!isProfileLive("std::init"))
    return;
  // A RecoveryExpr is a placeholder for an expression that already failed, not
  // a read the user wrote, so it must not drive this rule; see also
  // inRebuiltOrSynthesizedCode.
  if (!Glvalue || isa<RecoveryExpr>(Glvalue->IgnoreParens()) ||
      (!AfterFlowPass && inRebuiltOrSynthesizedCode()))
    return;
  // The expression-check template policy (ProfilesFrameworkInternals.rst,
  // "Check Sites").
  if (Glvalue->isInstantiationDependent())
    return;
  // P4222R2 §4.6: reading an uninitialized std::byte is permitted. Reading a
  // std::nullptr_t accesses no storage ([conv.lval]p3).
  if (getASTContext().getBaseElementType(ValueType)->isStdByteType() ||
      ValueType->isNullPtrType())
    return;
  // A read with a flow-tracked leaf is the CFG pass's (its ReadThrough
  // sites), judged against the flow state at its program point.
  if (SmallVector<const Expr *, 2> Leaves;
      !AfterFlowPass && hasFlowTrackedGlvalueLeaf(Glvalue, &Leaves)) {
    recordDeferredInitSite({DeferredInitSite::Kind::ReadThrough,
                            InitBindingKind::Variable, Loc, /*Flag=*/false,
                            ValueType, Glvalue, /*Subject=*/nullptr,
                            std::move(Leaves)});
    return;
  }
  if (!shouldEmitProfileViolation("std::init",
                                  diag::err_init_uninit_read_through, Loc,
                                  /*D=*/nullptr, /*PostParse=*/AfterFlowPass))
    return;
  if (!isUninitializedOrMixed(glvalueDenotesUninitStorage(
          getASTContext(), Glvalue,
          UninitReadAccess
              .withThisUnderConstruction(thisIsUnderConstruction(AfterFlowPass))
              .withUninitMemberTrust(trustsUninitMembers(AfterFlowPass)))))
    return;
  Diag(Loc, diag::err_init_uninit_read_through)
      << "std::init" << (isMemberChainOfUninitObject(Glvalue) ? 1 : 0);
}

void SemaProfiles::checkInitProfileSubobjectWrite(SourceLocation Loc,
                                                  const Expr *LHS,
                                                  const Decl *AfterFlowPass) {
  if (!isProfileLive("std::init"))
    return;
  // A RecoveryExpr is a placeholder for an expression that already failed, not
  // a store the user wrote, so it must not drive this rule; see also
  // inRebuiltOrSynthesizedCode.
  if (!LHS || isa<RecoveryExpr>(LHS->IgnoreParens()) ||
      (!AfterFlowPass && inRebuiltOrSynthesizedCode()))
    return;
  // The expression-check template policy (ProfilesFrameworkInternals.rst,
  // "Check Sites").
  if (LHS->isInstantiationDependent())
    return;
  // P4222R2 §4.6: an uninitialized std::byte may be manipulated freely.
  if (getASTContext().getBaseElementType(LHS->getType())->isStdByteType())
    return;
  // A store with a flow-tracked leaf is the CFG pass's (its SubobjectWrite
  // sites), judged against the flow state at its program point.
  if (SmallVector<const Expr *, 2> Leaves;
      !AfterFlowPass && hasFlowTrackedGlvalueLeaf(LHS, &Leaves)) {
    recordDeferredInitSite({DeferredInitSite::Kind::SubobjectWrite,
                            InitBindingKind::Variable, Loc, /*Flag=*/false,
                            LHS->getType(), LHS, /*Subject=*/nullptr,
                            std::move(Leaves)});
    return;
  }
  if (!shouldEmitProfileViolation("std::init",
                                  diag::err_init_uninit_subobject_write, Loc,
                                  /*D=*/nullptr, /*PostParse=*/AfterFlowPass))
    return;
  if (!isUninitializedOrMixed(glvalueDenotesUninitStorage(
          getASTContext(), LHS,
          UninitWriteAccess
              .withThisUnderConstruction(thisIsUnderConstruction(AfterFlowPass))
              .withUninitMemberTrust(trustsUninitMembers(AfterFlowPass)))))
    return;
  // The provenance phrase: a member chain of a named [[uninit]] object
  // renders exactly as before the marked/allocator arms existed (arm 0);
  // isMemberChainOfUninitObject alone cannot key the split -- it is false
  // for allocator and new-expression provenance, which needs arm 2's
  // marker-free wording.
  const Expr *Target = LHS->IgnoreParenImpCasts();
  Diag(Loc, diag::err_init_uninit_subobject_write)
      << "std::init"
      << !(isa<MemberExpr>(Target) || asDataMemberPointerAccess(Target))
      << (isMemberChainOfUninitObject(LHS)
              ? 0
              : (uninitWriteChainSeesMarker(LHS) ? 1 : 2));
}

std::optional<bool> SemaProfiles::resolveAssignTargetMarking(const Expr *E) {
  std::optional<bool> Marked;
  bool Unknown = false;
  forEachTargetLeaf(E, [&](const ShapeLeaf &Leaf) {
    const ValueDecl *VD = getDirectlyNamedDecl(Leaf.E);
    if (!VD || VD->getType()->isReferenceType() ||
        (Marked && *Marked != VD->hasAttr<RefToUninitAttr>()))
      Unknown = true;
    else
      Marked = VD->hasAttr<RefToUninitAttr>();
  });
  return Unknown ? std::nullopt : Marked;
}

void SemaProfiles::checkInitProfilePointerAssignment(Expr *LHS, Expr *RHS,
                                                     SourceLocation OpLoc) {
  if (!isProfileLive("std::init"))
    return;
  // References cannot be reseated, so only pointer assignment applies.
  if (!LHS->getType()->isPointerType())
    return;
  // An instantiation-dependent LHS (e.g. an unresolved member access) has no
  // readable marker yet -- getDirectlyNamedDecl would report it unmarked, a
  // false positive when the instantiated entity is [[ref_to_uninit]]. The
  // assignment is rebuilt at instantiation, where the marker is concrete.
  // The source's dependence is the funnel's to defer on. The marker is read
  // through transparent casts ((int *&)p = q is p's own reseat, and p's
  // marker must judge q), like every other marker read.
  if (LHS->isInstantiationDependent())
    return;
  // A target of unknown marking -- an alias of a pointer object, an unnamed
  // lvalue, a conditional whose arms disagree -- checks neither direction:
  // either marking answer would reject a legal store.
  if (std::optional<bool> Marked = resolveAssignTargetMarking(LHS))
    judgeInitProfileBinding(InitBindingKind::PointerAssignment, OpLoc, *Marked,
                            LHS->getType(), RHS, /*D=*/nullptr);
}

void SemaProfiles::checkInitProfileAssignmentOperands(BinaryOperatorKind Opc,
                                                      Expr *LHSExpr, Expr *RHS,
                                                      bool IsCompound,
                                                      SourceLocation OpLoc) {
  if (!isProfileLive("std::init"))
    return;
  // A compound assignment reads the old value but builds no lvalue-to-rvalue
  // node for it, so the DefaultLvalueConversion read-through chokepoint never
  // sees the load; check it here. The shift forms are the exception:
  // CheckShiftOperands promotes their LHS through DefaultLvalueConversion,
  // which has already fired for them.
  if (IsCompound && Opc != BO_ShlAssign && Opc != BO_ShrAssign)
    checkInitProfileReadThrough(LHSExpr->getExprLoc(), LHSExpr,
                                LHSExpr->getType());
  checkInitProfileSubobjectWrite(OpLoc, LHSExpr);
  if (!IsCompound)
    checkInitProfilePointerAssignment(LHSExpr, RHS, OpLoc);
}

void SemaProfiles::checkInitProfileIncDec(Expr *Operand, SourceLocation OpLoc) {
  if (!isProfileLive("std::init"))
    return;
  // ++/-- reads the old value with no lvalue-to-rvalue node (unlike -x or
  // !x), then stores to its operand like an assignment does to its LHS.
  checkInitProfileReadThrough(Operand->getExprLoc(), Operand,
                              Operand->getType());
  checkInitProfileSubobjectWrite(OpLoc, Operand);
}

bool SemaProfiles::getHandleReseat(const CallExpr *CE, const Expr *&Obj,
                                   const Expr *&Src, SourceLocation &Loc) {
  const auto *MD = dyn_cast_or_null<CXXMethodDecl>(CE->getDirectCallee());
  if (!MD || MD->getParent()->isUnion())
    return false;
  if (const auto *OCE = dyn_cast<CXXOperatorCallExpr>(CE)) {
    if (OCE->getOperator() != OO_Equal || MD->getNumParams() != 1 ||
        MD->isStatic() || MD->isExplicitObjectMemberFunction() ||
        OCE->getNumArgs() != 2 ||
        !OCE->getArg(1)->getType()->getAsCXXRecordDecl())
      return false;
    Obj = OCE->getArg(0)->IgnoreParenImpCasts();
    Src = OCE->getArg(1);
    Loc = OCE->getOperatorLoc();
    return true;
  }
  // An operator= called by name, or an owner's reset with a written argument.
  const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE);
  if (!MCE || CE->getNumArgs() == 0 || isa<CXXDefaultArgExpr>(CE->getArg(0)))
    return false;
  if (MD->getOverloadedOperator() == OO_Equal
          ? MD->getNumParams() != 1 || MD->isStatic() ||
                MD->isExplicitObjectMemberFunction() ||
                !CE->getArg(0)->getType()->getAsCXXRecordDecl()
          : !isStdOwnerReset(MD))
    return false;
  // The object as written, before its conversion to the class declaring
  // reset (libstdc++'s std::__shared_ptr), or the pointer an arrow call
  // reaches it through.
  Obj = MCE->getImplicitObjectArgument()->IgnoreParenImpCasts();
  Src = CE->getArg(0);
  Loc = Src->IgnoreImplicit()->getExprLoc();
  return true;
}

std::optional<bool> SemaProfiles::resolveHandleReseatMarking(const Expr *Obj) {
  std::optional<bool> Marked;
  bool Disagree = false;
  forEachTargetLeaf(Obj, [&](const ShapeLeaf &Leaf) {
    const ValueDecl *VD = getDirectlyNamedDecl(Leaf.E);
    bool M = VD && !VD->getType()->isReferenceType() &&
             !VD->getType()->isPointerType() && VD->hasAttr<RefToUninitAttr>();
    Disagree |= Marked && *Marked != M;
    Marked = M;
  });
  if (Disagree)
    return std::nullopt;
  return Marked.value_or(false);
}

QualType SemaProfiles::handleReseatType(const Expr *Obj) {
  QualType T = Obj->getType();
  return T->isPointerType() ? T->getPointeeType() : T.getNonReferenceType();
}

bool SemaProfiles::isCopySource(const ParmVarDecl *P) {
  if (isAssignmentSource(P))
    return true;
  const auto *Ctor =
      P ? dyn_cast<CXXConstructorDecl>(P->getDeclContext()) : nullptr;
  return Ctor && P->getFunctionScopeIndex() == 0 &&
         Ctor->isCopyOrMoveConstructor();
}

bool SemaProfiles::isStdLibraryFunction(const FunctionDecl *FD) {
  for (const DeclContext *DC = FD->getDeclContext();
       DC && !DC->isTranslationUnit(); DC = DC->getParent())
    if (const auto *NS = dyn_cast<NamespaceDecl>(DC);
        NS && NS->isStdNamespace())
      return true;
  return false;
}

bool SemaProfiles::isStdCallable(const FunctionDecl *FD, StringRef Name) {
  if (FD->getIdentifier() && FD->getName() == Name)
    return isStdLibraryFunction(FD);
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (!MD || MD->getOverloadedOperator() != OO_Call)
    return false;
  const CXXRecordDecl *RD = MD->getParent()->getCanonicalDecl();
  const NamespaceDecl *Ranges = enclosingStdSubNamespace(RD, "ranges");
  if (!Ranges)
    return false;
  for (const NamedDecl *ND :
       Ranges->lookup(&FD->getASTContext().Idents.get(Name)))
    if (const auto *VD = dyn_cast<VarDecl>(ND->getUnderlyingDecl()))
      if (const CXXRecordDecl *VarClass = VD->getType()->getAsCXXRecordDecl();
          VarClass && VarClass->getCanonicalDecl() == RD)
        return true;
  return false;
}

bool SemaProfiles::isAssignmentSource(const ParmVarDecl *P) {
  const auto *MD = P ? dyn_cast<CXXMethodDecl>(P->getDeclContext()) : nullptr;
  return MD && MD->getOverloadedOperator() == OO_Equal && !MD->isStatic() &&
         P->getFunctionScopeIndex() ==
             (MD->isExplicitObjectMemberFunction() ? 1u : 0u);
}

void SemaProfiles::checkInitProfileHandleReseat(const FunctionDecl *FD,
                                                const CallExpr *CE) {
  if (!isProfileLive("std::init"))
    return;
  const Expr *Obj, *Src;
  SourceLocation Loc;
  if (!getHandleReseat(CE, Obj, Src, Loc) || Obj->isInstantiationDependent())
    return;
  if (std::optional<bool> Marked = resolveHandleReseatMarking(Obj))
    judgeInitProfileBinding(InitBindingKind::PointerAssignment, Loc, *Marked,
                            handleReseatType(Obj), Src, /*D=*/nullptr);
}

/// The operand of the member, element, indirection, or address-of step
/// \p E whose conditionals forEachTargetLeaf distributes, or null. A
/// non-field member names storage independent of its base.
static const Expr *shapeChainOperand(const Expr *E) {
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return isa<FieldDecl, IndirectFieldDecl>(ME->getMemberDecl())
               ? ME->getBase()
               : nullptr;
  if (const BinaryOperator *BO = SemaProfiles::asDataMemberPointerAccess(E))
    return BO->getLHS();
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return ASE->getBase();
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_Deref)
    return UO->getSubExpr();
  // A handle's member call deriving a pointer or reference, or returning the
  // handle itself, steps to its object (SemaProfiles::handleChainBase).
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    bool PastStart;
    if (const Expr *Arr = SemaProfiles::stdArrayAddressObject(CE, PastStart))
      return Arr;
    if (const Expr *Arg = SemaProfiles::derivedArgument(CE, PastStart))
      return Arg;
    const Expr *Obj;
    bool Element;
    if (SemaProfiles::isHandleDerefCall(CE, Obj, Element))
      return Obj;
    bool Partial;
    if (const Expr *Self = handleSelfReturnObject(CE, Partial))
      return Self;
  }
  return SemaProfiles::addressOfOperand(E);
}

/// The local \p E names when it stands for its initializer in a handle chain
/// (SemaProfiles::handleChainBase): an implicit handle local (range-for's
/// __range and __begin) or an unmarked local reference to a handle, not
/// reached by capture; null otherwise.
static const VarDecl *followedHandleLocal(const Expr *E) {
  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  const auto *V = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
  if (!V || !V->hasLocalStorage() || isa<ParmVarDecl>(V) || !V->getInit() ||
      !SemaProfiles::isHandleType(V->getType().getNonReferenceType()) ||
      DRE->refersToEnclosingVariableOrCapture())
    return nullptr;
  if (V->isImplicit() ||
      (V->getType()->isReferenceType() && !V->hasAttr<RefToUninitAttr>()))
    return V;
  return nullptr;
}

/// Descend the chain below \p Cur, appending to \p Arms the arm taken at
/// each conditional reached, and call \p Emit once per combination of arms.
/// A local in \p Followed, which the descent to \p Cur already stepped
/// through, ends the chain.
static void
forEachNestedArmChoice(const Expr *Cur, SmallVectorImpl<const Expr *> &Arms,
                       llvm::SmallPtrSetImpl<const VarDecl *> &Followed,
                       llvm::function_ref<void()> Emit) {
  SmallVector<const VarDecl *, 2> Entered;
  while (true) {
    Cur = SemaProfiles::ignoreTransparentCasts(Cur);
    if (const auto *CO = dyn_cast<AbstractConditionalOperator>(Cur)) {
      auto [T, F] = conditionalArms(CO);
      for (const Expr *Arm : {T, F}) {
        Arms.push_back(Arm);
        forEachNestedArmChoice(Arm, Arms, Followed, Emit);
        Arms.pop_back();
      }
      break;
    }
    if (const VarDecl *V = followedHandleLocal(Cur);
        V && Followed.insert(V).second) {
      Entered.push_back(V);
      Cur = V->getInit();
      continue;
    }
    Cur = followedHandleLocal(Cur) ? nullptr : shapeChainOperand(Cur);
    if (!Cur) {
      Emit();
      break;
    }
  }
  for (const VarDecl *V : Entered)
    Followed.erase(V);
}

void SemaProfiles::forEachTargetLeaf(
    const Expr *E, llvm::function_ref<void(const ShapeLeaf &)> F) {
  E = ignoreTransparentCasts(E);
  // A conditional names whichever lvalue the chosen arm does: each arm is a
  // leaf of its own.
  if (const auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
    auto [T, Fa] = conditionalArms(CO);
    forEachTargetLeaf(T, F);
    forEachTargetLeaf(Fa, F);
    return;
  }
  SmallVector<const Expr *, 4> Arms;
  llvm::SmallPtrSet<const VarDecl *, 4> Followed;
  forEachNestedArmChoice(E, Arms, Followed, [&] { F(ShapeLeaf(E, Arms)); });
}

/// The test::class_final pilot: fires on every completed non-lambda class.
static void checkTestClassFinal(Sema &S, CXXRecordDecl *RD) {
  if (!S.Profiles().shouldEmitProfileViolation(
          "test::class_final", diag::err_profile_class_final_test,
          RD->getLocation(), RD))
    return;
  S.Diag(RD->getLocation(), diag::err_profile_class_final_test)
      << "test::class_final" << RD;
}

/// The test::ctor_final pilot: fires once per user-provided, non-delegating
/// constructor.
static void checkTestCtorFinal(Sema &S, CXXConstructorDecl *Ctor) {
  if (!S.Profiles().shouldEmitProfileViolation(
          "test::ctor_final", diag::err_profile_ctor_final_test,
          Ctor->getLocation(), Ctor))
    return;
  S.Diag(Ctor->getLocation(), diag::err_profile_ctor_final_test)
      << "test::ctor_final" << Ctor->getParent();
}

// True if any leaf field of \p RD -- recursing through nested anonymous
// records -- has a written member-initializer in \p Written. One written
// leaf gives an anonymous union its active member.
static bool
anyLeafFieldWritten(const CXXRecordDecl *RD,
                    const llvm::SmallPtrSetImpl<const FieldDecl *> &Written) {
  for (const FieldDecl *F : RD->fields()) {
    if (Written.count(F))
      return true;
    if (F->isAnonymousStructOrUnion())
      if (const CXXRecordDecl *AnonRD = F->getType()->getAsCXXRecordDecl();
          AnonRD && AnonRD->hasDefinition() &&
          anyLeafFieldWritten(AnonRD->getDefinition(), Written))
        return true;
  }
  return false;
}

// The shared per-field walk of the ctor_uninit_member checks (the
// user-provided-constructor check and the inherited-constructor
// class-completion check): visit every checkable field of \p RD left without an
// initializer against \p Written, recursing into anonymous struct members --
// their leaves initialize exactly like direct members of the walked class (a
// written initializer for one is an *indirect* member-initializer, which the
// Written collection already resolved to the leaf FieldDecl via getAnyMember,
// and NSDMIs and [[uninit]] markers sit on the leaves), so the same per-field
// logic applies to them. \p DiagnoseField is invoked for an uninitialized
// named field and \p DiagnoseAnonUnion for an anonymous union none of whose
// leaves is written or NSDMI-activated; both callbacks gate (suppression /
// deferral) and emit.
template <typename FieldFn, typename AnonUnionFn>
static void
forEachCtorUninitField(Sema &S, const CXXRecordDecl *RD,
                       const llvm::SmallPtrSetImpl<const FieldDecl *> &Written,
                       FieldFn DiagnoseField, AnonUnionFn DiagnoseAnonUnion) {
  for (const FieldDecl *F : RD->fields()) {
    if (F->isUnnamedBitField())
      continue;
    if (F->isAnonymousStructOrUnion()) {
      const CXXRecordDecl *AnonRD = F->getType()->getAsCXXRecordDecl();
      if (!AnonRD || !AnonRD->hasDefinition() || AnonRD->isInvalidDecl())
        continue;
      // An anonymous union's members are mutually exclusive: one written
      // leaf gives it its active member -- lenient for a struct variant
      // only partially covered by its written leaves (a missed
      // diagnostic, never a false positive) -- and a vacuous
      // default-initialization (an empty union, or a leaf NSDMI, which is
      // the active member) needs nothing. A leaf [[uninit]] marker is not
      // consulted: union_marker already rejects markers on union members.
      if (AnonRD->isUnion()) {
        if (!anyLeafFieldWritten(AnonRD->getDefinition(), Written) &&
            S.Profiles().defaultInitLeavesScalarIndeterminate(F->getType()))
          DiagnoseAnonUnion(F);
        continue;
      }
      forEachCtorUninitField(S, AnonRD->getDefinition(), Written, DiagnoseField,
                             DiagnoseAnonUnion);
      continue;
    }
    // Other unnamed fields are skipped; a named bit-field is checked like
    // any other member. Reference and const members already have dedicated
    // diagnostics when left uninitialized.
    if (!F->getDeclName() || F->getType()->isReferenceType() ||
        F->getType().isConstQualified())
      continue;
    if (F->hasAttr<UninitAttr>() || F->hasInClassInitializer() ||
        Written.count(F))
      continue;
    if (!S.Profiles().defaultInitLeavesScalarIndeterminate(F->getType()))
      continue;
    DiagnoseField(F);
  }
}

// The per-field walk of the ctor_uninit_member constructor check:
// diagnoses at the constructor, gated per field on the violation gate, which
// takes the constructor so a templated pattern defers to its instantiations.
static void diagnoseCtorUninitFields(
    Sema &S, const CXXConstructorDecl *Ctor, const CXXRecordDecl *RD,
    const llvm::SmallPtrSetImpl<const FieldDecl *> &Written) {
  forEachCtorUninitField(
      S, RD, Written,
      [&](const FieldDecl *F) {
        if (!S.Profiles().shouldEmitProfileViolation(
                "std::init", diag::err_init_ctor_uninit_member,
                Ctor->getLocation(), Ctor))
          return;
        S.Diag(Ctor->getLocation(), diag::err_init_ctor_uninit_member)
            << "std::init" << F->getDeclName();
        S.Diag(F->getLocation(), diag::note_init_uninit_member_here)
            << F->getDeclName();
      },
      [&](const FieldDecl *F) {
        if (!S.Profiles().shouldEmitProfileViolation(
                "std::init", diag::err_init_ctor_uninit_member,
                Ctor->getLocation(), Ctor))
          return;
        S.Diag(Ctor->getLocation(), diag::err_init_ctor_uninit_anon_union)
            << "std::init";
        S.Diag(F->getLocation(), diag::note_init_uninit_anon_union_here);
      });
}

static void checkStdInitCtorUninitMember(Sema &S, CXXConstructorDecl *Ctor) {
  if (!S.Profiles().isProfileLive("std::init"))
    return;
  // Paper §6.1: a user-provided constructor must initialize every member via
  // its member-initializer list or an NSDMI, unless the member is marked
  // [[uninit]] (whose body initialization is the deferred R7 check).
  // A plain assignment in the constructor body does not count.
  if (!Ctor->isUserProvided())
    return;

  // An explicitly defaulted copy or move constructor (out-of-line
  // '= default'; the constructor-finalization funnel sends every defaulted
  // definition here) initializes every member member-wise while *writing*
  // no initializer -- exempt, or the written-initializer rule below would
  // false-positive on it. The defaulted *default* constructor stays: its
  // members get exactly the non-written initializers this rule checks.
  if (Ctor->isExplicitlyDefaulted() && !Ctor->isDefaultConstructor())
    return;

  // A union's members are mutually exclusive; a constructor initializes at most
  // one, so the "every member" rule does not apply (paper §6.5). Whether the
  // active member is set is a constructor-body flow question, deferred.
  if (Ctor->getParent()->isUnion())
    return;

  // Members and direct bases given a written initializer by this constructor.
  llvm::SmallPtrSet<const FieldDecl *, 8> Written;
  llvm::SmallPtrSet<const Type *, 4> WrittenBases;
  for (const CXXCtorInitializer *Init : Ctor->inits()) {
    if (!Init->isWritten())
      continue;
    if (Init->isAnyMemberInitializer()) {
      if (const FieldDecl *F = Init->getAnyMember())
        Written.insert(F);
    } else if (Init->isBaseInitializer()) {
      if (const Type *T = Init->getBaseClass())
        WrittenBases.insert(
            S.Context.getCanonicalType(QualType(T, 0)).getTypePtr());
    }
  }

  diagnoseCtorUninitFields(S, Ctor, Ctor->getParent(), Written);

  // The guarantee is over the complete object (paper §5.1, §7.1), so a
  // direct base-class subobject left indeterminate is as much a violation as a
  // member. A base cannot carry an [[uninit]] marker (the attribute's subjects
  // are Var/Field), so an indeterminate base must always be initialized --
  // there
  // is no marker escape. Virtual bases are the most-derived constructor's
  // responsibility, not a local property of this constructor, so they are
  // deferred. A written base-initializer initializes the base; an implicit
  // (non-written) one is default-init, handled by the indeterminate check.
  for (const CXXBaseSpecifier &Base : Ctor->getParent()->bases()) {
    if (Base.isVirtual())
      continue;
    if (WrittenBases.count(
            S.Context.getCanonicalType(Base.getType()).getTypePtr()))
      continue;
    if (!S.Profiles().defaultInitLeavesScalarIndeterminate(Base.getType()))
      continue;
    if (!S.Profiles().shouldEmitProfileViolation(
            "std::init", diag::err_init_ctor_uninit_member, Ctor->getLocation(),
            Ctor))
      continue;
    S.Diag(Ctor->getLocation(), diag::err_init_ctor_uninit_base)
        << "std::init" << Base.getType();
    S.Diag(Base.getBeginLoc(), diag::note_init_uninit_base_here)
        << Base.getType();
  }
}

static void checkStdInitInheritedCtorUninitMember(Sema &S, CXXRecordDecl *RD) {
  if (!S.Profiles().isProfileLive("std::init"))
    return;
  // Paper §6.1's obligation applied to inheriting constructors
  // ([class.inhctor.init]): an inherited constructor initializes only the
  // nominated base; the inheriting class's own members and its other bases
  // get NSDMI-or-default-initialization -- invariantly, for every inherited
  // signature. uninit_decl cannot catch a defaulted-argument use (`D d(1)`
  // has an initializer) and the constructor-finalization check skips the
  // synthesized constructors (!isUserProvided), so the obligation is checked
  // here, once per class at finalization, attributed to the
  // using-declaration -- not at lazy constructor synthesis, which is
  // use-dependent, duplicates per inherited signature, and attributes far
  // from the defect. The rule name stays ctor_uninit_member: it is the same
  // obligation, so suppression targeting stays uniform.
  //
  // Inheriting-constructor introducers in lexical order, each with its
  // nominated base and whether any target constructor is usable (a group
  // whose every target is deleted inherits nothing callable and imposes no
  // obligation).
  struct InheritedCtorGroup {
    const UsingDecl *Introducer;
    const CXXRecordDecl *NominatedBase;
    bool AnyUsable;
  };
  llvm::SmallVector<InheritedCtorGroup, 2> Groups;
  for (const Decl *D : RD->decls()) {
    const auto *Shadow = dyn_cast<ConstructorUsingShadowDecl>(D);
    if (!Shadow || Shadow->isInvalidDecl())
      continue;
    const auto *Introducer = dyn_cast<UsingDecl>(Shadow->getIntroducer());
    if (!Introducer)
      continue;
    InheritedCtorGroup *G = nullptr;
    for (InheritedCtorGroup &Existing : Groups)
      if (Existing.Introducer == Introducer)
        G = &Existing;
    if (!G) {
      Groups.push_back({Introducer, Shadow->getNominatedBaseClass(), false});
      G = &Groups.back();
    }
    const NamedDecl *Target = Shadow->getTargetDecl();
    if (const auto *FTD = dyn_cast<FunctionTemplateDecl>(Target))
      Target = FTD->getTemplatedDecl();
    // A base copy/move constructor never acts as an inherited constructor
    // ([over.match.funcs.general]p8 excludes it from every candidate set),
    // so it cannot make the group usable -- otherwise the base's implicit
    // copy constructor would defeat the all-deleted skip below.
    if (const auto *CD = dyn_cast<CXXConstructorDecl>(Target);
        CD && !CD->isDeleted() && !CD->isCopyOrMoveConstructor())
      G->AnyUsable = true;
  }
  llvm::erase_if(Groups,
                 [](const InheritedCtorGroup &G) { return !G.AnyUsable; });
  if (Groups.empty())
    return;

  // MEMBERS once per class -- the obligation is identical for every
  // introducer and signature -- anchored at the lexically first introducer.
  // The shared walk runs with an empty written-set: an inherited constructor
  // writes no member-initializers. The gate takes the introducer, so a
  // templated pattern defers (this class-completion check re-fires on
  // instantiation).
  {
    const UsingDecl *First = Groups.front().Introducer;
    const CXXRecordDecl *FirstBase = Groups.front().NominatedBase;
    SourceLocation FirstLoc = First->getLocation();
    llvm::SmallPtrSet<const FieldDecl *, 1> NoneWritten;
    forEachCtorUninitField(
        S, RD, NoneWritten,
        [&](const FieldDecl *F) {
          if (!S.Profiles().shouldEmitProfileViolation(
                  "std::init", diag::err_init_ctor_uninit_member, FirstLoc,
                  First))
            return;
          S.Diag(FirstLoc, diag::err_init_inherited_ctor_uninit_member)
              << "std::init" << FirstBase << F->getDeclName();
          S.Diag(F->getLocation(), diag::note_init_uninit_member_here)
              << F->getDeclName();
        },
        [&](const FieldDecl *F) {
          if (!S.Profiles().shouldEmitProfileViolation(
                  "std::init", diag::err_init_ctor_uninit_member, FirstLoc,
                  First))
            return;
          S.Diag(FirstLoc, diag::err_init_inherited_ctor_uninit_anon_union)
              << "std::init" << FirstBase;
          S.Diag(F->getLocation(), diag::note_init_uninit_anon_union_here);
        });
  }

  // BASES per introducer: the inherited constructor initializes exactly its
  // nominated base, so every *other* direct non-virtual base whose
  // default-initialization is indeterminate is left that way (mirror of the
  // constructor check's base loop; virtual bases stay deferred as the
  // most-derived constructor's responsibility).
  for (const InheritedCtorGroup &G : Groups) {
    for (const CXXBaseSpecifier &Base : RD->bases()) {
      if (Base.isVirtual())
        continue;
      const CXXRecordDecl *BaseRD = Base.getType()->getAsCXXRecordDecl();
      if (BaseRD && G.NominatedBase &&
          BaseRD->getCanonicalDecl() == G.NominatedBase->getCanonicalDecl())
        continue;
      if (!S.Profiles().defaultInitLeavesScalarIndeterminate(Base.getType()))
        continue;
      if (!S.Profiles().shouldEmitProfileViolation(
              "std::init", diag::err_init_ctor_uninit_member,
              G.Introducer->getLocation(), G.Introducer))
        continue;
      S.Diag(G.Introducer->getLocation(),
             diag::err_init_inherited_ctor_uninit_base)
          << "std::init" << G.NominatedBase << Base.getType();
      S.Diag(Base.getBeginLoc(), diag::note_init_uninit_base_here)
          << Base.getType();
    }
  }
}

static void checkStdInitUninitFieldMarker(Sema &S, CXXRecordDecl *RD) {
  if (!S.Profiles().isProfileLive("std::init"))
    return;
  // std::init / uninit_with_initializer, field flavor (paper §4.2 rule 2,
  // §5.3): [[uninit]] on a data member claims default-initialization leaves
  // the member uninitialized. When the member type's default-initialization
  // is not a no-op (a non-trivial default constructor initializes something)
  // or leaves nothing indeterminate (nothing to acknowledge), the marker is a
  // contradiction, just like a variable's explicit initializer. The NSDMI
  // case is the ActOnFinishCXXInClassMemberInitializer flavor's to diagnose;
  // this covers the initializer-less member, which only the class walk sees.
  //
  // A union's members are union_marker's territory (the marker is banned on
  // them wholesale, paper §5.6).
  if (RD->isUnion())
    return;
  for (const FieldDecl *F : RD->fields()) {
    const auto *UA = F->getAttr<UninitAttr>();
    // hasInClassInitializer is style-based, so it is true even while a
    // late-parsed NSDMI is still pending.
    if (!UA || F->isInvalidDecl() || F->hasInClassInitializer())
      continue;
    // Act only on the classifier's NonVacuousDefault verdict: a union- or
    // pointer-typed member is union_marker / pointer_marker's (load-bearing
    // for union members: a union with a non-trivial member has a deleted --
    // hence non-trivial -- default constructor and would otherwise draw
    // both), and a Consistent member really is left uninitialized.
    UninitMarkerClass C = classifyUninitMarker(S.Profiles(), F, nullptr);
    if (C.Verdict != UninitMarkerVerdict::NonVacuousDefault)
      continue;
    // The gate takes the field, so a templated pattern defers (instantiations
    // re-fire through CheckCompletedCXXClass). Diagnose at the attribute --
    // the marker is the thing to delete -- like union_marker /
    // pointer_marker.
    if (!S.Profiles().shouldEmitProfileViolation(
            "std::init", diag::err_init_uninit_not_left_uninitialized,
            UA->getLocation(), F))
      continue;
    S.Diag(UA->getLocation(), diag::err_init_uninit_not_left_uninitialized)
        << "std::init" << F->getDeclName() << F->getType() << /*IsMember=*/1;
    S.Diag(F->getTypeSpecStartLoc(), diag::note_init_uninit_marker_type)
        << S.Context.getBaseElementType(F->getType()) << C.Reason;
  }
}

void SemaProfiles::checkProfileViolationsAtClassFinalization(
    CXXRecordDecl *RD) {
  if (!getLangOpts().Profiles || !RD)
    return;
  if (RD->isInvalidDecl() || RD->isDependentType() || RD->isLambda())
    return;
  checkTestClassFinal(SemaRef, RD);
  checkStdInitUninitFieldMarker(SemaRef, RD);
  checkStdInitInheritedCtorUninitMember(SemaRef, RD);
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
  checkTestCtorFinal(SemaRef, Ctor);
  checkStdInitCtorUninitMember(SemaRef, Ctor);
}
