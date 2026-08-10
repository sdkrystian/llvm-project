//===----- SemaProfiles.cpp --- C++ profiles framework --------*- C++ -*-===//
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

bool SemaProfiles::isProfileEnforcedAt(StringRef ProfileName,
                                       SourceLocation Loc) const {
  if (!isProfileEnforced(ProfileName))
    return false;
  // P3589R2 [decl.attr.enforce]p4: the enforcement's dominion starts after
  // the attribute, so a violation located before it -- global-module-fragment
  // tokens ahead of the module declaration -- is outside it. Fail open on an
  // invalid location on either side (isBeforeInTranslationUnit rejects
  // invalid locations): a PCH restore records no location and must mean
  // "whole TU" (a PCH is this TU's textual prefix), and synthesized code
  // keeps plain-enforcement behavior. Locations are compared by *expansion*
  // location, deliberately unlike the suppression comparator's raw token
  // order (see isProfileSuppressed): enforcement dominion is TU-scale, so a
  // macro defined in the GMF but *invoked* in the purview stays enforced --
  // its invocation tokens are purview tokens -- while GMF pattern tokens are
  // skipped.
  const profiles::ProfileEnforcement *E = getProfileEnforcement(ProfileName);
  const SourceManager &SM = getASTContext().getSourceManager();
  if (Loc.isValid() && E->EnforceLoc.isValid() &&
      SM.isBeforeInTranslationUnit(SM.getExpansionLoc(Loc),
                                   SM.getExpansionLoc(E->EnforceLoc)))
    return false;
  return true;
}

const profiles::ProfileEnforcement *
SemaProfiles::getProfileEnforcement(StringRef ProfileName) const {
  return getASTContext().getProfileEnforcement(ProfileName);
}

bool SemaProfiles::addProfileEnforcement(StringRef Name, StringRef Designator,
                                 SourceLocation Loc) {
  if (const auto *Existing = getProfileEnforcement(Name)) {
    if (Existing->Designator != Designator) {
      Diag(Loc, diag::err_profiles_enforce_mismatch) << Name;
      Diag(Existing->EnforceLoc, diag::note_previous_attribute);
      return false;
    }
    return true;
  }
  getASTContext().EnforcedProfiles.push_back(
      {{Name.str(), Designator.str()}, Loc});
  return true;
}

// Unzip profile arguments into the parallel key/value/kind arrays that
// ProfilesSuppressAttr stores (Attr.td cannot hold structured arguments).
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
    bool IsNew = !getProfileEnforcement(Name);
    if (!addProfileEnforcement(Name, Spelling, AL.getLoc()))
      continue;

    if (Mod && !llvm::any_of(Mod->EnforcedProfileDesignators,
                             [&](const Module::EnforcedProfile &EP) {
                               return EP.ProfileName == Name;
                             }))
      Mod->EnforcedProfileDesignators.push_back({Name.str(), Spelling.str()});

    if (IsNew) {
      if (NewNames)
        NewNames->push_back(Name);
      if (NewDesignators)
        NewDesignators->push_back(Spelling);
    }
  }
  return true;
}

