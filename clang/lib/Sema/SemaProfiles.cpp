//===--- SemaProfiles.cpp - Semantic Analysis for C++ Profiles ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements semantic analysis for the C++ profiles framework
/// (P3589R2) and the built-in std::init initialization profile (P4222R1.1):
/// enforcement and suppression recording, the violation gate, the class- and
/// constructor-finalization dispatch, and the parse-time std::init rule
/// checks. The CFG-based std::init checks live in AnalysisBasedWarnings.cpp.
///
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaProfiles.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/IgnoreExpr.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/ScopeInfo.h"
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
  // profile maps nothing. See ProfilesFrameworkInternals.rst,
  // "Enforcement and Suppression State".
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
  // A template's attributes are attached to its templated declaration.
  if (const auto *TD = dyn_cast<TemplateDecl>(D))
    D = TD->getTemplatedDecl();
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
                                              SourceLocation Loc, const Decl *D,
                                              bool PostParse) {
  if (!getASTContext().isProfileRuleActiveAt(DiagID, Loc))
    return false;
  // A templated entity is not a phase-7 entity (P3589R2 §1.1), so a profile
  // rule fires only on the instantiation, never on the pattern (checking the
  // pattern would diagnose never-instantiated templates and double-fire);
  // std::init's marker and binding checks pass the Decl and defer here. A
  // Decl-less expression check site must instead defer in a dependent context
  // from its own wrapper; see ProfilesFrameworkInternals.rst, "Pattern 1".
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
// *root* record only (recursion always re-trusts): defaultInitIsVacuous uses
// it to ask the factual question about a class whose out-of-line defaulted
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

bool SemaProfiles::defaultInitLeavesScalarIndeterminate(
    QualType T, bool HonorUninitMarkers) {
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
  return defaultInitLeavesScalarIndeterminateImpl(getASTContext(), T,
                                                  HonorUninitMarkers, Visited);
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

bool SemaProfiles::defaultInitIsVacuous(QualType T) {
  QualType BaseTy = getASTContext().getBaseElementType(T);
  bool UntrustRoot = false;
  if (const auto *RD = BaseTy->getAsCXXRecordDecl()) {
    // A non-trivial default constructor (user-provided anywhere in the
    // subtree, a default member initializer, a virtual table pointer)
    // initializes something, contradicting an [[uninit]] marker (paper §4.2
    // rule 2, §5.3). hasTrivialDefaultConstructor asserts without a
    // definition.
    if (!RD->hasDefinition())
      return false;
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
      // stays rejected (see Limitations).
      const CXXRecordDecl *Def = RD->getDefinition();
      if (!hasOutOfLineDefaultedDefaultCtor(Def) ||
          !defaultedDefaultCtorIsTrivial(getASTContext(), Def))
        return false;
      UntrustRoot = true;
    }
    // A deleted default constructor keeps the triviality bit, but makes
    // default-initialization ill-formed rather than a no-op: the entity can
    // never be left default-initialized, so the marker is unsatisfiable. Only
    // a declared deleted constructor is visible here; a lazily *implicitly*
    // deleted one of an otherwise trivial type escapes this scan -- a missed
    // diagnostic (never a false positive), like the framework's other
    // conservative omissions.
    for (const CXXConstructorDecl *Ctor : RD->getDefinition()->ctors())
      if (Ctor->isDefaultConstructor() && Ctor->isDeleted())
        return false;
  }
  // The factual (HonorUninitMarkers=false) walk: an all-scalars-determinate
  // type (e.g. an empty struct) has nothing uninitialized, so the marker
  // contradicts it too, while a type whose only indeterminate scalars are
  // themselves marked members really is left uninitialized.
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
  return defaultInitLeavesScalarIndeterminateImpl(getASTContext(), T,
                                                  /*HonorUninitMarkers=*/false,
                                                  Visited, UntrustRoot);
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

// Whether the declaration initializer \p Init is a vacuous
// default-initialization of \p T: one that runs no code and leaves the object
// factually uninitialized, hence consistent with an [[uninit]] marker. The
// shared guard of static_marker and uninit_with_initializer keeps the pair
// complementary by construction: exactly one of the two fires for a marked
// static.
static bool isVacuousDefaultInit(SemaProfiles &SP, const Expr *Init,
                                 QualType T) {
  return SemaProfiles::isDefaultInitShape(Init) &&
         (!Init || SP.defaultInitIsVacuous(T));
}

// Why default-initialization of \p BaseTy is not the no-op an [[uninit]]
// marker claims, as the select index of note_init_uninit_marker_type: a
// non-trivial default constructor runs code (0); a trivial one that leaves no
// subobject uninitialized has nothing to acknowledge (1); a deleted or absent
// one makes the marker unsatisfiable (2). Shared by the variable and data
// member flavors of uninit_with_initializer so both explain it the same way.
static unsigned uninitMarkerNonVacuityReason(QualType BaseTy) {
  const auto *RD = BaseTy->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return 1;
  const CXXRecordDecl *Def = RD->getDefinition();
  bool Deleted = !Def->hasDefaultConstructor();
  for (const CXXConstructorDecl *Ctor : Def->ctors())
    if (Ctor->isDefaultConstructor() && Ctor->isDeleted())
      Deleted = true;
  if (Deleted)
    return 2;
  return Def->hasTrivialDefaultConstructor() ? 1 : 0;
}

void SemaProfiles::checkInitProfileUninitDecl(const VarDecl *Var) {
  // std::init / uninit_decl: a definition without any initializer (after
  // attempted default-initialization) must either carry [[uninit]] or
  // be initialized by a language rule. Static / thread storage duration is
  // excluded -- those are zero-initialized; runtime-init concerns are R3's.
  static constexpr StringRef Profile = "std::init";
  // Enforcement first: the call site (ActOnUninitializedDecl) is ungated, so
  // every compile would otherwise pay the type walk below. The hoisted gate
  // is evaluated in the same call as shouldEmitProfileViolation's identical
  // first conjunct, with nothing but const queries in between, so it cannot
  // change any answer; the cheap decl-state tests likewise run before the
  // type walk.
  if (!isProfileEnforced(Profile))
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
      shouldEmitProfileViolation(diag::err_init_uninit_decl, Var->getLocation(),
                                 Var) &&
      // A definition with no initializer (scalar / pointer / enum, or an
      // array of them), or a class/aggregate type -- possibly the element
      // type of an array -- whose default-init leaves a scalar subobject
      // indeterminate (its synthesized constructor call provides an
      // initializer, so the !getInit() test alone misses it). The
      // suppression walk stays ahead of the recursive type walk.
      (!Var->getInit() ||
       (BaseTy->isRecordType() &&
        defaultInitLeavesScalarIndeterminate(Var->getType(),
                                             /*HonorUninitMarkers=*/true)))) {
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
// callback's skip -- share this predicate so the exactly-one-diagnostic
// invariants hold by construction. ObjC object pointers are a follow-on
// (needs ObjC++ scaffolding).
static bool isPointerMarkerBannedType(QualType T) {
  return T->isPointerType() || T->isMemberPointerType() ||
         T->isBlockPointerType();
}

void SemaProfiles::checkInitProfileStaticMarker(const VarDecl *Var) {
  // std::init / static_marker: a variable with static or thread storage
  // duration is zero-initialized by language rule (paper §3), so it is an
  // initialized object; marking it [[uninit]] contradicts paper §4.2 ("an
  // initialized object marked [[uninit]] is an error"). The case with a real
  // initializer -- explicit, or a default-initialization that is not a no-op
  // -- is already caught by uninit_with_initializer (R4, in
  // CheckCompleteVariableDeclaration); this covers the vacuous-initialization
  // case R4 treats as consistent. The guard is the shared
  // isVacuousDefaultInit, so the pair stays complementary by construction and
  // exactly one of static_marker / uninit_with_initializer fires (this one
  // runs first, from ActOnUninitializedDecl, after the synthesized
  // default-initialization is attached).
  static constexpr StringRef Profile = "std::init";
  // Enforcement first (same rationale as checkInitProfileUninitDecl: the
  // call site is ungated, the hoisted gate is shouldEmitProfileViolation's
  // own first conjunct, and only const queries run in between), then the
  // cheap decl-state tests, then the type walk.
  if (!isProfileEnforced(Profile))
    return;
  if (Var->isInvalidDecl() ||
      (Var->getStorageDuration() != SD_Static &&
       Var->getStorageDuration() != SD_Thread) ||
      !Var->hasAttr<UninitAttr>())
    return;
  QualType BaseTy = getASTContext().getBaseElementType(Var->getType());
  // A union or pointer object -- or an array of them -- marked [[uninit]]
  // is already rejected by union_marker / pointer_marker (regardless of
  // storage duration, and keyed on the same base element type), and they
  // retain the marker; do not pile a second diagnostic on top.
  if (!BaseTy->isUnionType() && !isPointerMarkerBannedType(BaseTy) &&
      shouldEmitProfileViolation(diag::err_init_uninit_static_marker,
                                 Var->getLocation(), Var) &&
      isVacuousDefaultInit(*this, Var->getInit(), Var->getType())) {
    bool IsThread = Var->getStorageDuration() == SD_Thread;
    Diag(Var->getLocation(), diag::err_init_uninit_static_marker)
        << Profile << Var->getDeclName() << IsThread;
  }
}

bool SemaProfiles::checkInitProfileStaticRuntimeInit(
    const VarDecl *Var, llvm::function_ref<bool()> CheckConstInit) {
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
  static constexpr StringRef Profile = "std::init";
  // Gate on enforcement before evaluating the initializer: this call site
  // sits ahead of -Wglobal-constructors' isIgnored guard, so evaluating
  // first would charge every global with a non-constant initializer for the
  // constant-initializer evaluation even with profiles disabled.
  if (!shouldEmitProfileViolation(diag::err_init_static_runtime_init,
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
  // [[uninit]] documents that the entity is intentionally left
  // uninitialized, so it contradicts an explicit initializer. A RecoveryExpr
  // is a placeholder for an initialization that already failed (e.g.
  // default-init of a const scalar), not an initializer the user wrote, so it
  // must not trigger this rule.
  if (!D->hasAttr<UninitAttr>() || !Init ||
      isa<RecoveryExpr>(Init->IgnoreParens()))
    return;
  SourceLocation Loc = D->getLocation();
  static constexpr StringRef Profile = "std::init";
  // Gate the (possibly recursive) type walk below on enforcement.
  if (!shouldEmitProfileViolation(diag::err_init_uninit_with_initializer, Loc,
                                  D))
    return;
  // A vacuous default-initialization -- a synthesized trivial
  // default-constructor call that runs no code and leaves the object
  // indeterminate -- is consistent with the marker: the object really is left
  // uninitialized, to be initialized later (e.g. via construct_at), mirroring
  // the scalar case. Anything else -- an explicit initializer, a `= P()`
  // value-initialization, or a default-initialization that initializes
  // something (a non-trivial default constructor, paper §4.2 rule 2, §5.3) --
  // contradicts it.
  if (isVacuousDefaultInit(*this, Init, D->getType()))
    return;
  bool IsMember = isa<FieldDecl>(D);
  // The marker is contradicted for one of two reasons, which read very
  // differently to a user. A *default*-initialization that is not a no-op
  // initializes something without the user writing anything, so saying the
  // entity "has an initializer" would be wrong; report the type and why, as
  // the initializer-less data member flavor in
  // runStdInitUninitFieldMarkerCallback does.
  if (isDefaultInitShape(Init)) {
    QualType BaseTy = getASTContext().getBaseElementType(D->getType());
    Diag(Loc, diag::err_init_uninit_not_left_uninitialized)
        << Profile << D->getDeclName() << D->getType() << IsMember;
    SourceLocation NoteLoc = Loc;
    if (const auto *DD = dyn_cast<DeclaratorDecl>(D))
      NoteLoc = DD->getTypeSpecStartLoc();
    Diag(NoteLoc, diag::note_init_uninit_marker_type)
        << BaseTy << uninitMarkerNonVacuityReason(BaseTy);
    return;
  }
  Diag(Loc, diag::err_init_uninit_with_initializer)
      << Profile << D->getDeclName() << IsMember;
}

void SemaProfiles::checkInitProfileMarkerPlacement(const Decl *D) {
  const auto *UA = D->getAttr<UninitAttr>();
  if (!UA)
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
  QualType BaseTy =
      getASTContext().getBaseElementType(cast<ValueDecl>(D)->getType());
  bool UnionMember =
      isa<FieldDecl>(D) && cast<FieldDecl>(D)->getParent()->isUnion();
  if ((BaseTy->isUnionType() || UnionMember) &&
      shouldEmitProfileViolation(diag::err_init_union_marker, Loc, D))
    Diag(Loc, diag::err_init_union_marker) << "std::init"
                                           << (UnionMember         ? 1
                                               : isa<FieldDecl>(D) ? 2
                                                                   : 0);
  else if (isPointerMarkerBannedType(BaseTy) &&
           shouldEmitProfileViolation(diag::err_init_uninit_pointer_marker, Loc,
                                      D))
    Diag(Loc, diag::err_init_uninit_pointer_marker) << "std::init";
}

bool SemaProfiles::diagnoseInvalidUninitMarker(const Decl *D,
                                               SourceLocation AttrLoc,
                                               bool Diagnose) {
  const auto *VD = dyn_cast<ValueDecl>(D);
  if (!VD)
    return false;
  QualType T = VD->getType();

  // A dependent subject is validated at instantiation instead, once the
  // substituted type is known (Sema::InstantiateAttrs).
  if (T->isDependentType())
    return false;

  if (T->isReferenceType()) {
    if (Diagnose)
      Diag(AttrLoc, diag::err_uninit_attr_invalid_subject) << /*Reference=*/0u;
    return true;
  }
  return false;
}

bool SemaProfiles::diagnoseInvalidRefToUninitMarker(const Decl *D,
                                                    SourceLocation AttrLoc,
                                                    bool Diagnose) {
  QualType T;
  if (const auto *FD = dyn_cast<FunctionDecl>(D))
    T = FD->getReturnType();
  else
    T = cast<ValueDecl>(D)->getType();

  // A dependent subject is validated at instantiation instead, once the
  // substituted type is known (Sema::InstantiateAttrs).
  if (T->isDependentType())
    return false;

  if ((!T->isPointerType() && !T->isReferenceType()) ||
      T->isFunctionPointerType() || T->isFunctionReferenceType()) {
    if (Diagnose)
      Diag(AttrLoc, diag::err_ref_to_uninit_attr_invalid_type);
    return true;
  }
  return false;
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
// The classification is tri-state: a recognized form is Initialized or
// Uninitialized, while an unrecognized one (pointer arithmetic, an
// integer-to-pointer cast, a call through a function pointer) is Unknown rather
// than assumed Initialized. Callers wanting a plain "is it uninitialized?"
// answer (SemaProfiles::refersToUninitializedMemory, the read-through check)
// treat
// Unknown as not uninitialized.
//
// How the classified expression is being accessed is carried by
// UninitAccessOpts below.
enum class UninitStorage { Initialized, Uninitialized, Unknown };

// How an expression is being used, for the uninit recognizers.
//
// DropTopLevelUninit: a *directly named* [[uninit]] entity does not count as
// uninitialized. A value access of such an entity is owned elsewhere: a named
// [[uninit]] object by the CFG uninit_read pass, a current-object member by
// the ctor-body pass, an [[uninit]] member of a constructor-less aggregate
// local by the local-aggregate pass (all three credit assignments), and a
// marked member of an object with a user-provided constructor reached through
// any other object is deliberately trusted (paper §5.1: its constructor body
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
// SubscriptBase: the classification runs below an element access (p[i]),
// where pointee store credit must not apply: element-wise state is
// untrackable by design (paper §5.4/§5.5 ban random access through the
// marker), so only the whole-`*p` form is ever credited. Purely syntactic:
// p[0] is not credited even though it denotes the same storage as *p.
//
// Credit: when non-null, the recognizers consult the parse-order store
// credit ("Parse-Order Store Credit" in ProfilesFrameworkInternals.rst) --
// a credited entity classifies as Initialized. Null in the constexpr
// presets; the checking entry points attach it via withCredit, choosing the
// Strength every consult below passes to the credit queries.
using InitCreditStrength = SemaProfiles::InitCreditStrength;

struct UninitAccessOpts {
  bool DropTopLevelUninit = false;
  bool TrustRefToUninit = false;
  bool SubscriptBase = false;
  const SemaProfiles *Credit = nullptr;
  InitCreditStrength Strength = InitCreditStrength::Maybe;

  // Copy-then-mutate, so each helper names only the field it changes and
  // adding a field cannot silently drop out of a positional rebuild.
  UninitAccessOpts withoutTopLevelDrop() const {
    UninitAccessOpts O = *this;
    O.DropTopLevelUninit = false;
    return O;
  }
  UninitAccessOpts withSubscriptBase() const {
    UninitAccessOpts O = *this;
    O.SubscriptBase = true;
    return O;
  }
  UninitAccessOpts withoutMarkerTrust() const {
    UninitAccessOpts O = *this;
    O.TrustRefToUninit = false;
    return O;
  }
  UninitAccessOpts withCredit(const SemaProfiles *SP) const {
    return withCredit(SP, InitCreditStrength::Maybe);
  }
  UninitAccessOpts withCredit(const SemaProfiles *SP,
                              InitCreditStrength St) const {
    UninitAccessOpts O = *this;
    O.Credit = SP;
    O.Strength = St;
    return O;
  }
};

// Presets: a binding source (markers count everywhere), a value read (the
// top-level drop applies), and a scalar store (additionally, storage reached
// through [[ref_to_uninit]] is trusted).
constexpr UninitAccessOpts UninitBindAccess{};
constexpr UninitAccessOpts UninitReadAccess{/*DropTopLevelUninit=*/true};
constexpr UninitAccessOpts UninitWriteAccess{/*DropTopLevelUninit=*/true,
                                             /*TrustRefToUninit=*/true};

// Combine the arms of a conditional: Uninitialized dominates (either arm may be
// taken), then Unknown, else Initialized.
static UninitStorage combineArms(UninitStorage A, UninitStorage B) {
  if (A == UninitStorage::Uninitialized || B == UninitStorage::Uninitialized)
    return UninitStorage::Uninitialized;
  if (A == UninitStorage::Unknown || B == UninitStorage::Unknown)
    return UninitStorage::Unknown;
  return UninitStorage::Initialized;
}

static UninitStorage
glvalueDenotesUninitStorage(ASTContext &Ctx, const Expr *E,
                            UninitAccessOpts Opts = UninitBindAccess);

const ValueDecl *SemaProfiles::getDirectlyNamedDecl(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl();
  return nullptr;
}

// True if E denotes the current object: `this` (the implicit/explicit pointer
// of an arrow access) or `*this` (the object lvalue of a dot access). A local
// twin of AnalysisBasedWarnings.cpp's isCurrentObjectBase (the CFG passes'
// helper); each file keeps its recognizer vocabulary self-contained.
static bool isCurrentObjectExpr(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (isa<CXXThisExpr>(E))
    return true;
  const auto *UO = dyn_cast<UnaryOperator>(E);
  return UO && UO->getOpcode() == UO_Deref &&
         isa<CXXThisExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
}

// The parse-time pattern of \p FD -- the declaration whose body statements
// the current function can share. TreeTransform hands a statement back
// *unchanged* when nothing in it needs rebuilding, so a fully non-dependent
// `this->m = 5` inside a generic lambda (or a lambda in a member function
// template) runs Sema -- and earns its parse-order credit -- only once,
// while the pattern is parsed; the instantiated call operator reuses the
// statement wholesale. Keying current-object member credit on the pattern
// makes record and consult agree whether a statement was reused
// (pattern-time credit) or rebuilt (the instantiation-time key normalizes
// to the same pattern). Iterated because each transform hop adds one link:
// a generic lambda in a member function template reaches its parsed pattern
// via a member-specialization link and then a primary-template link (a
// local twin of SemaLambda.cpp's getPatternFunctionDecl). Instantiations of
// one pattern share its key -- and its statements, so the parse order the
// credit approximates is the same for all of them; a store only one
// sibling instantiation rebuilds (e.g. under a dependent `if constexpr`)
// then credits the others too, a parse-order-style missed diagnostic, never
// a false positive.
static const FunctionDecl *getParseTimePattern(const FunctionDecl *FD) {
  while (FD) {
    // A local function without template machinery of its own, instantiated
    // while transforming an enclosing templated body. (A transformed lambda
    // call operator instead carries a member-specialization link and a
    // generic lambda's specialization a primary-template link -- both
    // resolved by the pattern walk below.)
    if (FD->getTemplatedKind() == FunctionDecl::TK_DependentNonTemplate) {
      const FunctionDecl *P = FD->getInstantiatedFromDecl();
      if (!P)
        return FD;
      FD = P;
      continue;
    }
    const FunctionDecl *P = FD->getTemplateInstantiationPattern();
    if (!P || P == FD)
      return FD;
    FD = P;
  }
  return FD;
}

const Decl *SemaProfiles::resolveMemberStoreBase(const MemberExpr *ME) const {
  const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
  // The current object: this->m, the implicit m, or (*this).m. Keyed on the
  // enclosing function declaration's parse-time pattern (`this` cannot be
  // reseated, so the key is stable for the whole body; the *pattern*, so
  // that a statement an instantiation reuses from its pattern and a rebuilt
  // one agree on the key -- see getParseTimePattern); AllowLambda gives a
  // lambda body inside a member function its own key, so its stores and the
  // enclosing function's never share credit. No current function (e.g. an
  // NSDMI parse) is untrackable.
  if (isCurrentObjectExpr(Base))
    return getParseTimePattern(
        SemaRef.getCurFunctionDecl(/*AllowLambda=*/true));
  // A directly named local object: a dot access on a local-storage,
  // non-reference VarDecl (a by-value parameter is its own object and
  // qualifies). A reference base is an alias to an object also reachable
  // under other names, and an arrow base reaches the object through an
  // arbitrary (reseatable) pointer value -- both untrackable per object, the
  // same aliasing boundary that keeps fields of parameter-reached objects
  // uncredited. A deeper base (a.b.m) is §5.4's rejected deep
  // delayed-initialization tracking.
  if (!ME->isArrow())
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Base))
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
          VD && VD->hasLocalStorage() && !VD->getType()->isReferenceType())
        return VD;
  return nullptr;
}

// Strip what the recognizers see through on the way to a named entity:
// parens, implicit casts, and explicit casts whose operand is a pointer or
// glvalue (paper §4.3: a cast of a marked pointer is itself marked; a
// reference cast denotes the same storage). Implicit casts are re-stripped
// after every explicit-cast peel -- a cast's operand may itself be
// parenthesized or implicitly converted -- so a single leading
// IgnoreParenImpCasts is not equivalent. Shared by the store recorders, the
// lifetime-annotated-argument resolver, and the CFG member passes
// (AnalysisBasedWarnings.cpp), so crediting and flow tracking see through
// exactly the casts recognition does.
const Expr *SemaProfiles::ignoreTransparentCasts(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  while (const auto *CE = dyn_cast<ExplicitCastExpr>(E)) {
    const Expr *Sub = CE->getSubExpr();
    if (!Sub->getType()->isPointerType() && !Sub->isGLValue())
      break;
    E = Sub->IgnoreParenImpCasts();
  }
  return E;
}

// The directly named [[ref_to_uninit]] local/parameter *pointer* of \p E, if
// any: the only pointer entity whose pointee state is tracked (the credit
// map keys on VarDecls; a marked member pointer is the pinned per-object
// aliasing boundary -- copies share pointees). Sees through transparent
// casts, like the recognizers ((int *)p is still p). Shared by the
// store-recording deref arm and the [[now_init]] argument shapes.
static const VarDecl *getCreditableMarkedPointer(const Expr *E) {
  const auto *VD = dyn_cast_or_null<VarDecl>(SemaProfiles::getDirectlyNamedDecl(
      SemaProfiles::ignoreTransparentCasts(E)));
  if (VD && VD->hasLocalStorage() && VD->getType()->isPointerType() &&
      VD->hasAttr<RefToUninitAttr>())
    return VD;
  return nullptr;
}

// Pass-through forms shared by the pointer and glvalue recognizers, which are
// transparent to their operand: a single-element braced initializer { e }
// binds from e (modeling
// MismatchingNewDeleteDetector::getNewExprFromInitListOrExpr); a conditional
// is uninit if either arm is, so a value that may be uninit forces a marked
// target; a comma yields its right operand. \p EmptyListState classifies an
// empty braced list: {} value-initializes a pointer to null, which the
// pointer recognizer classifies Unknown (the null policy at its null arm),
// while a glvalue has no empty-list form (Unknown); a multi-element list is
// Unknown for both. Returns std::nullopt when E is not a pass-through form.
template <typename RecurseFn>
static std::optional<UninitStorage>
classifyUninitPassThrough(const Expr *E, UninitStorage EmptyListState,
                          RecurseFn Recurse) {
  if (const auto *ILE = dyn_cast<InitListExpr>(E)) {
    if (ILE->getNumInits() == 1)
      return Recurse(ILE->getInit(0));
    return ILE->getNumInits() == 0 ? EmptyListState : UninitStorage::Unknown;
  }
  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return combineArms(Recurse(CO->getTrueExpr()), Recurse(CO->getFalseExpr()));
  // The GNU `a ?: b` form: the true arm is the condition itself. getCommon()
  // is the *written* operand -- recursing into it (rather than getTrueExpr(),
  // which is an OpaqueValueExpr) keeps the recognizers free of OVE handling.
  if (const auto *BCO = dyn_cast<BinaryConditionalOperator>(E))
    return combineArms(Recurse(BCO->getCommon()), Recurse(BCO->getFalseExpr()));
  if (const auto *BO = dyn_cast<BinaryOperator>(E); BO && BO->isCommaOp())
    return Recurse(BO->getRHS());
  return std::nullopt;
}

// The role a known allocator callee's return value plays as an uninit
// source (paper §4.3: functions like malloc "must be known to an analyzer
// enforcing the initialization profile").
enum class AllocatorCalleeRole {
  /// Returns freshly allocated, uninitialized storage -- classified like a
  /// [[ref_to_uninit]] return, including the trusted-marker degradation.
  ReturnsUninitialized,
  /// Returns initialized (zero-filled) storage.
  ReturnsInitialized,
  /// Returns storage affirmatively neither -- realloc's preserved prefix
  /// plus indeterminate tail.
  ReturnsUnknown,
  /// Returns no storage at all (a pure release callee, present for its
  /// ReleasesStorage column): never consulted as a source -- a void return
  /// cannot reach the recognizers -- and inert (trusted) if it ever were.
  ReturnsNothing,
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

// The known allocator and deallocator callees. A row is keyed by builtin ID
// (the C allocation family) or, for the operator new/delete families, which
// never carry a builtin ID, by a form predicate. The source side (Role) says
// what the return value is -- the malloc/alloca/operator-new families return
// uninitialized memory, calloc returns zero-initialized memory, realloc a
// preserved prefix plus an indeterminate tail -- and the sink side
// (ReleasesStorage) says whether the callee releases the storage its pointer
// argument denotes, making it [[now_uninit]]-equivalent at the binding site
// (free, realloc's pointer parameter, operator delete). A builtin ID is
// absent under -fno-builtin / -ffreestanding (or on a non-matching
// declaration), where an allocator falls back to the trusted default and a
// release callee goes unrecognized -- a missed diagnostic or a missed
// relaxation, never a false positive or a false acceptance elsewhere.
// Future known callees go here. (getBuiltinFunctionEffects in
// SemaFunctionEffects.cpp groups almost exactly this set for the
// `allocating` effect.)
struct AllocatorCalleeEntry {
  /// The Clang builtin ID this row matches; 0 for a form-keyed row.
  unsigned BuiltinID = 0;
  AllocatorCalleeRole Role;
  bool ReleasesStorage = false;
  /// The form predicate for callees with no builtin ID; null for
  /// builtin-keyed rows.
  bool (*Form)(const FunctionDecl *) = nullptr;
  /// The library name of a plain-spelled builtin row, compared by the
  /// untrusted name-only fallback (matchAllocatorCallee) when the builtin
  /// ID is absent; empty for the __builtin_* spellings (which always keep
  /// their IDs) and the form-keyed rows. A per-row literal rather than
  /// Builtin::Context::getName, which returns std::string by value.
  StringRef Name;
};

static constexpr AllocatorCalleeEntry AllocatorCallees[] = {
    {Builtin::BImalloc, AllocatorCalleeRole::ReturnsUninitialized,
     /*ReleasesStorage=*/false, /*Form=*/nullptr, "malloc"},
    {Builtin::BI__builtin_malloc, AllocatorCalleeRole::ReturnsUninitialized},
    {Builtin::BIaligned_alloc, AllocatorCalleeRole::ReturnsUninitialized,
     /*ReleasesStorage=*/false, /*Form=*/nullptr, "aligned_alloc"},
    {Builtin::BIalloca, AllocatorCalleeRole::ReturnsUninitialized,
     /*ReleasesStorage=*/false, /*Form=*/nullptr, "alloca"},
    {Builtin::BI__builtin_alloca, AllocatorCalleeRole::ReturnsUninitialized},
    {Builtin::BI__builtin_alloca_uninitialized,
     AllocatorCalleeRole::ReturnsUninitialized},
    {Builtin::BI__builtin_alloca_with_align,
     AllocatorCalleeRole::ReturnsUninitialized},
    {Builtin::BI__builtin_alloca_with_align_uninitialized,
     AllocatorCalleeRole::ReturnsUninitialized},
    {Builtin::BI__builtin_operator_new,
     AllocatorCalleeRole::ReturnsUninitialized},
    {Builtin::BIcalloc, AllocatorCalleeRole::ReturnsInitialized,
     /*ReleasesStorage=*/false, /*Form=*/nullptr, "calloc"},
    {Builtin::BI__builtin_calloc, AllocatorCalleeRole::ReturnsInitialized},
    {Builtin::BIrealloc, AllocatorCalleeRole::ReturnsUnknown,
     /*ReleasesStorage=*/true, /*Form=*/nullptr, "realloc"},
    {Builtin::BI__builtin_realloc, AllocatorCalleeRole::ReturnsUnknown,
     /*ReleasesStorage=*/true},
    {Builtin::BIfree, AllocatorCalleeRole::ReturnsNothing,
     /*ReleasesStorage=*/true, /*Form=*/nullptr, "free"},
    {Builtin::BI__builtin_free, AllocatorCalleeRole::ReturnsNothing,
     /*ReleasesStorage=*/true},
    {/*BuiltinID=*/0, AllocatorCalleeRole::ReturnsUninitialized,
     /*ReleasesStorage=*/false, isReplaceableGlobalOperatorNew},
    {/*BuiltinID=*/0, AllocatorCalleeRole::ReturnsNothing,
     /*ReleasesStorage=*/true, isReplaceableGlobalOperatorDelete},
};

// The table row for \p FD, if any -- the *trusted* match: the builtin ID
// (or the operator form) guarantees the row's semantics. Builtin rows are
// consulted first; no collision is possible -- plain operator new/delete
// never carry the table's builtin IDs, and __builtin_operator_new is not an
// operator name -- so the order is cosmetic.
static const AllocatorCalleeEntry *findAllocatorCallee(const FunctionDecl *FD) {
  if (unsigned ID = FD->getBuiltinID())
    for (const AllocatorCalleeEntry &Entry : AllocatorCallees)
      if (Entry.BuiltinID == ID)
        return &Entry;
  for (const AllocatorCalleeEntry &Entry : AllocatorCallees)
    if (Entry.Form && Entry.Form(FD))
      return &Entry;
  return nullptr;
}

// The table row for \p FD plus whether the match is trusted. A trusted
// match is findAllocatorCallee's. The fallback recognizes a plain-named
// declaration whose builtin ID is absent -- -fno-builtin / -ffreestanding,
// or a non-matching signature -- as an *untrusted* match: the name says
// what the function is meant to be, but its semantics cannot be assumed.
// The gate mirrors clang's builtin-attachment condition (a file-scope
// declaration with C language linkage; deliberately not isGlobal(), which
// a static member and ns::malloc both pass), and a callee that still
// carries any builtin ID is never re-matched by name (the __builtin_*
// spellings keep their IDs everywhere).
struct AllocatorCalleeMatch {
  const AllocatorCalleeEntry *Entry = nullptr;
  bool Trusted = false;
};

static AllocatorCalleeMatch matchAllocatorCallee(const FunctionDecl *FD) {
  if (const AllocatorCalleeEntry *Entry = findAllocatorCallee(FD))
    return {Entry, /*Trusted=*/true};
  if (FD->getBuiltinID() != 0)
    return {};
  const IdentifierInfo *II = FD->getDeclName().getAsIdentifierInfo();
  if (!II || !FD->getDeclContext()->getRedeclContext()->isFileContext() ||
      FD->getLanguageLinkage() != CLanguageLinkage)
    return {};
  StringRef Name = II->getName();
  for (const AllocatorCalleeEntry &Entry : AllocatorCallees)
    if (!Entry.Name.empty() && Entry.Name == Name)
      return {&Entry, /*Trusted=*/false};
  return {};
}

// Derive, once per binding, what \p FD does to the storage bound to its
// parameters: the lifetime attributes plus the allocator-callee table's
// ReleasesStorage rows (free, realloc's pointer parameter, replaceable
// global operator delete / operator delete[]), split by trust. Releasing
// storage leaves it as uninitialized as ending the object's lifetime does,
// so the binding sites treat a release callee's pointer parameter like a
// [[now_uninit]] one -- except for the double-destroy check:
// destroy_at(p); free(p); is correct, because ending an object's lifetime
// and releasing its storage are different operations. The trusted/by-name
// split preserves the lenient/strict query pair: acceptance may read the
// union (a declared free(p) should not reject its argument just because
// -fno-builtin stripped the ID), while withdrawal must key on the trusted
// bit only -- withdrawing on an untrusted name-only free could manufacture
// read-through false positives, while stale credit is the documented
// missed-diagnostic direction.
static SemaProfiles::CalleeLifecycleRoles
getCalleeLifecycleRoles(const FunctionDecl *FD) {
  SemaProfiles::CalleeLifecycleRoles Roles;
  Roles.InitializesRefToUninitParams = FD->hasAttr<NowInitAttr>();
  Roles.DestroysPointerParams = FD->hasAttr<NowUninitAttr>();
  AllocatorCalleeMatch M = matchAllocatorCallee(FD);
  if (M.Entry && M.Entry->ReleasesStorage) {
    if (M.Trusted)
      Roles.ReleasesStorageTrusted = true;
    else
      Roles.ReleasesStorageByName = true;
  }
  return Roles;
}

// A call to a [[ref_to_uninit]]-returning function yields uninitialized
// storage (the pointed-to memory, or the returned referent) -- deferred to
// Unknown when the marker is trusted (a store). Known allocator callees
// (the table above) are classified the same way without a marker. Any other
// unmarked direct callee is trusted Initialized (paper §4.3); a call with
// no direct callee (through a function pointer) is Unknown. Shared by both
// recognizers.
static UninitStorage classifyRefToUninitCallee(const CallExpr *CE,
                                               UninitAccessOpts Opts) {
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return UninitStorage::Unknown;
  bool RefersToUninit = FD->hasAttr<RefToUninitAttr>();
  if (AllocatorCalleeMatch M = matchAllocatorCallee(FD); M.Entry) {
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
      switch (M.Entry->Role) {
      case AllocatorCalleeRole::ReturnsUninitialized:
        RefersToUninit = true;
        break;
      case AllocatorCalleeRole::ReturnsInitialized:
        return UninitStorage::Initialized;
      case AllocatorCalleeRole::ReturnsUnknown:
        return UninitStorage::Unknown;
      case AllocatorCalleeRole::ReturnsNothing:
        // A void return never reaches these recognizers; keep the trusted
        // default if it somehow did.
        break;
      }
    }
  }
  if (!RefersToUninit)
    return UninitStorage::Initialized;
  return Opts.TrustRefToUninit ? UninitStorage::Unknown
                               : UninitStorage::Uninitialized;
}

// \p E is a pointer prvalue. Classifies whether it points to uninitialized
// storage.
static UninitStorage
pointerRefersToUninitStorage(ASTContext &Ctx, const Expr *E,
                             UninitAccessOpts Opts = UninitBindAccess) {
  if (!E)
    return UninitStorage::Unknown;
  E = E->IgnoreParenImpCasts();

  // An empty braced list value-initializes a pointer to null, so it takes the
  // null classification below (Unknown), keeping `= {}` and `= nullptr`
  // consistent.
  if (auto PassThrough = classifyUninitPassThrough(
          E, /*EmptyListState=*/UninitStorage::Unknown, [&](const Expr *Sub) {
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

  // &G, where G denotes uninitialized storage.
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    if (UO->getOpcode() == UO_AddrOf)
      return glvalueDenotesUninitStorage(Ctx, UO->getSubExpr(), Opts);

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
      // accepted missed diagnostic (parse-order leniency). Deliberately
      // excluded: globals/extern (an extern pointer may be initialized
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
    // Parse-order store credit: after a whole-`*p` store, the marked
    // pointer's pointee counts as initialized (paper §4.3: "p no longer
    // refers to uninitialized memory") for further whole-`*p` accesses --
    // until the pointer is reseated, which clears the credit. The consult
    // sits before the TrustRefToUninit branch (under the write preset the
    // outcome merely changes Unknown to Initialized, both "not
    // Uninitialized": no preset regression) and is skipped below an element
    // access (SubscriptBase), preserving §5.4's random-access ban.
    if (Opts.Credit && !Opts.SubscriptBase &&
        Opts.Credit->hasPointeeStoreCredit(VD, Opts.Strength))
      return UninitStorage::Initialized;
    return Opts.TrustRefToUninit ? UninitStorage::Unknown
                                 : UninitStorage::Uninitialized;
  }
  if (const auto *CE = dyn_cast<CallExpr>(E))
    return classifyRefToUninitCallee(CE, Opts);

  // A default-initialized new-expression (none init style: no initializer
  // written) whose allocated type's default-initialization leaves a scalar
  // subobject indeterminate produces uninitialized free-store memory (paper
  // §1.2/§4.3), like a [[ref_to_uninit]] allocator. The style gates this
  // rather
  // than hasInitializer(), which is also true for new Agg -- default-
  // initializing a class synthesizes a (possibly trivial) constructor call.
  // new T(...) / new T{} are value- or list-initialized; a user-provided
  // default constructor is trusted by defaultInitLeavesScalarIndeterminate.
  if (const auto *NE = dyn_cast<CXXNewExpr>(E)) {
    if (NE->getInitializationStyle() != CXXNewInitializationStyle::None)
      return UninitStorage::Initialized;
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
    return defaultInitLeavesScalarIndeterminateImpl(Ctx, NE->getAllocatedType(),
                                                    /*HonorUninitMarkers=*/true,
                                                    Visited)
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

// \p E is a glvalue. Classifies whether it denotes uninitialized storage.
static UninitStorage glvalueDenotesUninitStorage(ASTContext &Ctx, const Expr *E,
                                                 UninitAccessOpts Opts) {
  if (!E)
    return UninitStorage::Unknown;
  E = ignoreParenImpCastsKeepMTE(E);

  // A materialized temporary is a fresh object initialized from its
  // subexpression's *value*: whatever storage that value was loaded from, the
  // temporary itself is initialized (e.g. `const long &r = u;` binds a new
  // long temporary, not `u`). A *pointer-typed* temporary is the exception:
  // its value still refers to the same storage, so an unmarked copy of a
  // marked pointer value (`const int *const &rp = alloc();`) must keep
  // classifying by the pointee -- recurse as if the MTE were stripped, the
  // pre-MTE-arm status quo.
  if (const auto *MTE = dyn_cast<MaterializeTemporaryExpr>(E)) {
    if (MTE->getType()->isPointerType())
      return glvalueDenotesUninitStorage(Ctx, MTE->getSubExpr(), Opts);
    return UninitStorage::Initialized;
  }

  if (auto PassThrough = classifyUninitPassThrough(
          E, /*EmptyListState=*/UninitStorage::Unknown, [&](const Expr *Sub) {
            return glvalueDenotesUninitStorage(Ctx, Sub, Opts);
          }))
    return *PassThrough;

  // A named entity denotes uninitialized storage if it is [[uninit]], or
  // if it is a reference marked [[ref_to_uninit]] (the glvalue is its referent,
  // which is uninitialized). A [[ref_to_uninit]] *pointer* named here denotes
  // the pointer object itself -- which is initialized -- so it does not count.
  // Under the top-level drop the [[uninit]] arm is skipped: a value access of
  // a directly named [[uninit]] object is the flow-based passes' territory, so
  // only a [[ref_to_uninit]] reference (or indirection, handled below) still
  // counts. Parse-order store credit clears both arms: a whole-entity store
  // is the [[uninit]] entity's initialization (paper §4.2/§4.5), and a store
  // through a marked reference initializes its referent (§4.3; references
  // cannot be reseated, so that credit never lapses).
  auto DeclDenotesUninit = [&](const ValueDecl *VD) {
    return (!Opts.DropTopLevelUninit && VD->hasAttr<UninitAttr>() &&
            !(Opts.Credit &&
              Opts.Credit->hasWholeObjectStoreCredit(VD, Opts.Strength))) ||
           (!Opts.TrustRefToUninit && VD->getType()->isReferenceType() &&
            VD->hasAttr<RefToUninitAttr>() &&
            !(Opts.Credit &&
              Opts.Credit->hasPointeeStoreCredit(VD, Opts.Strength)));
  };
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DeclDenotesUninit(DRE->getDecl()) ? UninitStorage::Uninitialized
                                             : UninitStorage::Initialized;
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    // a->m reaches m through the pointer a (object *a); a.m through the
    // glvalue a. When m does not itself denote uninit storage, the subobject is
    // uninit exactly when its base is. The base recursion clears the top-level
    // drop: the drop exists because a directly named [[uninit]] entity's value
    // accesses are owned by the flow passes or deliberately trusted (see the
    // UninitAccessOpts comment above), but nothing tracks a subobject reached
    // through a *further* member access -- and member-wise delayed
    // initialization of an [[uninit]] object is itself banned (paper §5.4;
    // only whole-object construct_at re-initializes, which is uniformly
    // unmodeled) -- so below the top level the marker counts for every access.
    // The base recursion clears the marker trust for the same reason: the
    // write preset trusts the marker only at the top level, where a scalar
    // write is the whole pointee's initialization (§4.5) -- below a member
    // step the write initializes nothing, so [[ref_to_uninit]] markers,
    // marked callees, and the allocator tail all count again.
    //
    // Parse-order member store credit: after `a.m = 5` / `this->m = 5`, the
    // marked member of that *specific* base object counts as initialized
    // (paper §4.2: "After initialization, the object is no longer
    // [[uninit]]"; §6: assignment initializes a built-in) -- covering both
    // `a.m` (a reference binding lands in this arm directly) and `&a.m` (the
    // UO_AddrOf arm recurses here). The consult keys on the same base
    // identity the recording resolved, so the same member observed through
    // any other object -- including a copy (§5.2: a copy does not inherit
    // credit) -- stays uncredited. It applies at any chain depth: the map
    // only ever holds whole-member stores (which initialize the entire
    // member), and in practice only scalar members (see
    // recordInitProfileStore), which have no subobjects to chain through.
    const ValueDecl *MD = ME->getMemberDecl();
    if (const auto *F = dyn_cast<FieldDecl>(MD);
        F && Opts.Credit && F->hasAttr<UninitAttr>() &&
        Opts.Credit->hasMemberStoreCredit(
            Opts.Credit->resolveMemberStoreBase(ME), F, Opts.Strength))
      return UninitStorage::Initialized;
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
  // A call to a [[ref_to_uninit]]-returning reference function: the referent
  // it returns is uninitialized.
  if (const auto *CE = dyn_cast<CallExpr>(E))
    return classifyRefToUninitCallee(CE, Opts);
  // An element access classifies like its base, but pointee store credit
  // must not apply below it (SubscriptBase): `*p = 5;` never legalizes
  // `p[1]` -- the pointee may be an array with only element 0 written, and
  // element-wise state is untrackable by design (paper §5.4/§5.5).
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return pointerRefersToUninitStorage(Ctx, ASE->getBase(),
                                        Opts.withSubscriptBase());

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

// Dispatches a binding source to the pointer or glvalue recognizer.
static UninitStorage
classifyUninitSource(ASTContext &Ctx, const Expr *E, bool IsReference,
                     UninitAccessOpts Opts = UninitBindAccess) {
  return IsReference ? glvalueDenotesUninitStorage(Ctx, E, Opts)
                     : pointerRefersToUninitStorage(Ctx, E, Opts);
}

bool SemaProfiles::refersToUninitializedMemory(const Expr *E,
                                               bool IsReference) const {
  return classifyUninitSource(getASTContext(), E, IsReference,
                              UninitBindAccess.withCredit(this)) ==
         UninitStorage::Uninitialized;
}

void SemaProfiles::checkInitProfileRefToUninit(SourceLocation Loc,
                                               bool TargetIsRefToUninit,
                                               bool IsReference,
                                               const Expr *Src, const Decl *D) {
  // A RecoveryExpr is a placeholder for an initialization that already failed,
  // not a source the user wrote, so it must not drive this rule.
  if (!Src || isa<RecoveryExpr>(Src->IgnoreParens()))
    return;
  // An instantiation-dependent source cannot be classified yet; its construct
  // is always rebuilt at instantiation, re-running this funnel with the
  // substituted source. A non-dependent source is checked here, at definition
  // time; if the construct is rebuilt at instantiation anyway (a local
  // operand, a call argument, a return), the same diagnostic repeats there --
  // accepted for now. Decl-carrying callers are exempt: they defer via the
  // D->isTemplated() check in shouldEmitProfileViolation and fire on the
  // instantiated declaration.
  if (!D && Src->isInstantiationDependent())
    return;
  static constexpr StringRef Profile = "std::init";
  if (!shouldEmitProfileViolation(diag::err_init_uninit_requires_ref_to_uninit,
                                  Loc, D))
    return;
  // Credit is asymmetric between the two directions. For an unmarked
  // target, credit only ever *suppresses* the diagnostic, so any store
  // earlier in parse order suffices: Maybe. For a marked target the
  // diagnostic *fires on* credit ("must refer to uninitialized memory"),
  // which is only sound when the store provably ran, so the consult is
  // Definite: an unconditional store in the entity's own function. A store
  // under an if/loop/lambda earns only Maybe credit and cannot reject a
  // legal marked binding on the untaken path -- never a false positive.
  //
  // While instantiating, the requires-uninit direction classifies without
  // credit entirely: parse-order credit is recorded once and never rewound,
  // so when an instantiation re-walks a statement the pattern already
  // checked, the re-check runs against post-pattern state -- including
  // credit this very statement recorded (a reused [[now_init]] argument, a
  // this-member store keyed to the pattern), which would turn the pattern's
  // pass into a false "must refer to uninitialized memory". The reverse
  // direction diagnoses at definition time, and a violation established
  // only by credit in fully dependent code is a missed diagnostic -- the
  // usual parse-order trade, never a false positive. Direct classification
  // (an initialized global, a marked entity) is unaffected, so credit-free
  // reverse violations still repeat per instantiation, and the accepting
  // direction keeps credit everywhere (a deferred binding needs the
  // instantiation-time record to pass).
  UninitAccessOpts Opts = UninitBindAccess;
  if (!TargetIsRefToUninit)
    Opts = Opts.withCredit(this, InitCreditStrength::Maybe);
  else if (!SemaRef.inTemplateInstantiation())
    Opts = Opts.withCredit(this, InitCreditStrength::Definite);
  UninitStorage SrcState =
      classifyUninitSource(getASTContext(), Src, IsReference, Opts);
  unsigned IsRef = IsReference ? 1 : 0;
  // A marked target is a violation only against an affirmatively Initialized
  // source: an Unknown one (pointer arithmetic, an integer-to-pointer cast, a
  // call through a function pointer) cannot be proven initialized, so rejecting
  // it would be a false positive. An unmarked target is diagnosed only against
  // an affirmatively Uninitialized source (Unknown stays a missed diagnostic).
  if (TargetIsRefToUninit && SrcState == UninitStorage::Initialized)
    Diag(Loc, diag::err_init_ref_to_uninit_requires_uninit) << Profile << IsRef;
  else if (!TargetIsRefToUninit && SrcState == UninitStorage::Uninitialized)
    Diag(Loc, diag::err_init_uninit_requires_ref_to_uninit) << Profile << IsRef;
}

void SemaProfiles::checkInitProfileRefToUninitBinding(SourceLocation Loc,
                                                      const ValueDecl *Target,
                                                      QualType T,
                                                      const Expr *Src,
                                                      const Decl *D) {
  if (!getLangOpts().Profiles || T.isNull() || T->isDependentType() ||
      (!T->isPointerType() && !T->isReferenceType()))
    return;
  const auto *Parm = dyn_cast_or_null<ParmVarDecl>(Target);
  const auto *Callee =
      Parm ? dyn_cast<FunctionDecl>(Parm->getDeclContext()) : nullptr;
  // The callee's lifecycle roles, derived once; every consumer below reads
  // only the bits its direction may rely on (see CalleeLifecycleRoles).
  CalleeLifecycleRoles Roles =
      Callee ? getCalleeLifecycleRoles(Callee) : CalleeLifecycleRoles();
  if (Roles.DestroysPointerParams || Roles.ReleasesStorageTrusted ||
      Roles.ReleasesStorageByName) {
    // A [[now_uninit]] callee's pointer/reference parameter -- and a known
    // storage-release callee's (free, realloc's pointer, replaceable
    // global operator delete), which is [[now_uninit]]-equivalent here --
    // accepts storage in any live state instead of the marker-consistency
    // check: the attribute declares destruction, whose operand is
    // initialized memory for a plain destructor-like callee, *any* state
    // for a raw-release one (free takes storage that may never have been
    // constructed; see the Limitations note), and initialized storage is
    // precisely what a dual-attributed reinitializer's destroy half exists
    // for. Acceptance never diagnoses, so it reads the union of the
    // release bits -- an untrusted name-only free still relaxes. Two
    // states a [[now_uninit]] callee must not take, though. Storage
    // *already destroyed*: a second destruction is P4222R2 §1's
    // double-destroy error, and the destroyed state is definite by
    // construction, so it may fire a diagnostic -- that applies to a
    // reinitializer too, whose destroy half is invalid on destroyed
    // storage; construct_at (a plain [[now_init]] function with a marked
    // parameter, whose binding check below is untouched) is the
    // sanctioned recovery path. And storage still (or again)
    // *uninitialized*: destruction makes an object uninitialized, so a
    // first destroy of never-constructed storage is as much an access to
    // raw memory as a second one ("Lifetimes", p4222r2.md:922-927 -- "it
    // is an error to uninitialize an object twice"). The argument is
    // classified exactly like an unmarked binding target
    // (affirmative-Uninitialized with Maybe credit), so Unknown never
    // fires and a conditional store suppresses. Both checks key on the
    // destroy role alone -- a storage-release callee is exempt from both:
    // destroy_at(p); free(p); is correct (different operations, correctly
    // ordered), and free takes never-constructed storage by contract. A
    // reinitializer (dual [[now_init]] [[now_uninit]]) is exempt from
    // destroy_uninit call-wide: its marked parameter positively legalizes
    // uninitialized sources (destroy-then-construct on fresh storage is
    // its purpose), and the exemption also covers its unmarked
    // destroy-only pointer parameters, whose storage the paper requires
    // live -- a missed diagnostic, not a rule. A parameter that itself
    // carries [[ref_to_uninit]] is exempt per parameter, whatever the
    // callee's other attributes: the marker is the author's declaration
    // that the parameter takes possibly-uninitialized storage -- the
    // annotation spelling for an unrecognized storage-release function
    // (the Limitations workaround for _aligned_free and kin), whose
    // contract a destroy-of-uninitialized rejection would break for
    // never-written buffers. The branch below keys on
    // the STATE, not on diagnostic emission: destroyed storage
    // re-classifies affirmatively Uninitialized (destruction withdrew its
    // credit), so a suppressed double destroy must stay silent rather
    // than fall through to a swapped destroy_uninit error. An
    // instantiation-dependent source defers exactly like
    // checkInitProfileRefToUninit's.
    bool Checkable = Src && !isa<RecoveryExpr>(Src->IgnoreParens()) &&
                     (D || !Src->isInstantiationDependent());
    bool Destroyed =
        Checkable && Roles.DestroysPointerParams && storageIsDestroyed(T, Src);
    if (Destroyed) {
      if (shouldEmitProfileViolation(diag::err_init_double_destroy, Loc, D))
        Diag(Loc, diag::err_init_double_destroy) << "std::init";
    } else if (Roles.DestroysPointerParams &&
               !Roles.InitializesRefToUninitParams &&
               !(Target && Target->hasAttr<RefToUninitAttr>()) && Checkable &&
               shouldEmitProfileViolation(diag::err_init_destroy_uninit, Loc,
                                          D) &&
               classifyUninitSource(getASTContext(), Src, T->isReferenceType(),
                                    UninitBindAccess.withCredit(
                                        this, InitCreditStrength::Maybe)) ==
                   UninitStorage::Uninitialized)
      Diag(Loc, diag::err_init_destroy_uninit) << "std::init";
  } else {
    // A null Target is a binding site with no declaration to carry the
    // marker (a parameter of a call through a function pointer): always
    // unmarked.
    checkInitProfileRefToUninit(Loc,
                                Target && Target->hasAttr<RefToUninitAttr>(),
                                T->isReferenceType(), Src, D);
  }
  recordLifecycleArguments(Roles, Target, T, Src);
}

void SemaProfiles::recordLifecycleArguments(const CalleeLifecycleRoles &Roles,
                                            const ValueDecl *Target, QualType T,
                                            const Expr *Src) {
  // Runs after the binding check: the binding itself is judged against the
  // *pre-call* state, and only then does a [[now_init]] callee's promised
  // initialization -- or a [[now_uninit]] callee's promised destruction --
  // take effect for what follows in parse order. The withdrawal runs before
  // the credit so a callee carrying both attributes (a reinitializer) nets
  // to destroy-then-construct: the storage is initialized after the call.
  // The alias escape runs last and is independent of the callee's roles.
  recordNowUninitArgument(Roles, Target, T, Src);
  recordNowInitArgument(Roles, Target, T, Src);
  recordInitProfilePointerAliasEscape(T, Src);
}

void SemaProfiles::recordNowInitArgument(const CalleeLifecycleRoles &Roles,
                                         const ValueDecl *Target, QualType T,
                                         const Expr *Src) {
  // Only the binding of a [[ref_to_uninit]] parameter of a [[now_init]]
  // function carries the callee's initialization promise (P4222R2 §6.2: the
  // attribute "would apply to every [[ref_to_uninit]] argument"). A variadic
  // argument, an unmarked parameter, or a call through a function pointer
  // presents no marked ParmVarDecl and earns nothing.
  if (!Roles.InitializesRefToUninitParams)
    return;
  const auto *Parm = dyn_cast_or_null<ParmVarDecl>(Target);
  if (!Parm || !Src || !Parm->hasAttr<RefToUninitAttr>())
    return;
  // Like recordInitProfileStore: no enforcement, suppression, or in-template
  // gate (a suppressed or pattern-parsed call still initializes; rebuilt
  // instantiations re-record against fresh declarations), but a call in a
  // never-executed context earns no credit.
  if (inNeverExecutedContext())
    return;
  recordLifetimeAnnotatedArgument(T, Src, LifetimeAnnotationEffect::Construct);
}

void SemaProfiles::recordNowUninitArgument(const CalleeLifecycleRoles &Roles,
                                           const ValueDecl *Target, QualType T,
                                           const Expr *Src) {
  // The mirror of recordNowInitArgument: a [[now_uninit]] callee ends the
  // lifetime of the storage bound to each of its pointer/reference
  // parameters (P4222R2 §4.4's missing destroy_at recording). The
  // parameters are *unmarked* -- destruction takes initialized memory -- so
  // the gate keys on the callee's role, not a parameter marker; the shape
  // walk is marker-keyed on the source side, so an ordinary initialized
  // argument withdraws nothing. A variadic argument or a call through a
  // function pointer presents no ParmVarDecl and withdraws nothing (stale
  // credit is a missed diagnostic, never a false positive -- the same
  // boundary as [[now_init]]).
  //
  // A known storage-release callee (free, realloc's pointer parameter,
  // replaceable global operator delete) withdraws like a [[now_uninit]]
  // one: the released storage no longer holds the object the credit
  // described, so a whole-`*q` read through a marked pointer after free(q)
  // classifies uninitialized again. (Unmarked pointers stay untracked;
  // use-after-free through them is the invalidation profile's job.) Only
  // the *trusted* release bit may withdraw (see CalleeLifecycleRoles).
  if (!Roles.DestroysPointerParams && !Roles.ReleasesStorageTrusted)
    return;
  const auto *Parm = dyn_cast_or_null<ParmVarDecl>(Target);
  if (!Parm || !Src)
    return;
  // Not enforcement- or suppression-gated (a suppressed destroy still
  // destroys), but a call in a never-executed context destroys nothing.
  if (inNeverExecutedContext())
    return;
  // A [[now_uninit]] callee ends the object's lifetime -- recording the
  // destroyed state double_destroy fires on -- while a plain release callee
  // only releases the storage: free(p); destroy_at(p); must not trip
  // double_destroy on free's account. A dual-attributed callee stays a
  // destroy.
  recordLifetimeAnnotatedArgument(T, Src,
                                  Roles.DestroysPointerParams
                                      ? LifetimeAnnotationEffect::Destroy
                                      : LifetimeAnnotationEffect::Release);
}

void SemaProfiles::recordInitProfilePointerAliasEscape(QualType T,
                                                       const Expr *Src) {
  // A mutable alias of a marked pointer object: T*& binds the pointer
  // glvalue itself, T** its address -- the funnel's pointer/reference type
  // gate passes both, and getPointeeType() works uniformly. The
  // pointee-of-the-binding must be a non-const pointer: whoever holds the
  // alias can reseat the pointer, so the Definite pointee credit (the
  // firing basis) is withdrawn while the suppressing Maybe credit survives.
  // (At Maybe strength the destroyed state is never recorded, so
  // EndsLifetime is inert here.)
  if (!Src || T.isNull() || (!T->isPointerType() && !T->isReferenceType()))
    return;
  QualType Pointee = T->getPointeeType();
  if (Pointee.isNull() || !Pointee->isPointerType() ||
      Pointee.isConstQualified())
    return;
  // The recorders' shared gate: an escape in a never-executed context
  // escapes nothing.
  if (inNeverExecutedContext())
    return;
  const Expr *E = ignoreTransparentCasts(Src);
  if (T->isPointerType()) {
    // T**: peel the &p to reach the pointer object.
    const auto *UO = dyn_cast<UnaryOperator>(E);
    if (!UO || UO->getOpcode() != UO_AddrOf)
      return;
    E = UO->getSubExpr();
  }
  if (const VarDecl *VD = getCreditableMarkedPointer(E))
    StoreCredit.destroyPointee(VD, InitCreditStrength::Maybe,
                               /*EndsLifetime=*/false);
}

void SemaProfiles::recordInitProfilePointerAliasEscape(const ValueDecl *Var) {
  // The by-reference-capture flavor: the closure holds a mutable alias of
  // the marked pointer object and can reseat it, exactly like `T **pp = &p`
  // above -- withdraw the Definite firing basis, keep the suppressing Maybe
  // credit. A const-qualified pointer cannot be reseated (mirror the Expr
  // flavor's mutable-alias gate), and only a creditable local/parameter
  // pointer has credit to withdraw.
  const auto *VD = dyn_cast_or_null<VarDecl>(Var);
  if (!VD || !VD->hasLocalStorage() || !VD->getType()->isPointerType() ||
      VD->getType().isConstQualified() || !VD->hasAttr<RefToUninitAttr>())
    return;
  // The recorders' shared gate: an escape in a never-executed context
  // escapes nothing.
  if (inNeverExecutedContext())
    return;
  StoreCredit.destroyPointee(VD, InitCreditStrength::Maybe,
                             /*EndsLifetime=*/false);
}

SemaProfiles::LifetimeAnnotatedStorage
SemaProfiles::resolveLifetimeAnnotatedStorage(QualType T,
                                              const Expr *Src) const {
  // Mirror the recognizers' explicit-cast pass-through
  // (ignoreTransparentCasts): the callee affects the same storage either
  // way.
  const Expr *E = ignoreTransparentCasts(Src);
  // The glvalue whose storage the callee affects: the operand of &G for a
  // pointer parameter, the bound glvalue itself for a reference one, or --
  // for fill(arr) -- the array glvalue the stripped decay leaves behind
  // (binding acceptance already has a dedicated decayed-array arm; the
  // credit side resolves the same storage, so accept and credit agree). An
  // element-address argument (fill(&arr[0])) still resolves nothing --
  // §5.4's element ban -- and a file-scope [[uninit]] array fails the
  // hasLocalStorage gate below like every other non-local.
  const Expr *Glvalue = nullptr;
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_AddrOf)
    Glvalue = UO->getSubExpr()->IgnoreParenImpCasts();
  else if (T->isReferenceType())
    Glvalue = E;
  else if (E->getType()->isArrayType())
    Glvalue = E;
  if (Glvalue) {
    // &base.m / base.m: the per-object member shape, under exactly the
    // member-store keys (resolveMemberStoreBase); an untrackable base -- a
    // parameter-reached object, a deeper chain -- resolves null and stays
    // strict, the same boundary as a direct store.
    if (const auto *ME = dyn_cast<MemberExpr>(Glvalue)) {
      if (const auto *F = dyn_cast<FieldDecl>(ME->getMemberDecl());
          F && F->hasAttr<UninitAttr>())
        if (const Decl *Base = resolveMemberStoreBase(ME))
          return LifetimeAnnotatedStorage::member(Base, F);
      return {};
    }
    // &*p / *p: the pointee of a marked pointer.
    if (const auto *UO = dyn_cast<UnaryOperator>(Glvalue);
        UO && UO->getOpcode() == UO_Deref) {
      if (const VarDecl *VD = getCreditableMarkedPointer(UO->getSubExpr()))
        return LifetimeAnnotatedStorage::pointee(VD);
      return {};
    }
    if (const auto *VD =
            dyn_cast_or_null<VarDecl>(getDirectlyNamedDecl(Glvalue));
        VD && VD->hasLocalStorage()) {
      // &u / u: the whole [[uninit]] entity, exactly the storage `u = e`
      // would credit.
      if (VD->hasAttr<UninitAttr>())
        return LifetimeAnnotatedStorage::whole(VD);
      // r (a marked reference bound onward): its referent; a reference
      // cannot be reseated, so no store ever clears that credit -- only a
      // [[now_uninit]] callee's withdrawal does.
      if (VD->getType()->isReferenceType() && VD->hasAttr<RefToUninitAttr>())
        return LifetimeAnnotatedStorage::pointee(VD);
    }
    return {};
  }
  // p as a pointer value: p's pointee -- §6.2's initialize2(p) example
  // verbatim. Only a directly named marked local/parameter pointer is
  // trackable; reseating p afterwards clears pointee credit like any other.
  if (const VarDecl *VD = getCreditableMarkedPointer(E))
    return LifetimeAnnotatedStorage::pointee(VD);
  return {};
}

void SemaProfiles::recordLifetimeAnnotatedArgument(
    QualType T, const Expr *Src, LifetimeAnnotationEffect Effect) {
  // A [[now_init]] callee's initialization marks the resolved storage
  // stored; a [[now_uninit]] callee's destruction -- or a release callee's
  // deallocation -- clears the mark. All directions share one strength: for
  // the credit it is the store's certainty (only an unconditional
  // same-function call may fire the requires-uninit direction), for the
  // withdrawal the destroy's -- a conditional destroy may or may not have
  // run, so it kills only the Definite claim and leaves the
  // suppression-only Maybe credit in place (see recordNowUninitArgument).
  // Only a Destroy records the destroyed state (LifetimeAnnotationEffect).
  bool Withdraw = Effect != LifetimeAnnotationEffect::Construct;
  bool EndsLifetime = Effect == LifetimeAnnotationEffect::Destroy;
  LifetimeAnnotatedStorage Storage = resolveLifetimeAnnotatedStorage(T, Src);
  switch (Storage.StorageKind) {
  case LifetimeAnnotatedStorage::Kind::None:
    break;
  case LifetimeAnnotatedStorage::Kind::Whole:
    if (Withdraw)
      StoreCredit.destroyWhole(
          Storage.Entity, currentStoreStrength(Storage.Entity), EndsLifetime);
    else
      StoreCredit.markWholeStored(Storage.Entity,
                                  currentStoreStrength(Storage.Entity));
    break;
  case LifetimeAnnotatedStorage::Kind::Pointee:
    if (Withdraw)
      StoreCredit.destroyPointee(
          Storage.Entity, currentStoreStrength(Storage.Entity), EndsLifetime);
    else
      StoreCredit.markPointeeStored(Storage.Entity,
                                    currentStoreStrength(Storage.Entity));
    break;
  case LifetimeAnnotatedStorage::Kind::Member:
    if (Withdraw)
      StoreCredit.destroyMember(Storage.Base, Storage.Field,
                                currentStoreStrength(Storage.Base),
                                EndsLifetime);
    else
      StoreCredit.markMemberStored(Storage.Base, Storage.Field,
                                   currentStoreStrength(Storage.Base));
    break;
  }
}

void SemaProfiles::checkInitProfileDeleteOperand(const Expr *Operand) {
  // The delete-expression twin of a storage-release callee's binding: the
  // operand's storage is released, so its credit is withdrawn -- a later
  // whole-`*q` read through a marked pointer classifies uninitialized
  // again -- with no binding diagnostic, matching both the funnel's
  // release relaxation and this expression's historical silence. An
  // instantiation-dependent operand defers: TreeTransform re-invokes
  // ActOnCXXDelete at instantiation, re-running this hook with the
  // substituted operand.
  if (!getLangOpts().Profiles || !Operand ||
      Operand->isInstantiationDependent())
    return;
  // A delete in a never-executed context releases nothing (the recorders'
  // shared gate).
  if (inNeverExecutedContext())
    return;
  // A Release, like operator delete's binding: the storage is gone, but no
  // destroyed state is recorded -- delete p; destroy_at(p); is the
  // invalidation profile's problem, not double_destroy's.
  recordLifetimeAnnotatedArgument(Operand->getType(), Operand,
                                  LifetimeAnnotationEffect::Release);
}

bool SemaProfiles::storageIsDestroyed(QualType T, const Expr *Src) const {
  LifetimeAnnotatedStorage Storage = resolveLifetimeAnnotatedStorage(T, Src);
  switch (Storage.StorageKind) {
  case LifetimeAnnotatedStorage::Kind::None:
    return false;
  case LifetimeAnnotatedStorage::Kind::Whole:
    return StoreCredit.isWholeDestroyed(Storage.Entity);
  case LifetimeAnnotatedStorage::Kind::Pointee:
    return StoreCredit.isPointeeDestroyed(Storage.Entity);
  case LifetimeAnnotatedStorage::Kind::Member:
    return StoreCredit.isMemberDestroyed(Storage.Base, Storage.Field);
  }
  llvm_unreachable("unknown LifetimeAnnotatedStorage kind");
}

void SemaProfiles::checkInitProfileVariadicArgument(const Expr *Arg) {
  // std::init / ref_to_uninit (paper §5): a variadic argument never reaches
  // parameter copy-initialization, and a `...` parameter cannot carry
  // [[ref_to_uninit]], so a pointer passed through it is checked as an
  // unmarked target (paper §7.2: passing uninitialized memory needs an
  // appropriately declared callee). Value reads of the promoted argument
  // already funnel through the lvalue-to-rvalue chokepoint; the pointer
  // binding is the only direction added here. Called from the two C++
  // variadic promotion loops -- Sema::GatherArgumentsForCall and
  // Sema::BuildCallToObjectOfClassType (functors, variadic lambdas) -- not
  // from Sema::DefaultVariadicArgumentPromotion itself, whose other callers
  // re-promote already-promoted arguments (the os_log builtin check) or
  // promote during ObjC method matching, where a check would double-fire.
  if (!getLangOpts().Profiles || !Arg || !Arg->getType()->isPointerType())
    return;
  checkInitProfileRefToUninit(Arg->getExprLoc(), /*TargetIsRefToUninit=*/false,
                              /*IsReference=*/false, Arg);
}

void SemaProfiles::checkInitProfileRefCapture(SourceLocation Loc,
                                              const ValueDecl *Var) {
  if (!getLangOpts().Profiles)
    return;
  // A by-reference capture of a mutable marked *pointer* escapes it to the
  // closure, which can reseat it -- withdraw the Definite pointee credit
  // like `T **pp = &p` would (the overload's own gates limit this to that
  // shape). Recorded before the diagnostic early-returns below: a marked
  // non-reference pointer is not this check's to diagnose and returns early,
  // but its escape must still be recorded (recorders never gate).
  recordInitProfilePointerAliasEscape(Var);
  // Mirrors the glvalue recognizer's named-entity arm: the captured variable
  // denotes uninitialized storage if it is [[uninit]], or if it is a
  // [[ref_to_uninit]] reference (the capture binds to its referent) -- in
  // both cases unless parse-order store credit says it has been initialized
  // (u = 5; then a by-ref capture of u is accepted, symmetric with &u). A
  // copy capture is not this check's: it reads the variable in the enclosing
  // function's CFG, which is the flow-based uninit_read pass's territory.
  bool UninitNoCredit =
      Var->hasAttr<UninitAttr>() &&
      !hasWholeObjectStoreCredit(Var, InitCreditStrength::Maybe);
  bool RefNoCredit = Var->getType()->isReferenceType() &&
                     Var->hasAttr<RefToUninitAttr>() &&
                     !hasPointeeStoreCredit(Var, InitCreditStrength::Maybe);
  if (!UninitNoCredit && !RefNoCredit)
    return;
  // The only Expr-less deferral here: an instantiation-dependent captured
  // type defers to instantiation, where TreeTransform's unconditional lambda
  // rebuild re-processes the capture. A concrete capture fires at definition
  // time and repeats on that same rebuild -- accepted for now.
  if (Var->getType()->isInstantiationDependentType())
    return;
  if (!shouldEmitProfileViolation(diag::err_init_uninit_ref_capture, Loc))
    return;
  Diag(Loc, diag::err_init_uninit_ref_capture) << "std::init" << Var;
}

void SemaProfiles::checkInitProfileObjectArgument(const Expr *Object,
                                                  const CXXMethodDecl *Method) {
  // A RecoveryExpr is a placeholder for an expression that already failed, not
  // an object argument the user wrote, so it must not drive this rule.
  if (!getLangOpts().Profiles || !Object ||
      isa<RecoveryExpr>(Object->IgnoreParens()))
    return;
  // Destroying uninitialized storage is the deferred destroy_at slice (the
  // paper models destruction, like construct_at, as a lifetime operation);
  // implicit scope-exit destructions never reach this funnel, so diagnosing
  // only the explicit s.~S() spelling would be an inconsistent sliver.
  if (isa<CXXDestructorDecl>(Method))
    return;
  // A static call operator (C++23) has no implicit object parameter: its
  // object argument is evaluated but its value is never used, exactly like a
  // static member function named through an object -- whose call path never
  // reaches this funnel. BuildCallToObjectOfClassType converts the object
  // argument for static call operators all the same, so skip them here.
  if (Method->isStatic())
    return;
  // An instantiation-dependent object argument cannot be classified yet; its
  // call is always rebuilt at instantiation, re-running this funnel with the
  // substituted object. A non-dependent call fires at definition time and
  // repeats if the call is rebuilt at instantiation anyway -- accepted.
  if (Object->isInstantiationDependent())
    return;
  if (!shouldEmitProfileViolation(diag::err_init_member_call_on_uninit,
                                  Object->getExprLoc()))
    return;
  // An arrow call's object argument arrives as the pointer expression, a dot
  // call's as the object glvalue; dispatch the recognizer accordingly.
  bool IsPointer = Object->getType()->isPointerType();
  if (!refersToUninitializedMemory(Object, /*IsReference=*/!IsPointer))
    return;
  Diag(Object->getExprLoc(), diag::err_init_member_call_on_uninit)
      << "std::init" << Method;
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
      return FD && FD->hasAttr<RefToUninitAttr>();
    }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return DRE->getDecl()->hasAttr<RefToUninitAttr>();
    return false;
  }
}

void SemaProfiles::checkInitProfileReadThrough(SourceLocation Loc,
                                               const Expr *Glvalue,
                                               QualType ValueType) {
  // Enforcement first: the lvalue-to-rvalue chokepoint calls this for every
  // load (behind only the call site's LangOpts.Profiles gate), so nothing
  // below should run without the profile. The hoisted gate is
  // shouldEmitProfileViolation's own first conjunct, evaluated in the same
  // call with only const queries in between, so it cannot change any
  // answer.
  if (!isProfileEnforced("std::init"))
    return;
  // A RecoveryExpr is a placeholder for an expression that already failed, not
  // a read the user wrote, so it must not drive this rule.
  if (!Glvalue || isa<RecoveryExpr>(Glvalue->IgnoreParens()))
    return;
  // An instantiation-dependent glvalue cannot be classified yet; its read is
  // always rebuilt at instantiation, where this check re-runs with the
  // substituted operand. A non-dependent read fires at definition time and
  // repeats if the read is rebuilt at instantiation anyway -- accepted.
  if (Glvalue->isInstantiationDependent())
    return;
  // Paper §4.5: reading an uninitialized std::byte is permitted.
  if (getASTContext().getBaseElementType(ValueType)->isStdByteType())
    return;
  if (!shouldEmitProfileViolation(diag::err_init_uninit_read_through, Loc))
    return;
  if (glvalueDenotesUninitStorage(getASTContext(), Glvalue,
                                  UninitReadAccess.withCredit(this)) !=
      UninitStorage::Uninitialized)
    return;
  Diag(Loc, diag::err_init_uninit_read_through)
      << "std::init" << (isMemberChainOfUninitObject(Glvalue) ? 1 : 0);
}

void SemaProfiles::checkInitProfileSubobjectWrite(SourceLocation Loc,
                                                  const Expr *LHS) {
  // A RecoveryExpr is a placeholder for an expression that already failed, not
  // a store the user wrote, so it must not drive this rule.
  if (!LHS || isa<RecoveryExpr>(LHS->IgnoreParens()))
    return;
  // An instantiation-dependent store target cannot be classified yet; its
  // assignment is always rebuilt at instantiation, where this check re-runs
  // with the substituted LHS. A non-dependent store fires at definition time
  // and repeats if the assignment is rebuilt at instantiation anyway --
  // accepted.
  if (LHS->isInstantiationDependent())
    return;
  // Paper §4.5: an uninitialized std::byte may be manipulated freely.
  if (getASTContext().getBaseElementType(LHS->getType())->isStdByteType())
    return;
  if (!shouldEmitProfileViolation(diag::err_init_uninit_subobject_write, Loc))
    return;
  if (glvalueDenotesUninitStorage(getASTContext(), LHS,
                                  UninitWriteAccess.withCredit(this)) !=
      UninitStorage::Uninitialized)
    return;
  // The provenance phrase: a member chain of a named [[uninit]] object
  // renders exactly as before the marked/allocator arms existed (arm 0);
  // isMemberChainOfUninitObject alone cannot key the split -- it is false
  // for allocator and new-expression provenance, which needs arm 2's
  // marker-free wording.
  Diag(Loc, diag::err_init_uninit_subobject_write)
      << "std::init" << !isa<MemberExpr>(LHS->IgnoreParenImpCasts())
      << (isMemberChainOfUninitObject(LHS)
              ? 0
              : (uninitWriteChainSeesMarker(LHS) ? 1 : 2));
}

void SemaProfiles::checkInitProfilePointerAssignment(Expr *LHS, Expr *RHS,
                                                     SourceLocation OpLoc) {
  // References cannot be reseated, so only pointer assignment applies. The
  // marker is read when the LHS directly names a pointer entity; any other
  // lvalue (e.g. *pp, arr[i]) cannot carry a local marker, so it is the
  // default unmarked pointer (paper §4.3) and must not be bound to
  // uninitialized memory.
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
  const ValueDecl *VD = getDirectlyNamedDecl(ignoreTransparentCasts(LHS));
  checkInitProfileRefToUninit(OpLoc, VD && VD->hasAttr<RefToUninitAttr>(),
                              /*IsReference=*/false, RHS);
  // An assignment can hand out a mutable alias of a marked pointer object
  // (pp = &p) exactly like a binding does; the withdrawal mirrors the
  // funnel's recorder tail.
  recordInitProfilePointerAliasEscape(LHS->getType(), RHS);
}

void SemaProfiles::checkInitProfileAssignmentOperands(BinaryOperatorKind Opc,
                                                      Expr *LHSExpr,
                                                      bool IsCompound,
                                                      SourceLocation OpLoc) {
  // A compound assignment reads the old value but builds no lvalue-to-rvalue
  // node for it, so the DefaultLvalueConversion read-through chokepoint never
  // sees the load; check it here. The shift forms are the exception:
  // CheckShiftOperands promotes their LHS through DefaultLvalueConversion,
  // which has already fired for them.
  if (IsCompound && Opc != BO_ShlAssign && Opc != BO_ShrAssign)
    checkInitProfileReadThrough(LHSExpr->getExprLoc(), LHSExpr,
                                LHSExpr->getType());
  checkInitProfileSubobjectWrite(OpLoc, LHSExpr);
}

void SemaProfiles::checkInitProfileIncDec(Expr *Operand, SourceLocation OpLoc) {
  // ++/-- reads the old value with no lvalue-to-rvalue node (unlike -x or
  // !x), then stores to its operand like an assignment does to its LHS.
  checkInitProfileReadThrough(Operand->getExprLoc(), Operand,
                              Operand->getType());
  checkInitProfileSubobjectWrite(OpLoc, Operand);
  // Record last: the pre-store checks above must see pre-store state (there
  // is no RHS). ++u credits the whole entity; ++p reseats a marked pointer.
  recordInitProfileStore(Operand);
}

bool SemaProfiles::inNeverExecutedContext() const {
  return SemaRef.isUnevaluatedContext() ||
         SemaRef.currentEvaluationContext().isDiscardedStatementContext();
}

SemaProfiles::InitCreditStrength
SemaProfiles::currentStoreStrength(const Decl *CreditKey) const {
  // The parser scope chain is parser-only state (see Sema::getCurScope):
  // during template instantiation it describes whatever the parser happens
  // to be doing, not the instantiated function -- and the requires-uninit
  // direction ignores credit while instantiating anyway.
  if (SemaRef.inTemplateInstantiation())
    return InitCreditStrength::Maybe;
  // Synthesized special members build member-wise assignments through
  // CheckAssignmentOperands *outside* template instantiation with a stale
  // getCurScope() (Sema::DefineImplicitCopyAssignment). Harmless without
  // further machinery: those stores target this->m of the synthesized
  // operator=, so their member credit keys on that operator's parse-time
  // pattern, which no user-code consult ever matches.
  if (!SemaRef.getCurScope() || currentConditionalDepth() != 0)
    return InitCreditStrength::Maybe;
  // A goto seen *earlier* in this function body can jump over any later
  // store without introducing a scope (if (c) goto skip; u = 5; skip:),
  // which neither the scope walk nor the expression-depth counter can see
  // -- so once the current function has branched (goto, indirect goto, asm
  // goto; a switch shares the flag, an over-inclusion in the safe
  // direction), every later store records Maybe only: lost requires-uninit
  // diagnostics, never a false positive. A goto *after* a store cannot
  // skip it -- every path from the goto's target to a later consult
  // re-passes the store or never reaches the consult -- so stores before
  // the first branch keep their strength.
  const sema::FunctionScopeInfo *FSI = SemaRef.getCurFunction();
  if (!FSI || FSI->HasBranchIntoScope || FSI->HasIndirectGoto)
    return InitCreditStrength::Maybe;
  // The innermost function-like context, walked from CurContext directly:
  // getCurFunctionDecl skips blocks and captured regions, but a store
  // inside a block body must not definitely credit the enclosing
  // function's entity -- the block may never run. (The depth walk cannot
  // see this either: a block body's scope carries FnScope, so a store at
  // its top level is depth 0 *within the block*.) Only a FunctionDecl --
  // a plain function, a method, a lambda call operator -- earns Definite;
  // block-, captured-, and ObjC-method-owned stores stay Maybe, as they
  // were when this predicate keyed on getCurFunctionDecl.
  const DeclContext *Innermost = SemaRef.CurContext;
  while (Innermost && !Innermost->isFunctionOrMethod())
    Innermost = Innermost->getParent();
  const auto *InnermostFn = dyn_cast_or_null<FunctionDecl>(Innermost);
  if (!InnermostFn)
    return InitCreditStrength::Maybe;
  // Normalized to its parse-time pattern to match the member-credit key
  // (an identity while parsing -- inTemplateInstantiation was excluded
  // above -- kept for symmetry with resolveMemberStoreBase).
  const DeclContext *Enclosing = getParseTimePattern(InnermostFn);
  // The credited entity's owning function: the DeclContext of a credited
  // local/parameter (or of the directly named local base object of member
  // credit); for current-object member credit the key *is* the owning
  // function's parse-time pattern (see resolveMemberStoreBase). The
  // same-function requirement stops a store inside a lambda body from
  // definitely crediting an enclosing function's local -- depth alone
  // cannot, since the walk stops at the lambda's own function scope.
  const DeclContext *Owner = nullptr;
  if (const auto *VD = dyn_cast<VarDecl>(CreditKey))
    Owner = VD->getDeclContext();
  else if (const auto *FD = dyn_cast<FunctionDecl>(CreditKey))
    Owner = FD;
  return Owner == Enclosing ? InitCreditStrength::Definite
                            : InitCreditStrength::Maybe;
}

unsigned SemaProfiles::currentConditionalDepth() const {
  unsigned Depth = ConditionalExprDepth;
  // Count the conditional scopes from the current parse position up to --
  // and excluding -- the nearest function scope. Any flag beyond a plain
  // declaration/compound-statement block marks a scope conditional (see the
  // header comment for the rationale and the known conservatisms).
  for (const Scope *S = SemaRef.getCurScope();
       S && !(S->getFlags() & Scope::FnScope); S = S->getParent())
    if (S->getFlags() & ~unsigned(Scope::DeclScope | Scope::CompoundStmtScope))
      ++Depth;
  return Depth;
}

void SemaProfiles::recordInitProfileStore(const Expr *LHS) {
  if (!getLangOpts().Profiles || !LHS)
    return;
  // Only the never-executed-context gate applies here -- deliberately no
  // enforcement, suppression, or in-template gate; see "Parse-Order Store
  // Credit" in ProfilesFrameworkInternals.rst.
  if (inNeverExecutedContext())
    return;
  // Peel transparent casts so a cast-form store credits like its uncast
  // form ((int &)u = 5 credits u whole; *(int *)p = 5 credits p's pointee;
  // (int *&)p = q reseats p), symmetric with the recognizers' cast
  // pass-through.
  const Expr *E = ignoreTransparentCasts(LHS);
  // *p = e: a store through the exact whole-`*p` lvalue of a marked
  // local/parameter pointer is the pointee's initialization (paper
  // §4.3/§4.5: for a built-in type, a write is its initialization).
  // Class-typed pointees never get here: `*sp = S{...}` resolves to a member
  // operator= (already rejected as a call on uninitialized storage), so
  // PointeeStored is only ever set for built-in-typed pointee stores.
  // Subscript stores (p[i] = e) are deliberately neither credited nor
  // invalidating: the paper bans element-wise tracking (§5.4/§5.5).
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_Deref) {
    if (const VarDecl *VD = getCreditableMarkedPointer(UO->getSubExpr()))
      StoreCredit.markPointeeStored(VD, currentStoreStrength(VD));
    return;
  }
  // a.m = e / this->m = e / m = e (also `@=` and `++`, via the shared
  // hosts): a whole-member store to an [[uninit]] field of a trackable base
  // object is that member's initialization (paper §4.2: "After
  // initialization, the object is no longer [[uninit]]"; §6: ordinary
  // assignment initializes a built-in), keyed per (base, field) so unrelated
  // objects and other function bodies never share credit. Only single-level
  // bases earn credit (x.agg.m = e resolves no base -- and is itself an
  // uninit_write violation; §5.4 rejects deep delayed-initialization
  // tracking), element stores (a.m[i] = e) present a subscript, not a
  // MemberExpr, and stay uncredited, and a class-typed x.agg = e is a member
  // operator= call (rejected as a call on uninitialized storage) that never
  // reaches this built-in-assignment funnel -- so only scalar members are
  // ever credited. Member *pointee* stores (*a.p = e) took the deref arm
  // above, which keys on local pointers only: the pinned per-object aliasing
  // boundary (copies share pointees).
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (const auto *F = dyn_cast<FieldDecl>(ME->getMemberDecl());
        F && F->hasAttr<UninitAttr>())
      if (const Decl *Base = resolveMemberStoreBase(ME))
        StoreCredit.markMemberStored(Base, F, currentStoreStrength(Base));
    return;
  }
  // Only a directly named local-storage variable can be credited beyond
  // this point: statics fail hasLocalStorage.
  const auto *VD = dyn_cast_or_null<VarDecl>(getDirectlyNamedDecl(E));
  if (!VD || !VD->hasLocalStorage())
    return;
  // u = e (also u @= e and ++u, via the inc-dec host): assigning the whole
  // [[uninit]] entity is its initialization (paper §4.2/§4.5).
  if (VD->hasAttr<UninitAttr>()) {
    StoreCredit.markWholeStored(VD, currentStoreStrength(VD));
    return;
  }
  if (!VD->hasAttr<RefToUninitAttr>())
    return;
  if (VD->getType()->isReferenceType()) {
    // r = e stores through the marked reference to its referent; a reference
    // cannot be reseated, so the credit is never cleared.
    StoreCredit.markPointeeStored(VD, currentStoreStrength(VD));
  } else if (VD->getType()->isPointerType()) {
    // p = q / p += n / ++p reseats the marked pointer: every pointee fact
    // -- credit and the destroyed state -- described the old pointee, so
    // all of it is retired wholesale, whatever the reseat's own
    // conditionality (the parse-order status quo). The clear lives here in
    // the tail funnel -- not in checkInitProfilePointerAssignment, which
    // runs only for plain assignment and would miss compound reseats.
    StoreCredit.clearPointee(VD);
  }
}

bool SemaProfiles::hasWholeObjectStoreCredit(
    const ValueDecl *VD, InitCreditStrength Strength) const {
  const auto *Var = dyn_cast<VarDecl>(VD);
  return Var && StoreCredit.hasWholeStored(Var, Strength);
}

bool SemaProfiles::hasPointeeStoreCredit(const ValueDecl *VD,
                                         InitCreditStrength Strength) const {
  const auto *Var = dyn_cast<VarDecl>(VD);
  return Var && StoreCredit.hasPointeeStored(Var, Strength);
}

bool SemaProfiles::hasMemberStoreCredit(const Decl *Base, const FieldDecl *F,
                                        InitCreditStrength Strength) const {
  return Base && F && StoreCredit.hasMemberStored(Base, F, Strength);
}

void SemaProfiles::checkInitProfileThrowOperand(const Expr *Operand) {
  // A thrown pointer copy-initializes the exception object, which cannot
  // carry [[ref_to_uninit]], so throwing a pointer to uninitialized memory is
  // always the unmarked-direction violation. (Reads like `throw *p` funnel
  // through the read-through check instead.)
  QualType ExceptionObjectTy =
      getASTContext().getExceptionObjectType(Operand->getType());
  if (!ExceptionObjectTy->isPointerType())
    return;
  checkInitProfileRefToUninit(Operand->getExprLoc(),
                              /*TargetIsRefToUninit=*/false,
                              /*IsReference=*/false, Operand);
}

void SemaProfiles::checkInitProfileNewInitializer(QualType AllocType,
                                                  Expr *Init) {
  // A written initializer for an allocated pointer binds it like a variable
  // initialization -- but a heap pointer object cannot carry
  // [[ref_to_uninit]], so binding it to uninitialized memory is always the
  // unmarked-direction violation. A braced `new T*{&x}` presents the
  // InitListExpr, which the recognizer's single-element pass-through looks
  // through -- which is why the caller invokes this for scalar allocations
  // only: for an array new, AllocType is the element type and the lone
  // initializer of `new T*[k]{&x}` would be peeled to the same binding the
  // aggregate element hooks already diagnose. An instantiation-dependent
  // allocated type (note that a dependent-pointee `T*` still passes
  // isPointerType) defers to the instantiation rebuild, which re-runs this
  // check with the concrete type.
  if (!AllocType->isPointerType() ||
      AllocType->isInstantiationDependentType() || !Init)
    return;
  checkInitProfileRefToUninit(Init->getExprLoc(),
                              /*TargetIsRefToUninit=*/false,
                              /*IsReference=*/false, Init);
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
          diag::err_profile_class_final_test, RD->getLocation(), RD))
    return;
  S.Diag(RD->getLocation(), diag::err_profile_class_final_test)
      << "test::class_final" << RD;
}

void runTestCtorFinalCallback(Sema &S, CXXConstructorDecl *Ctor) {
  if (!S.Profiles().shouldEmitProfileViolation(
          diag::err_profile_ctor_final_test, Ctor->getLocation(), Ctor))
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
// user-provided-constructor callback and the inherited-constructor class
// callback): visit every checkable field of \p RD left without an
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
      // leaf gives it its active member -- deliberately lenient for a
      // struct variant only partially covered by its written leaves (a
      // missed diagnostic, never a false positive) -- and a vacuous
      // default-initialization (an empty union, or a leaf NSDMI, which is
      // the active member) needs nothing. A leaf [[uninit]] marker is not
      // consulted: union_marker already rejects markers on union members.
      if (AnonRD->isUnion()) {
        if (!anyLeafFieldWritten(AnonRD->getDefinition(), Written) &&
            S.Profiles().defaultInitLeavesScalarIndeterminate(
                F->getType(), /*HonorUninitMarkers=*/true))
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
    if (!S.Profiles().defaultInitLeavesScalarIndeterminate(
            F->getType(), /*HonorUninitMarkers=*/true))
      continue;
    DiagnoseField(F);
  }
}

// The per-field walk of the ctor_uninit_member constructor callback:
// diagnoses at the constructor, gated per field on the Decl-aware violation
// gate (suppression on the constructor or a lexical parent; deferral on
// templated patterns).
static void diagnoseCtorUninitFields(
    Sema &S, const CXXConstructorDecl *Ctor, const CXXRecordDecl *RD,
    const llvm::SmallPtrSetImpl<const FieldDecl *> &Written) {
  forEachCtorUninitField(
      S, RD, Written,
      [&](const FieldDecl *F) {
        if (!S.Profiles().shouldEmitProfileViolation(
                diag::err_init_ctor_uninit_member, Ctor->getLocation(), Ctor))
          return;
        S.Diag(Ctor->getLocation(), diag::err_init_ctor_uninit_member)
            << "std::init" << F->getDeclName();
        S.Diag(F->getLocation(), diag::note_init_uninit_member_here)
            << F->getDeclName();
      },
      [&](const FieldDecl *F) {
        if (!S.Profiles().shouldEmitProfileViolation(
                diag::err_init_ctor_uninit_member, Ctor->getLocation(), Ctor))
          return;
        S.Diag(Ctor->getLocation(), diag::err_init_ctor_uninit_anon_union)
            << "std::init";
        S.Diag(F->getLocation(), diag::note_init_uninit_anon_union_here);
      });
}

void runStdInitCtorUninitMemberCallback(Sema &S, CXXConstructorDecl *Ctor) {
  // Paper §6.1: a user-provided constructor must initialize every member via
  // its member-initializer list or an NSDMI, unless the member is marked
  // [[uninit]] (whose body initialization is the deferred R7 check).
  // A plain assignment in the constructor body does not count.
  if (!Ctor->isUserProvided())
    return;

  // An explicitly defaulted copy or move constructor (out-of-line
  // '= default'; the uniform pattern-4 dispatch sends every defaulted
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
    if (!S.Profiles().defaultInitLeavesScalarIndeterminate(
            Base.getType(),
            /*HonorUninitMarkers=*/true))
      continue;
    if (!S.Profiles().shouldEmitProfileViolation(
            diag::err_init_ctor_uninit_member, Ctor->getLocation(), Ctor))
      continue;
    S.Diag(Ctor->getLocation(), diag::err_init_ctor_uninit_base)
        << "std::init" << Base.getType();
    S.Diag(Base.getBeginLoc(), diag::note_init_uninit_base_here)
        << Base.getType();
  }
}

