//===----- SemaProfiles.h --- C++ profiles framework ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares semantic analysis for the C++ profiles framework
/// (P3589R2) and the built-in std::init initialization profile (P4222R1.1).
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
#include "llvm/ADT/DenseMap.h"
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

  /// std::init / uninit_decl (R2, paper §4.2): diagnose an automatic variable
  /// definition that leaves the object (or a scalar subobject) indeterminate
  /// without an acknowledging [[uninit]] marker. Called from
  /// \c ActOnUninitializedDecl after default-initialization is attempted.
  void checkInitProfileUninitDecl(const VarDecl *Var);

  /// std::init / static_marker (paper §3, §4.2): diagnose [[uninit]] on a
  /// static or thread-storage variable, which is zero-initialized by language
  /// rule and therefore an initialized object. Called from
  /// \c ActOnUninitializedDecl.
  void checkInitProfileStaticMarker(const VarDecl *Var);

  /// std::init / static_runtime_init (paper §3): diagnose a non-local static
  /// whose initialization needs a runtime constructor. \p CheckConstInit
  /// lazily evaluates whether the initializer is constant (trivial
  /// default-init counts as constant here). Returns true if the diagnostic
  /// was emitted, in which case the caller skips -Wglobal-constructors.
  bool
  checkInitProfileStaticRuntimeInit(const VarDecl *Var,
                                    llvm::function_ref<bool()> CheckConstInit);

  /// std::init / uninit_with_initializer (R4): diagnose \p D if it is both
  /// marked [[uninit]] and has an initializer. Shared by the variable
  /// (\c CheckCompleteVariableDeclaration) and non-static data member
  /// (\c ActOnFinishCXXInClassMemberInitializer) paths. \p Init is the
  /// (possibly null) initializer; a RecoveryExpr placeholder for a failed
  /// initialization does not count as a user-written initializer.
  void checkInitProfileUninitWithInitializer(const ValueDecl *D,
                                             const Expr *Init);

  /// True if default-initialization of \p T would leave at least one scalar
  /// subobject with an indeterminate value. Shared by the std::init rules
  /// uninit_decl (at the variable declaration), ctor_uninit_member (for a
  /// class-typed member), and uninit_with_initializer. A class with a
  /// user-provided default constructor is trusted (that constructor is
  /// checked at its own definition). Dependent and incomplete types are
  /// treated as determinate.
  ///
  /// When \p HonorUninitMarkers is true, a data member marked [[uninit]]
  /// is treated as acknowledged and skipped, so a type whose only
  /// indeterminate scalars are all marked is reported as determinate.
  /// uninit_decl and ctor_uninit_member pass true (the marker excuses the
  /// member, paper §6.2); uninit_with_initializer passes false because it
  /// needs the factual answer (whether the default-initialization is
  /// genuinely a no-op).
  bool defaultInitLeavesScalarIndeterminate(QualType T,
                                            bool HonorUninitMarkers = false);

  /// True if default-initialization of \p T is a genuine no-op that leaves
  /// the object (or every array element) uninitialized -- the only state
  /// consistent with an [[uninit]] marker. A non-trivial default constructor
  /// (user-provided anywhere in the subtree, a default member initializer, a
  /// virtual table pointer) initializes something, contradicting the marker
  /// (paper §4.2 rule 2, §5.3); a deleted or absent default constructor makes
  /// default-initialization ill-formed, not a no-op (the marker is
  /// unsatisfiable); an all-scalars-determinate type (e.g. an
  /// empty struct) has nothing uninitialized. Type-level (not a query on the
  /// synthesized construct-expression) because the field flavor and
  /// static_marker's no-initializer arm have no construct-expression, and
  /// getBaseElementType handles arrays and scalars uniformly. Shared by
  /// uninit_with_initializer and static_marker (through their common
  /// initializer guard) and the field-marker flavor.
  bool defaultInitIsVacuous(QualType T);

  /// If \p E (stripped of parens and implicit casts) directly names a
  /// declaration -- a DeclRefExpr or a MemberExpr -- return that declaration;
  /// otherwise null. The std::init checks read [[ref_to_uninit]] /
  /// [[uninit]] markers only off a directly named entity.
  static const ValueDecl *getDirectlyNamedDecl(const Expr *E);

  /// std::init / ref_to_uninit (paper §5): true only if \p E is affirmatively
  /// recognized as referring to (for a pointer source) or, when
  /// \p IsReference, denoting (for a glvalue source) uninitialized storage.
  /// Recognized purely locally from the expression's syntactic form -- the
  /// address of, or a subobject of, a [[uninit]] entity; a value of a
  /// [[ref_to_uninit]] pointer/reference or array; a dereference of such a
  /// pointer; a cast of such a pointer to another pointer type, or of such a
  /// glvalue to another reference; a call to a [[ref_to_uninit]]-returning
  /// function or to a known uninitialized-returning allocator (the malloc
  /// and alloca builtin families and raw replaceable ::operator new calls;
  /// calloc's result is initialized, realloc's
  /// unknown); or a new-expression whose default-initialization leaves the
  /// allocated object indeterminate (e.g. new int). A trusted-initialized
  /// source and an unrecognized (unknown) source both return false (no flow
  /// analysis).
  bool refersToUninitializedMemory(const Expr *E, bool IsReference) const;

  /// std::init / ref_to_uninit (paper §5): check that the initialization of a
  /// pointer or reference is consistent with its [[ref_to_uninit]] marking --
  /// a marked target must refer to uninitialized memory, and an unmarked
  /// target must not. Shared by the variable, data-member, assignment,
  /// argument, and return check sites; gated by shouldEmitProfileViolation.
  /// A Decl-less call defers only on an instantiation-dependent \p Src --
  /// such a construct is always rebuilt at instantiation, re-running this
  /// funnel with the substituted source -- and otherwise fires at definition
  /// time; if the construct is rebuilt at instantiation anyway (a local
  /// operand, a call argument, a return), the same diagnostic repeats there
  /// (accepted for now). A Decl-carrying call instead defers via the
  /// D->isTemplated() check in shouldEmitProfileViolation and fires on the
  /// instantiated declaration.
  void checkInitProfileRefToUninit(SourceLocation Loc, bool TargetIsRefToUninit,
                            bool IsReference, const Expr *Src,
                            const Decl *D = nullptr);

  /// std::init / ref_to_uninit (paper §5): check that binding \p Src to
  /// \p Target (a variable, data member, parameter, or function) is
  /// consistent with the target's [[ref_to_uninit]] marking. A null \p Target
  /// is a binding with no declaration to carry the marker (a parameter of a
  /// call through a function pointer) and is checked as unmarked. \p T is the
  /// bound type -- the target's type, or the return type when \p Target is a
  /// function. No-op unless \p T is a non-dependent pointer or reference (a
  /// dependent type defers to instantiation, where the check site re-runs
  /// with the concrete type). \p D, when available, is the declaration used
  /// for suppression lookup and template-pattern deferral.
  void checkInitProfileRefToUninitBinding(SourceLocation Loc,
                                          const ValueDecl *Target, QualType T,
                                          const Expr *Src,
                                          const Decl *D = nullptr);

  /// std::init / uninit_read (paper §4.5): diagnose a read *through* a
  /// [[ref_to_uninit]] pointer or reference, whose result is itself
  /// uninitialized. Called from Sema::DefaultLvalueConversion at the single
  /// lvalue-to-rvalue chokepoint, with \p Glvalue the operand being loaded
  /// and \p ValueType its value type; and from the compound-assignment
  /// (Sema::CheckAssignmentOperands) and increment/decrement
  /// (Sema::CreateBuiltinUnaryOp) operator sites, whose reads build no
  /// lvalue-to-rvalue node (the shift-compounds are excluded there because
  /// their LHS promotion already funnels through the chokepoint). Reuses the
  /// ref_to_uninit recognizer with its read access preset, so a direct read
  /// of a named [[uninit]] object is left to the flow-based uninit_read
  /// pass. A std::byte read is exempt (paper §4.5). Defers only on an
  /// instantiation-dependent \p Glvalue (rebuilt at instantiation, where the
  /// check re-runs); a non-dependent read fires at definition time and may
  /// repeat if the read is rebuilt at instantiation anyway (accepted).
  void checkInitProfileReadThrough(SourceLocation Loc, const Expr *Glvalue,
                            QualType ValueType);

  /// std::init / uninit_write (paper §5.4-§5.6): diagnose a scalar store to a
  /// proper subobject of a named [[uninit]] entity -- delayed piecemeal
  /// initialization, which only whole-object construct_at could make good.
  /// Called from Sema::CheckAssignmentOperands (the shared simple/compound
  /// assignment funnel) and from the built-in increment/decrement arm of
  /// Sema::CreateBuiltinUnaryOp, with \p LHS the store target. Reuses the
  /// recognizer with its write access preset: a store to the whole named
  /// entity is its initialization (paper §4.5), and storage reached through
  /// [[ref_to_uninit]] is trusted (the deferred construct_at slice), so only
  /// a below-top-level [[uninit]] marker fires. A std::byte store is exempt
  /// (paper §4.5). Defers only on an instantiation-dependent \p LHS (rebuilt
  /// at instantiation, where the check re-runs); a non-dependent store fires
  /// at definition time and may repeat if the assignment is rebuilt at
  /// instantiation anyway (accepted).
  void checkInitProfileSubobjectWrite(SourceLocation Loc, const Expr *LHS);

  /// std::init / ref_to_uninit (paper §5): a pointer argument passed through
  /// a variadic `...` parameter, which cannot carry [[ref_to_uninit]], is
  /// checked as an unmarked target. Called with the promoted argument from
  /// the C++ variadic promotion loops (Sema::GatherArgumentsForCall and
  /// Sema::BuildCallToObjectOfClassType); a non-pointer argument is a no-op
  /// (its value read is the lvalue-to-rvalue chokepoint's).
  void checkInitProfileVariadicArgument(const Expr *Arg);

  /// std::init / ref_to_uninit (paper §4.3): a by-reference lambda capture of
  /// \p Var binds a reference to its storage, and a capture cannot carry
  /// [[ref_to_uninit]], so capturing an entity that denotes uninitialized
  /// storage -- an [[uninit]] variable, or a [[ref_to_uninit]] reference --
  /// is always the unmarked-direction violation. Called from
  /// \c Sema::BuildLambdaExpr for each by-reference non-init variable capture
  /// (init-captures are checked at \c createLambdaInitCaptureVarDecl); defers
  /// only when the captured variable's type is instantiation-dependent.
  /// TreeTransform always rebuilds a lambda at instantiation, so a deferred
  /// capture re-processes there -- and a definition-time fire repeats there
  /// (accepted).
  void checkInitProfileRefCapture(SourceLocation Loc, const ValueDecl *Var);

  /// std::init / ref_to_uninit (paper §7.2): a member call binds its implicit
  /// object parameter to \p Object, and that parameter can never carry
  /// [[ref_to_uninit]], so a call on an object recognized as uninitialized
  /// storage is always the unmarked-direction violation. Called from
  /// \c Sema::PerformImplicitObjectArgumentInitialization, the funnel every
  /// member-call flavor's object argument converts through -- dot and arrow
  /// calls, member operators, functor operator(), operator->, and conversion
  /// operators. Explicit-object member functions initialize their object as an
  /// ordinary parameter and are already checked there; a destructor call is
  /// skipped (destruction of uninitialized storage is the deferred destroy_at
  /// slice), as is a static call operator (no implicit object parameter, like
  /// a static member call). Defers only on an instantiation-dependent
  /// \p Object -- the call
  /// is rebuilt at instantiation, re-running the funnel -- and otherwise fires
  /// at definition time, repeating if the call is rebuilt anyway (accepted).
  void checkInitProfileObjectArgument(const Expr *Object,
                                      const CXXMethodDecl *Method);

  /// std::init / ref_to_uninit (paper §4.3): assigning to a pointer must
  /// respect the assigned-to pointer's [[ref_to_uninit]] marking; a no-op for
  /// a non-pointer LHS. Hosts the cluster from Sema::CreateBuiltinBinOp's
  /// BO_Assign arm. An instantiation-dependent LHS defers to the
  /// instantiation rebuild (its marker cannot be read yet); the source's
  /// dependence is the shared funnel's to defer on.
  void checkInitProfilePointerAssignment(Expr *LHS, Expr *RHS,
                                         SourceLocation OpLoc);

  /// std::init: the check pair every built-in assignment hosts (paper
  /// §5.4-§5.6): the compound-assignment old-value load (read-through --
  /// excluding the shifts, whose LHS promotion already loads through the
  /// lvalue-to-rvalue chokepoint) and the subobject-write check. Hosts the
  /// cluster from Sema::CheckAssignmentOperands. \p IsCompound distinguishes
  /// `op=` from `=` (!CompoundType.isNull() at the host site).
  void checkInitProfileAssignmentOperands(BinaryOperatorKind Opc,
                                          Expr *LHSExpr, bool IsCompound,
                                          SourceLocation OpLoc);

  /// std::init: the check pair a built-in ++/-- hosts -- the old-value load
  /// (read-through) and the store (subobject-write). Hosts the cluster from
  /// Sema::CreateBuiltinUnaryOp's increment/decrement arm. Records the store
  /// credit last, after both pre-store checks.
  void checkInitProfileIncDec(Expr *Operand, SourceLocation OpLoc);

  /// std::init: record parse-order whole-entity store credit for \p LHS, the
  /// left operand of a completed built-in assignment (called from the tail
  /// of Sema::CheckAssignmentOperands) or the operand of a built-in ++/--.
  /// Assigning a whole [[uninit]] local is its initialization (paper
  /// §4.2/§4.5), and a store through the exact `*p` / `r` lvalue of a
  /// [[ref_to_uninit]] local or parameter initializes the pointee (§4.3); a
  /// store to a marked *pointer* itself reseats it and clears its pointee
  /// credit. Element stores (p[i] = e) neither credit nor invalidate
  /// (§5.4/§5.5 ban element-wise tracking), and escapes never credit (§6.2
  /// reserves callee-initialization for now_init()). Purely parse-order --
  /// no dominance or flow analysis -- so the credit errs only toward missed
  /// diagnostics: every store records Maybe credit (suppression only), and
  /// only an unconditional store in the credited entity's own function
  /// records the Definite credit a diagnostic may fire on (see
  /// currentStoreStrength). Deliberately not gated on enforcement or
  /// [[profiles::suppress]]: a suppressed store still initializes, and
  /// failing to credit it would turn suppression into later false positives.
  void recordInitProfileStore(const Expr *LHS);

  /// std::init / [[now_init]] (P4222R2 §6.2): a [[now_init]] callee
  /// initializes the storage bound to each of its [[ref_to_uninit]]
  /// parameters, so the binding earns the same parse-order credit the
  /// equivalent direct store would. Called from the tail of
  /// checkInitProfileRefToUninitBinding when \p Target is a marked parameter
  /// of a [[now_init]] function; recognizes the affirmatively creditable
  /// source shapes -- &u / u (whole-entity credit on an [[uninit]] local), p
  /// / *p / &*p (pointee credit on a marked local/parameter pointer; §6.2's
  /// initialize2(p) example), r (pointee credit on a marked reference), and
  /// &base.m / base.m (per-object member credit, resolveMemberStoreBase
  /// keys) -- through the recognizers' explicit-cast pass-through. Variadic
  /// arguments, unmarked parameters, and calls through function pointers
  /// never reach here (no marked ParmVarDecl target). Recorded regardless of
  /// enforcement, suppression, or diagnosis of the binding itself (the
  /// callee still initializes; recordInitProfileStore's rationale), but not
  /// in never-executed contexts.
  void recordNowInitArgument(const ValueDecl *Target, QualType T,
                             const Expr *Src);

  /// std::init / [[now_uninit]]: a [[now_uninit]] callee ends the lifetime
  /// of the storage bound to each of its pointer/reference parameters
  /// (P4222R2 §4.4's missing destroy_at recording), so the binding
  /// *withdraws* the parse-order credit the equivalent [[now_init]] call
  /// would have recorded: the storage classifies as uninitialized again,
  /// re-construction becomes legal, an unmarked-target binding of the
  /// storage is the ordinary unmarked-direction violation, and a second
  /// destruction is the dedicated double_destroy violation (the destroyed
  /// state a Definite withdrawal records). Called from the tail of
  /// checkInitProfileRefToUninitBinding when \p Target is a
  /// pointer/reference parameter of a [[now_uninit]] function -- the
  /// parameters are unmarked (they receive initialized memory), so unlike
  /// recordNowInitArgument no parameter marker is required. Recognizes the
  /// same source shapes, which are marker-keyed on the source side, so an
  /// ordinary initialized argument withdraws nothing. Same gates as its
  /// sibling: not enforcement- or suppression-gated (a suppressed destroy
  /// still destroys), but never-executed contexts withdraw nothing. The
  /// withdrawal mirrors the recording's strength rule: an unconditional
  /// same-function destroy withdraws credit of both strengths, while a
  /// merely-possible one (under a condition, or in another function)
  /// withdraws only the Definite claim -- it may have destroyed the
  /// storage, so no credit-fired diagnostic may rely on it, but the Maybe
  /// credit survives and the lenient direction gains no new errors.
  void recordNowUninitArgument(const ValueDecl *Target, QualType T,
                               const Expr *Src);

  /// The tracked storage a lifetime-annotated callee's argument denotes, as
  /// resolved by resolveLifetimeAnnotatedStorage: the whole [[uninit]]
  /// entity (Whole), the storage behind a marked pointer or reference
  /// (Pointee), the [[uninit]] member of a trackable base object (Member),
  /// or nothing trackable (None).
  struct LifetimeAnnotatedStorage {
    enum class Kind { None, Whole, Pointee, Member };
    Kind StorageKind = Kind::None;
    /// The credited local/parameter (Whole and Pointee).
    const VarDecl *Entity = nullptr;
    /// The member store-credit key (Member; resolveMemberStoreBase's base).
    const Decl *Base = nullptr;
    const FieldDecl *Field = nullptr;

    static LifetimeAnnotatedStorage whole(const VarDecl *VD) {
      return {Kind::Whole, VD, nullptr, nullptr};
    }
    static LifetimeAnnotatedStorage pointee(const VarDecl *VD) {
      return {Kind::Pointee, VD, nullptr, nullptr};
    }
    static LifetimeAnnotatedStorage member(const Decl *Base,
                                           const FieldDecl *F) {
      return {Kind::Member, nullptr, Base, F};
    }
  };

  /// The shared shape walk of recordNowInitArgument and
  /// recordNowUninitArgument: resolve \p Src (bound as \p T) to the storage
  /// the annotated callee initializes or destroys -- &u / u (whole-entity),
  /// p / *p / &*p (marked-pointer pointee), r (marked-reference referent),
  /// &base.m / base.m (per-object member) -- through the recognizers'
  /// explicit-cast pass-through. Marker-keyed on the source side: an
  /// ordinary unmarked argument resolves to None.
  LifetimeAnnotatedStorage resolveLifetimeAnnotatedStorage(QualType T,
                                                           const Expr *Src)
      const;

  /// Resolve \p Src (bound as \p T, see resolveLifetimeAnnotatedStorage)
  /// and add (\p Withdraw false) or remove (true) the resolved storage's
  /// credit, at the strength the current parse position earns toward it
  /// (currentStoreStrength; for a withdrawal, the strength rule described
  /// at recordNowUninitArgument). A Definite withdrawal also records the
  /// storage as destroyed; any recorded store retires that state.
  void recordLifetimeAnnotatedArgument(QualType T, const Expr *Src,
                                       bool Withdraw);

  /// True if the storage \p Src (bound as \p T) denotes -- resolved through
  /// the same shapes as recordLifetimeAnnotatedArgument -- is in the
  /// destroyed state: definitely destroyed by a [[now_uninit]] callee and
  /// not stored or reinitialized since. Read-only; untrackable shapes are
  /// never destroyed.
  bool storageIsDestroyed(QualType T, const Expr *Src) const;

  /// std::init: a delete-expression releases its operand's storage like a
  /// storage-release callee, but the operand never passes through the
  /// parameter-binding funnel (Sema::ActOnCXXDelete converts it with no
  /// InitializedEntity, and a usual operator delete's void* parameter skips
  /// the conversion entirely), so this hook -- hosted just before the
  /// CXXDeleteExpr is built -- records the same credit withdrawal, with no
  /// binding diagnostic of its own: `delete q` and `::operator delete(q)`
  /// agree, and a later whole-`*q` read through the marked pointer is
  /// diagnosed. An instantiation-dependent operand defers to the rebuilt
  /// expression (TreeTransform re-invokes ActOnCXXDelete); never-executed
  /// contexts withdraw nothing, as everywhere.
  void checkInitProfileDeleteOperand(const Expr *Operand);

  /// True if the current expression-evaluation context never executes at
  /// runtime (unevaluated or discarded-statement), mirroring
  /// shouldEmitProfileViolation's context checks: a store or a callee
  /// initialization seen there earns no credit. The shared gate of
  /// recordInitProfileStore and recordNowInitArgument.
  bool inNeverExecutedContext() const;

  /// How firmly a consult may rely on recorded store credit. A `Maybe`
  /// consult asks whether a store happened somewhere earlier in parse order
  /// -- enough to *suppress* a diagnostic (the storage may well be
  /// initialized), never to fire one. A `Definite` consult uses credit as
  /// the firing basis of a diagnostic and therefore needs the store to be
  /// certain: only a store unconditionally executed in the function body
  /// owning the credited entity records it (see currentStoreStrength).
  enum class InitCreditStrength { Maybe, Definite };

  /// The strength a store (or lifetime-annotated call) recorded at the
  /// current parse position earns toward the credit keyed by \p CreditKey:
  /// Definite iff the store is unconditionally executed in the function
  /// body that owns the credited entity -- outside template instantiation
  /// (the parser scope chain is parser-only state, and the requires-uninit
  /// direction ignores credit while instantiating anyway), at conditional
  /// depth 0 (currentConditionalDepth), with the enclosing function's
  /// parse-time pattern equal to the entity's owning function: the
  /// DeclContext of a credited local/parameter (or of the directly named
  /// local base object of member credit), or the key itself for
  /// current-object member credit, which resolveMemberStoreBase already
  /// keys on that pattern. The same-function requirement is what stops a
  /// store inside a lambda body from definitely crediting an enclosing
  /// function's local; the enclosing function is resolved from the context
  /// chain directly, so a store inside a *block* body -- which
  /// getCurFunctionDecl would skip -- stays Maybe too. Everything else
  /// records Maybe.
  InitCreditStrength currentStoreStrength(const Decl *CreditKey) const;

  /// True if \p VD is a local [[uninit]] variable credited by a recorded
  /// whole-entity store of at least \p Strength; the recognizers then
  /// classify it as initialized (which also enables the paper's
  /// reverse-direction rule: a credited entity requires an unmarked target).
  bool hasWholeObjectStoreCredit(const ValueDecl *VD,
                                 InitCreditStrength Strength) const;

  /// True if \p VD is a [[ref_to_uninit]] local/parameter pointer or
  /// reference credited by a recorded store through it of at least
  /// \p Strength; the storage behind it then classifies as initialized
  /// (until a pointer is reseated -- references cannot be reseated, so no
  /// *store* ever clears their credit; a [[now_uninit]] callee withdraws
  /// either kind).
  bool hasPointeeStoreCredit(const ValueDecl *VD,
                             InitCreditStrength Strength) const;

  /// True if the [[uninit]] member \p F of the base object identified by
  /// \p Base (see resolveMemberStoreBase; null returns false) is credited by
  /// a recorded whole-member store of at least \p Strength; the member then
  /// classifies as initialized through that same base.
  bool hasMemberStoreCredit(const Decl *Base, const FieldDecl *F,
                            InitCreditStrength Strength) const;

  /// Resolve the identity key of a member access's base object for the
  /// per-object member store credit: the parse-time pattern of the enclosing
  /// function declaration for a current-object access (this->m / m /
  /// (*this).m) -- so credit recorded in one function body can never satisfy
  /// a binding in another, while a statement an instantiation reuses
  /// (unrebuilt) from its template or generic-lambda pattern agrees with a
  /// rebuilt one on the key -- or the directly named local-storage,
  /// non-reference VarDecl of a dot access (a.m). Any other base -- another
  /// member (a.b.m; §5.4 rejects deep delayed-initialization tracking), an
  /// arrow through an arbitrary pointer value, a reference (an alias to an
  /// object also reachable other ways) -- is untrackable per object: null.
  const Decl *resolveMemberStoreBase(const MemberExpr *ME) const;

  /// Depth of conditionally-evaluated *expression* regions enclosing the
  /// current parse position -- the right operand of && and ||, and the
  /// operands after a conditional's ? and : (including the GNU x ?: y
  /// form). These are the only conditional constructs that introduce no
  /// parser Scope, so the scope walk of currentConditionalDepth cannot see
  /// them; the parser bumps this counter around them instead (via
  /// ConditionalExprRegion). Deliberately not saved and restored around a
  /// lambda body nested inside a conditional expression
  /// (c ? [&]{ ... }() : 0), so stores in such a body conservatively count
  /// as conditional -- only ever losing a credit-fired diagnostic, never
  /// adding a false positive.
  unsigned ConditionalExprDepth = 0;

  /// RAII bump of ConditionalExprDepth around a conditionally-evaluated
  /// expression region; inert when \p Conditional is false (the same parse
  /// site also handles unconditional operators).
  class ConditionalExprRegion {
    SemaProfiles *SP;

  public:
    ConditionalExprRegion(SemaProfiles &SP, bool Conditional)
        : SP(Conditional ? &SP : nullptr) {
      if (this->SP)
        ++this->SP->ConditionalExprDepth;
    }
    ConditionalExprRegion(const ConditionalExprRegion &) = delete;
    ConditionalExprRegion &operator=(const ConditionalExprRegion &) = delete;
    ~ConditionalExprRegion() {
      if (SP)
        --SP->ConditionalExprDepth;
    }
  };

  /// How many conditionally-executed regions enclose the current parse
  /// position within the innermost function body: ConditionalExprDepth plus
  /// the number of conditional parser scopes from the current scope up to --
  /// and excluding -- the nearest function scope (or the top, if none). A
  /// scope counts as conditional when it carries any flag beyond a plain
  /// declaration/compound-statement block: safe by default, since a future
  /// statement kind cannot silently become "unconditional", while plain
  /// nested { } blocks correctly do not count. Stopping at the nearest
  /// function scope makes a store at a lambda body's top level depth 0
  /// *within the lambda* -- isolation from the *enclosing* function's
  /// entities is a same-function predicate's job, not depth's. Known
  /// conservatisms, each of which can only lose a credit-fired diagnostic
  /// and never adds a false positive: a store in an if/while/for
  /// *condition* or a for *init-statement* counts as conditional (the
  /// control scope is pushed before the parens are parsed), and both
  /// do { } while bodies and the taken branch of if constexpr count as
  /// conditional. Parser-only: meaningless during template instantiation,
  /// where getCurScope() does not track the instantiated function.
  unsigned currentConditionalDepth() const;

  /// Parse-order store credit (see recordInitProfileStore): one façade owns
  /// the whole-entity/pointee credit of local variables and the per-object
  /// whole-member credit, so every mutation and query is a named operation
  /// on a single seam -- the place a new kind of recorded fact (or a
  /// flow-sensitive replacement) slots in.
  ///
  /// Entries persist across the translation unit; only the named clear
  /// operations ever remove a fact. The keys are unique declarations, and
  /// template instantiations build fresh declarations, so pattern-time and
  /// instantiation-time state stay independent. A clear of an absent entry
  /// leaves a harmless zero entry behind.
  class InitStoreCreditMap {
  public:
    /// The [[uninit]] entity itself was assigned (u = e, u @= e, ++u).
    /// Every store earns the Maybe credit; a \p Strength of Definite (an
    /// unconditional store in the owning function, currentStoreStrength)
    /// earns the Definite bit besides. A store at either strength retires
    /// the destroyed state: a write to a built-in *is* its initialization,
    /// and clearing on a conditional store only converts a would-be
    /// double-destroy error into a missed diagnostic.
    void markWholeStored(const VarDecl *VD, InitCreditStrength Strength) {
      Entity[VD] |= storedBits(WholeStoredMaybe, WholeStoredDefinite,
                               Strength);
      Entity[VD] &= ~unsigned(WholeDestroyed);
    }
    /// A [[now_uninit]] callee destroyed the whole entity. \p Strength is
    /// the *destroy's* certainty: a Definite destroy withdraws credit of
    /// both strengths and records the destroyed state; a merely-possible
    /// one withdraws only the Definite claim -- it may have destroyed the
    /// storage, so no credit-fired diagnostic may rely on it, while the
    /// Maybe credit (which only ever suppresses) survives -- and records
    /// no destroyed state, since that state is a diagnostic's firing basis
    /// and must be definite by construction.
    void destroyWhole(const VarDecl *VD, InitCreditStrength Strength) {
      Entity[VD] &= ~clearedBits(WholeStoredMaybe, WholeStoredDefinite,
                                 Strength);
      if (Strength == InitCreditStrength::Definite)
        Entity[VD] |= WholeDestroyed;
    }
    bool hasWholeStored(const VarDecl *VD,
                        InitCreditStrength Strength) const {
      auto It = Entity.find(VD);
      return It != Entity.end() &&
             (It->second & queriedBit(WholeStoredMaybe, WholeStoredDefinite,
                                      Strength));
    }
    bool isWholeDestroyed(const VarDecl *VD) const {
      auto It = Entity.find(VD);
      return It != Entity.end() && (It->second & WholeDestroyed);
    }

    /// The storage behind the [[ref_to_uninit]] entity was written through
    /// the exact *p / r lvalue (strength and destroyed-state semantics as
    /// for markWholeStored).
    void markPointeeStored(const VarDecl *VD, InitCreditStrength Strength) {
      Entity[VD] |= storedBits(PointeeStoredMaybe, PointeeStoredDefinite,
                               Strength);
      Entity[VD] &= ~unsigned(PointeeDestroyed);
    }
    /// A [[now_uninit]] callee destroyed the pointee (semantics as for
    /// destroyWhole).
    void destroyPointee(const VarDecl *VD, InitCreditStrength Strength) {
      Entity[VD] &= ~clearedBits(PointeeStoredMaybe, PointeeStoredDefinite,
                                 Strength);
      if (Strength == InitCreditStrength::Definite)
        Entity[VD] |= PointeeDestroyed;
    }
    /// Reseating a marked pointer: every pointee fact -- credit of both
    /// strengths and the destroyed state -- described the old pointee, so
    /// all of it is retired wholesale, whatever the reseat's own
    /// conditionality (the parse-order status quo).
    void clearPointee(const VarDecl *VD) {
      Entity[VD] &= ~unsigned(PointeeStoredMaybe | PointeeStoredDefinite |
                              PointeeDestroyed);
    }
    bool hasPointeeStored(const VarDecl *VD,
                          InitCreditStrength Strength) const {
      auto It = Entity.find(VD);
      return It != Entity.end() &&
             (It->second & queriedBit(PointeeStoredMaybe,
                                      PointeeStoredDefinite, Strength));
    }
    bool isPointeeDestroyed(const VarDecl *VD) const {
      auto It = Entity.find(VD);
      return It != Entity.end() && (It->second & PointeeDestroyed);
    }

    /// The [[uninit]] member \p F of the base object \p Base (a
    /// resolveMemberStoreBase key) was assigned whole. Only whole-member
    /// stores are ever recorded: member *pointee* stores (*a.p = e) are
    /// deliberately never credited -- per-object pointee aliasing (copies
    /// share pointees) makes them unsound to approximate. Strength and
    /// destroyed-state semantics as for markWholeStored.
    void markMemberStored(const Decl *Base, const FieldDecl *F,
                          InitCreditStrength Strength) {
      Member[{Base, F}] |= storedBits(WholeStoredMaybe, WholeStoredDefinite,
                                      Strength);
      Member[{Base, F}] &= ~unsigned(WholeDestroyed);
    }
    /// A [[now_uninit]] callee destroyed the member (semantics as for
    /// destroyWhole).
    void destroyMember(const Decl *Base, const FieldDecl *F,
                       InitCreditStrength Strength) {
      Member[{Base, F}] &= ~clearedBits(WholeStoredMaybe, WholeStoredDefinite,
                                        Strength);
      if (Strength == InitCreditStrength::Definite)
        Member[{Base, F}] |= WholeDestroyed;
    }
    bool hasMemberStored(const Decl *Base, const FieldDecl *F,
                         InitCreditStrength Strength) const {
      auto It = Member.find({Base, F});
      return It != Member.end() &&
             (It->second & queriedBit(WholeStoredMaybe, WholeStoredDefinite,
                                      Strength));
    }
    bool isMemberDestroyed(const Decl *Base, const FieldDecl *F) const {
      auto It = Member.find({Base, F});
      return It != Member.end() && (It->second & WholeDestroyed);
    }

  private:
    /// The per-entry bits: one stored pair per strength -- a Definite store
    /// sets both bits of its pair, so the Maybe bit is exactly "any store"
    /// and the Definite bit exactly "an unconditional owner-function store"
    /// -- plus a destroyed bit per shape, recording that the storage's
    /// lifetime was definitely ended and not restarted (P4222R2 §1:
    /// destroying an object twice is an error). Destroyed is
    /// Definite-by-construction: only an unconditional same-function
    /// destroy sets it, so it can serve as a diagnostic's firing basis
    /// without a strength of its own.
    enum Flags : unsigned {
      WholeStoredMaybe = 1u << 0,
      PointeeStoredMaybe = 1u << 1,
      WholeStoredDefinite = 1u << 2,
      PointeeStoredDefinite = 1u << 3,
      WholeDestroyed = 1u << 4,
      PointeeDestroyed = 1u << 5,
    };

    /// The bits a store of \p Strength sets.
    static unsigned storedBits(unsigned MaybeBit, unsigned DefiniteBit,
                               InitCreditStrength Strength) {
      return Strength == InitCreditStrength::Definite
                 ? (MaybeBit | DefiniteBit)
                 : MaybeBit;
    }
    /// The stored bits a destroy of \p Strength clears.
    static unsigned clearedBits(unsigned MaybeBit, unsigned DefiniteBit,
                                InitCreditStrength Strength) {
      return Strength == InitCreditStrength::Definite
                 ? (MaybeBit | DefiniteBit)
                 : DefiniteBit;
    }
    /// The bit a query at \p Strength tests.
    static unsigned queriedBit(unsigned MaybeBit, unsigned DefiniteBit,
                               InitCreditStrength Strength) {
      return Strength == InitCreditStrength::Definite ? DefiniteBit
                                                      : MaybeBit;
    }

    /// Whole-entity and pointee credit, keyed by the credited
    /// local/parameter (only local-storage VarDecls carrying the relevant
    /// marker are ever inserted).
    llvm::DenseMap<const VarDecl *, unsigned> Entity;

    /// Whole-member credit, keyed per base object: the base is the directly
    /// named local-storage VarDecl (a.m = e) or, for the current object
    /// (this->m = e / m = e), the parse-time pattern of the enclosing
    /// function declaration -- so credit recorded in one function body can
    /// never satisfy a binding in another, two locals of the same type
    /// never share credit, and instantiations agree with their pattern on
    /// statements they reuse from it (see resolveMemberStoreBase).
    llvm::DenseMap<std::pair<const Decl *, const FieldDecl *>, unsigned>
        Member;
  };

  /// The recorded std::init store credit; mutated by the recorders above,
  /// consulted through the has*Credit queries.
  InitStoreCreditMap StoreCredit;

  /// std::init / ref_to_uninit (paper §5): a thrown pointer copy-initializes
  /// the exception object, which cannot carry [[ref_to_uninit]]; a no-op for
  /// a non-pointer exception object. Hosts the cluster from
  /// Sema::BuildCXXThrow.
  void checkInitProfileThrowOperand(const Expr *Operand);

  /// std::init / ref_to_uninit (paper §5): a written initializer for an
  /// allocated pointer binds it like a variable initialization, and a heap
  /// pointer object cannot carry [[ref_to_uninit]]. \p Init is the single
  /// written initializer expression, or null when there is none (a no-op).
  /// Hosts the cluster from Sema::BuildCXXNew, which calls it for scalar
  /// allocations only: an array new's written elements are each checked by
  /// the aggregate element hooks instead. An instantiation-dependent
  /// allocated type defers to the instantiation rebuild.
  void checkInitProfileNewInitializer(QualType AllocType, Expr *Init);

  /// std::init / pointer_marker + union_marker (paper §4.1, §5.6): diagnose
  /// [[uninit]] placed on a pointer, a union variable, or a union member.
  /// \p D must already carry the UninitAttr (the marker location is taken
  /// from it). Decl-aware via shouldEmitProfileViolation, so it defers on a
  /// templated pattern and is re-checked on the instantiated entity.
  void checkInitProfileMarkerPlacement(const Decl *D);

  /// [[ref_to_uninit]] is only meaningful on a pointer or reference to an
  /// object (for a function, its return type). Returns true when \p D's type
  /// is invalid for the marker, diagnosing err_ref_to_uninit_attr_invalid_type
  /// at \p AttrLoc unless \p Diagnose is false. A dependent type returns
  /// false: validation defers to the instantiation re-check in
  /// Sema::InstantiateAttrs, which drops the marker when the substituted type
  /// is invalid -- silently in a SFINAE context (\p Diagnose false there), so
  /// the marker can never affect overload resolution; a dropped marker is
  /// inert. Not profile policy -- fires regardless of -fprofiles, like the
  /// parse-time handler it serves.
  bool diagnoseInvalidRefToUninitMarker(const Decl *D, SourceLocation AttrLoc,
                                        bool Diagnose = true);

  /// [[uninit]] is meaningless on a reference (it must bind when declared).
  /// Returns true when \p D's type is a reference, diagnosing
  /// err_uninit_attr_invalid_subject at \p AttrLoc unless \p Diagnose is
  /// false. A dependent type returns false: validation defers to the
  /// instantiation re-check in Sema::InstantiateAttrs, which drops the marker
  /// when the substituted type is a reference (silently in a SFINAE context).
  /// The parameter / structured-binding rejections stay in the parse-time
  /// handler -- they do not depend on the type. Not profile policy -- fires
  /// regardless of -fprofiles.
  bool diagnoseInvalidUninitMarker(const Decl *D, SourceLocation AttrLoc,
                                   bool Diagnose = true);

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