// P3589R2 [decl.attr.enforce]p5: profiles are compatible if they are the same
// -- by name; arguments configure a profile without changing its identity --
// or proclaimed compatible by the implementation. "All standard profiles are
// compatible with each other" is the one proclamation modeled here.
static bool areProfilesCompatible(StringRef A, StringRef B) {
  return A == B || (A.starts_with("std::") && B.starts_with("std::"));
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
  if (isProfileExemptSystemHeaderLoc(Old->getLocation()))
    return;
  // An explicit global-module-fragment declaration's dominion is unknown;
  // skip rather than guess.
  if (M->isExplicitGlobalModule())
    return;
  Module *Top = M->getTopLevelModule();
  // Same module family: the exported set under-approximates the interface
  // TU's dominion, which this unit inherits anyway.
  if (Module *Current = SemaRef.getCurrentModule())
    if (Current->getTopLevelModule()->getPrimaryModuleInterfaceName() ==
        Top->getPrimaryModuleInterfaceName())
      return;

  // A gated-off test:: profile is inert in this compilation, on either side.
  auto IsActive = [&](StringRef Name) {
    return getLangOpts().ProfilesTestProfiles || !Name.starts_with("test::");
  };
  // First active profile in Enforced with no compatible counterpart in
  // Covering; empty if fully covered.
  auto FindUncovered = [&](const auto &Enforced,
                           const auto &Covering) -> StringRef {
    for (const auto &EP : Enforced) {
      StringRef Name = EP.ProfileName;
      if (!IsActive(Name))
        continue;
      if (llvm::none_of(Covering, [&](const auto &Other) {
            return areProfilesCompatible(Name, Other.ProfileName);
          }))
        return Name;
    }
    return {};
  };

  // The rule is symmetric: every profile whose dominion covers one
  // declaration must have a compatible counterpart covering the other.
  // Report the first violation in each direction.
  StringRef MissingHere = FindUncovered(Top->EnforcedProfileDesignators,
                                        getASTContext().EnforcedProfiles);
  StringRef MissingThere = FindUncovered(getASTContext().EnforcedProfiles,
                                         Top->EnforcedProfileDesignators);
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

  SmallVector<StringRef, 4> RawArgs;
  for (const auto &Arg : Args.RawArguments)
    RawArgs.push_back(Arg);
  SmallVector<StringRef, 4> RawArgumentKeys;
  SmallVector<StringRef, 4> RawArgumentValues;
  SmallVector<unsigned, 4> RawArgumentKinds;
  unzipProfileArguments(Args.Arguments, RawArgumentKeys, RawArgumentValues,
                        RawArgumentKinds);

  return ::new (getASTContext()) ProfilesSuppressAttr(
      getASTContext(), AL, Args.Name, Args.Justification, Args.Rule,
      RawArgs.data(), RawArgs.size(), RawArgumentKeys.data(),
      RawArgumentKeys.size(), RawArgumentValues.data(),
      RawArgumentValues.size(), RawArgumentKinds.data(),
      RawArgumentKinds.size());
}

ProfilesSuppressAttr *
SemaProfiles::makeImplicitProfilesSuppressAttr(StringRef ProfileName,
                                       StringRef RuleName) {
  return ProfilesSuppressAttr::CreateImplicit(
      getASTContext(), ProfileName, /*Justification=*/"", RuleName,
      /*RawArguments=*/nullptr, /*RawArgumentsSize=*/0,
      /*RawArgumentKeys=*/nullptr, /*RawArgumentKeysSize=*/0,
      /*RawArgumentValues=*/nullptr, /*RawArgumentValuesSize=*/0,
      /*RawArgumentKinds=*/nullptr, /*RawArgumentKindsSize=*/0);
}

bool SemaProfiles::isProfileSuppressed(StringRef ProfileName,
                                       StringRef RuleName,
                                       SourceLocation Loc) const {
  const SourceManager &SM = getASTContext().getSourceManager();
  for (const auto &E : ProfileSuppressStack) {
    if (!profiles::suppressionMatches(E.ProfileName, E.RuleName, ProfileName,
                                      RuleName))
      continue;
    // Dominion check; see ProfilesFrameworkInternals.rst, "Suppression
    // Dominion Mechanics". Fail open on an invalid location on either side
    // (isBeforeInTranslationUnit rejects invalid locations), preserving
    // plain-liveness behavior for synthesized code. Compare raw TU token
    // order: expansion-loc normalization would collapse a macro expansion's
    // tokens onto the invocation and over-suppress.
    if (Loc.isInvalid() || E.Begin.isInvalid() ||
        (!SM.isBeforeInTranslationUnit(Loc, E.Begin) &&
         (E.End.isInvalid() || !SM.isBeforeInTranslationUnit(E.End, Loc))))
      return true;
  }
  return false;
}

bool SemaProfiles::isProfileSuppressed(StringRef ProfileName,
                                       StringRef RuleName, const Stmt *S,
                                       AnalysisDeclContext &AC) const {
  ParentMap &PM = AC.getParentMap();
  for (const Stmt *Cur = S; Cur; Cur = PM.getParent(Cur))
    if (profiles::isSuppressedFor(Cur, ProfileName, RuleName))
      return true;
  return profiles::isSuppressedFor(AC.getDecl(), ProfileName, RuleName);
}