void runStdInitInheritedCtorUninitMemberCallback(Sema &S, CXXRecordDecl *RD) {
  // Paper §6.1's obligation applied to inheriting constructors
  // ([class.inhctor.init]): an inherited constructor initializes only the
  // nominated base; the inheriting class's own members and its other bases
  // get NSDMI-or-default-initialization -- invariantly, for every inherited
  // signature. uninit_decl cannot catch a defaulted-argument use (`D d(1)`
  // has an initializer) and the constructor-finalization callback skips the
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
  // writes no member-initializers. The Decl-aware gate on the introducer
  // honors suppression on it or the enclosing class and defers on templated
  // patterns (this class callback re-fires on instantiation).
  {
    const UsingDecl *First = Groups.front().Introducer;
    const CXXRecordDecl *FirstBase = Groups.front().NominatedBase;
    SourceLocation FirstLoc = First->getLocation();
    llvm::SmallPtrSet<const FieldDecl *, 1> NoneWritten;
    forEachCtorUninitField(
        S, RD, NoneWritten,
        [&](const FieldDecl *F) {
          if (!S.Profiles().shouldEmitProfileViolation(
                  diag::err_init_ctor_uninit_member, FirstLoc, First))
            return;
          S.Diag(FirstLoc, diag::err_init_inherited_ctor_uninit_member)
              << "std::init" << FirstBase << F->getDeclName();
          S.Diag(F->getLocation(), diag::note_init_uninit_member_here)
              << F->getDeclName();
        },
        [&](const FieldDecl *F) {
          if (!S.Profiles().shouldEmitProfileViolation(
                  diag::err_init_ctor_uninit_member, FirstLoc, First))
            return;
          S.Diag(FirstLoc, diag::err_init_inherited_ctor_uninit_anon_union)
              << "std::init" << FirstBase;
          S.Diag(F->getLocation(), diag::note_init_uninit_anon_union_here);
        });
  }

  // BASES per introducer: the inherited constructor initializes exactly its
  // nominated base, so every *other* direct non-virtual base whose
  // default-initialization is indeterminate is left that way (mirror of the
  // constructor callback's base loop; virtual bases stay deferred as the
  // most-derived constructor's responsibility).
  for (const InheritedCtorGroup &G : Groups) {
    for (const CXXBaseSpecifier &Base : RD->bases()) {
      if (Base.isVirtual())
        continue;
      const CXXRecordDecl *BaseRD = Base.getType()->getAsCXXRecordDecl();
      if (BaseRD && G.NominatedBase &&
          BaseRD->getCanonicalDecl() == G.NominatedBase->getCanonicalDecl())
        continue;
      if (!S.Profiles().defaultInitLeavesScalarIndeterminate(
              Base.getType(), /*HonorUninitMarkers=*/true))
        continue;
      if (!S.Profiles().shouldEmitProfileViolation(
              diag::err_init_ctor_uninit_member, G.Introducer->getLocation(),
              G.Introducer))
        continue;
      S.Diag(G.Introducer->getLocation(),
             diag::err_init_inherited_ctor_uninit_base)
          << "std::init" << G.NominatedBase << Base.getType();
      S.Diag(Base.getBeginLoc(), diag::note_init_uninit_base_here)
          << Base.getType();
    }
  }
}