bool SemaProfiles::isProfileExemptSystemHeaderLoc(SourceLocation Loc) const {
  return getASTContext().isProfileExemptSystemHeaderLoc(Loc);
}


bool SemaProfiles::shouldEmitProfileViolation(StringRef ProfileName,
                                              StringRef RuleName,
                                              SourceLocation Loc) {
  return shouldEmitProfileViolation(ProfileName, RuleName, Loc, /*D=*/nullptr);
}

bool SemaProfiles::shouldEmitProfileViolation(StringRef ProfileName,
                                              StringRef RuleName,
                                              SourceLocation Loc,
                                              const Decl *D) {
  if (!isProfileEnforcedAt(ProfileName, Loc))
    return false;
  if (getASTContext().isProfileExemptSystemHeaderLoc(Loc))
    return false;
  // Honor [[profiles::suppress]] from the parse-time stack (dominion-checked;
  // see ProfilesFrameworkInternals.rst) and, when a Decl is available, from
  // the declaration and its lexical parents -- the latter survives the parse
  // scope's teardown, so finalization checks still respect suppression.
  if (isProfileSuppressed(ProfileName, RuleName, Loc) ||
      profiles::isSuppressedFor(D, ProfileName, RuleName))
    return false;
  // P3589R2 Section 1.1: "its static semantic effects are as-if applied only
  // after translation phase 7. It is not possible for a profile to change the
  // outcome of overload resolution or template instantiation, nor is it
  // possible to 'SFINAE out' failure of a program to satisfy a profile
  // requirement."
  //
  // A templated entity is not yet a phase-7 entity, so a profile rule must fire
  // only on its instantiation -- where D is the instantiated, non-templated
  // declaration -- not on the template pattern. Checking the pattern too would
  // diagnose never-instantiated templates and double-fire (once when the
  // pattern is parsed and again at each instantiation).
  //
  // A Decl-less expression check site whose Build* routine is re-run at
  // instantiation must instead defer in a dependent context from its own
  // wrapper, since no Decl is available here. The reinterpret_cast check
  // passes D == nullptr and is not re-checked at instantiation, so it keeps
  // running once at parse time (a separate gap).
  if (D && D->isTemplated())
    return false;
  if (SemaRef.isUnevaluatedContext())
    return false;
  if (SemaRef.currentEvaluationContext().isDiscardedStatementContext())
    return false;
  return true;
}

bool SemaProfiles::shouldEmitProfileViolation(StringRef ProfileName,
                                              StringRef RuleName,
                                              const Stmt *UseStmt,
                                              AnalysisDeclContext &AC) const {
  SourceLocation Loc =
      UseStmt ? UseStmt->getBeginLoc() : AC.getDecl()->getLocation();
  if (!isProfileEnforcedAt(ProfileName, Loc))
    return false;
  if (getASTContext().isProfileExemptSystemHeaderLoc(Loc))
    return false;
  if (isProfileSuppressed(ProfileName, RuleName, UseStmt, AC))
    return false;
  // A function can be analyzed while an enclosing construct is still
  // mid-parse (e.g. a local class's method body), so consult the live
  // parse-time stack too; the consult is dominion-checked, so an unrelated
  // live scope never matches (see ProfilesFrameworkInternals.rst).
  if (isProfileSuppressed(ProfileName, RuleName, Loc))
    return false;
  return true;
}

bool SemaProfiles::checkProfileViolation(StringRef ProfileName,
                                         StringRef RuleName, SourceLocation Loc,
                                         unsigned DiagID) {
  if (!shouldEmitProfileViolation(ProfileName, RuleName, Loc))
    return false;
  Diag(Loc, DiagID) << ProfileName;
  return true;
}