void runStdInitUninitFieldMarkerCallback(Sema &S, CXXRecordDecl *RD) {
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
    QualType BaseTy = S.Context.getBaseElementType(F->getType());
    // A union- or pointer-typed member (keyed on the same base element type)
    // already draws union_marker / pointer_marker and keeps the marker; do
    // not pile a second diagnostic on top. Load-bearing for union members: a
    // union with a non-trivial member has a deleted -- hence non-trivial --
    // default constructor and would otherwise draw both.
    if (BaseTy->isUnionType() || isPointerMarkerBannedType(BaseTy))
      continue;
    // std::byte may be left uninitialized (paper §4), mirroring
    // checkInitProfileUninitDecl.
    if (BaseTy->isStdByteType())
      continue;
    if (S.Profiles().defaultInitIsVacuous(F->getType()))
      continue;
    // Decl-aware gate: defers on templated patterns (instantiations re-fire
    // through CheckCompletedCXXClass) and honors [[profiles::suppress]] on
    // the field or the enclosing class. Diagnose at the attribute -- the
    // marker is the thing to delete -- like union_marker / pointer_marker.
    if (!S.Profiles().shouldEmitProfileViolation(
            diag::err_init_uninit_not_left_uninitialized, UA->getLocation(), F))
      continue;
    S.Diag(UA->getLocation(), diag::err_init_uninit_not_left_uninitialized)
        << "std::init" << F->getDeclName() << F->getType() << /*IsMember=*/1;
    S.Diag(F->getTypeSpecStartLoc(), diag::note_init_uninit_marker_type)
        << BaseTy << uninitMarkerNonVacuityReason(BaseTy);
  }
}

// Class-finalization opt-in table (pattern 3). The dispatcher runs every
// matching row, so a profile may have several.
constexpr FinalizationProfile<CXXRecordDecl> ClassFinalizationProfiles[] = {
    {"test::class_final", &runTestClassFinalCallback},
    {"std::init", &runStdInitUninitFieldMarkerCallback},
    {"std::init", &runStdInitInheritedCtorUninitMemberCallback},
};

constexpr FinalizationProfile<CXXConstructorDecl>
    ConstructorFinalizationProfiles[] = {
        {"test::ctor_final", &runTestCtorFinalCallback},
        {"std::init", &runStdInitCtorUninitMemberCallback},
};

/// Run the enforced finalization-profile callbacks in \p Table for \p D; the
/// per-node filter (dependent, lambda, delegating, ...) stays at each call
/// site. Each callback consults shouldEmitProfileViolation with \p D and its
/// location.
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