void SemaProfiles::ProfileSuppressScope::push(StringRef ProfileName,
                                      StringRef RuleName,
                                      SourceLocation Begin,
                                      SourceLocation End) {
  S.Profiles().ProfileSuppressStack.push_back(
      {ProfileName, RuleName, Begin, End});
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
/// but early* -- a mid-parse class collapses to its name token (the brace
/// range is set only by ActOnTagFinishDefinition, after even the late-parsed
/// members), a body-pending function ends at its declarator, an
/// uninitialized variable at its declarator -- so each arm gates on the
/// marker that the construct's real end has been seen. Returning invalid
/// falls back to scope-lifetime bounding, which is exact mid-parse.
///
/// Deliberately dyn_cast dispatch with a fail-open tail, not a Decl::Kind
/// switch: a switch would fail open for a newly added *derived* kind anyway
/// (recreating the same defect one level down), while here every unhandled
/// declaration kind reaches the tail explicitly. The tail's invalid return
/// is safe by construction: with no recorded end, the entry's scope
/// lifetime bounds the dominion, which can only make the dominion *longer*
/// -- more suppression, never a false positive.
static SourceLocation getCompletedConstructEnd(const Decl *D) {
  // Self-gating: the brace range is set only by ActOnTagFinishDefinition,
  // so a mid-parse tag yields an invalid end with no explicit gate.
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
  // Self-gating: the r-brace is recorded only when the namespace body
  // finishes.
  if (const auto *ND = dyn_cast<NamespaceDecl>(D))
    return ND->getRBraceLoc();
  // Fail-open tail (see above): an unhandled declaration kind gets
  // scope-lifetime bounding, the longer -- suppression-favoring -- dominion.
  return SourceLocation();
}

SemaProfiles::ProfileSuppressScope::ProfileSuppressScope(Sema &S, const Decl *D,
                                                  bool WalkLexicalParents)
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

SemaProfiles::ProfileSuppressScope::ProfileSuppressScope(Sema &S,
                                                  ArrayRef<const Attr *> Attrs,
                                                  SourceLocation Begin,
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
// Row for the unified finalization dispatch shared by class-finalization
// (pattern 3) and constructor-finalization (pattern 4): a profile name plus a
// callback invoked once per finalized, non-dependent, non-invalid Node (a
// CXXRecordDecl or a CXXConstructorDecl). Adding a new profile is a single row
// in the matching table below plus a ProfileRuleError diagnostic in
// DiagnosticSemaKinds.td and a callback that consults
// SemaProfiles::shouldEmitProfileViolation before emitting.
template <class Node> struct FinalizationProfile {
  StringRef Name;
  void (*Callback)(Sema &, Node *);
};

void runTestClassFinalCallback(Sema &S, CXXRecordDecl *RD) {
  if (!S.Profiles().shouldEmitProfileViolation("test::class_final", /*Rule=*/"",
                                               RD->getLocation(), RD))
    return;
  S.Diag(RD->getLocation(), diag::err_profile_class_final_test)
      << "test::class_final" << RD;
}

void runTestCtorFinalCallback(Sema &S, CXXConstructorDecl *Ctor) {
  if (!S.Profiles().shouldEmitProfileViolation("test::ctor_final", /*Rule=*/"",
                                               Ctor->getLocation(), Ctor))
    return;
  S.Diag(Ctor->getLocation(), diag::err_profile_ctor_final_test)
      << "test::ctor_final" << Ctor->getParent();
}

// Class-finalization opt-in table (pattern 3).
constexpr FinalizationProfile<CXXRecordDecl> ClassFinalizationProfiles[] = {
    {"test::class_final", &runTestClassFinalCallback},
};

// Constructor-finalization opt-in table (pattern 4).
constexpr FinalizationProfile<CXXConstructorDecl>
    ConstructorFinalizationProfiles[] = {
        {"test::ctor_final", &runTestCtorFinalCallback},
};

// Run the enforced finalization-profile callbacks in Table for D. Merges the
// former per-node dispatchers; the per-node filter (dependent, lambda,
// delegating, ...) stays at each call site. Each callback passes D to the
// Decl-aware SemaProfiles::shouldEmitProfileViolation, which honors
// [[profiles::suppress]]
// on D or a lexical parent, so the dispatcher needs no suppress scope of its
// own. The table is taken by reference-to-array, not ArrayRef: deducing Node
// from a C array against an ArrayRef<FinalizationProfile<Node>> parameter is
// not
// possible (no array-to-ArrayRef conversion happens during template argument
// deduction).
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

