//===----- SemaProfiles.h ----- Semantic Analysis for C++ Profiles --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares semantic analysis for the C++ profiles framework
/// (P3589R2) and the built-in std::init initialization profile (P4222R2):
/// the parse-time checks, and the recognizers and tracked-storage predicates
/// the CFG pass in AnalysisBasedWarnings.cpp shares with them. See
/// clang/docs/ProfilesFrameworkInternals.rst for the design and
/// clang/docs/ProfilesFramework.rst for the user-facing documentation.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SEMA_SEMAPROFILES_H
#define LLVM_CLANG_SEMA_SEMAPROFILES_H

#include "clang/AST/ASTFwd.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Sema/SemaBase.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace clang {

class InitializationKind;
class InitializedEntity;
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

  /// Why default-initialization of \p T is not the genuine no-op an
  /// [[uninit]] marker claims -- as the select index of
  /// note_init_uninit_marker_type -- or std::nullopt when it is: vacuous,
  /// leaving the object (or every array element) uninitialized, the only
  /// state consistent with the marker. A non-trivial default constructor
  /// (user-provided anywhere in the subtree, a default member initializer, a
  /// virtual table pointer) initializes something, contradicting the marker
  /// (paper §4.2 rule 2, §5.3; index 0); a deleted or absent default
  /// constructor makes default-initialization ill-formed, not a no-op (the
  /// marker is unsatisfiable; index 2); an all-scalars-determinate type
  /// (e.g. an empty struct) has nothing uninitialized (index 1). Type-level
  /// (not a query on the synthesized construct-expression) because the field
  /// flavor and static_marker's no-initializer arm have no
  /// construct-expression, and getBaseElementType handles arrays and scalars
  /// uniformly. Shared by uninit_with_initializer and static_marker (through
  /// their common initializer guard) and the field-marker flavor.
  std::optional<unsigned> defaultInitNonVacuityReason(QualType T);

  /// Whether \p Init is the *shape* of a plain default-initialization -- the
  /// language's own, not something the user wrote. No initializer at all (a
  /// scalar default-init synthesizes none) qualifies, as does a synthesized
  /// default-constructor call. Every written form is excluded: a `= P()`
  /// value-initialization (a CXXTemporaryObjectExpr, which zeroes), any
  /// zero-initializing construction, and -- since a written `T x{}` on a
  /// type with a user-provided default constructor is none of those -- a
  /// construction carrying list-initialization or a written paren/brace
  /// range. The shared shape half of two separate questions: whether an
  /// [[uninit]] marker's declaration wrote an initializer (whether such a
  /// default-initialization is *vacuous* is the type's business,
  /// defaultInitNonVacuityReason, so a written initializer and a non-no-op
  /// default-initialization get different diagnostics), and whether a local
  /// aggregate's declaration ran nothing before flow-tracking starts
  /// (getTrackedLocalAggregate in AnalysisBasedWarnings.cpp, where a
  /// zero-initializing construction gives every member a value -- excluding
  /// it keeps the untracked, FP-safe direction).
  static bool isDefaultInitShape(const Expr *Init);

  /// A derived-to-base path: the canonical class of each base step from an
  /// object's class to the class declaring a member; empty for the object's
  /// own members. Two copies of one member reached through different bases
  /// (a non-virtual diamond) have different paths.
  using BasePath = SmallVector<const CXXRecordDecl *, 2>;

  /// Peel the transparent casts ignoreTransparentCasts peels -- parens,
  /// implicit casts, and explicit casts of a pointer or glvalue -- from
  /// \p E, prepending the derived-to-base steps they take to \p Path in
  /// object-to-member order; returns the operand reached.
  static const Expr *peelBaseCasts(const Expr *E, BasePath &Path);

  /// The shape of an lvalue the std::init flow analysis may track: the
  /// object expression at the bottom of the member, element, and
  /// dereference chain, and the chain itself. Shared by the parse-time
  /// tracked-leaf predicate (isFlowTrackedLeaf) and the CFG pass's entity
  /// resolver (AnalysisBasedWarnings.cpp), so the two never disagree on what
  /// is tracked.
  struct FlowLeafShape {
    /// The directly named local or parameter at the bottom; null when the
    /// chain bottoms out at `this`.
    const VarDecl *Local = nullptr;
    /// Local's DeclRefExpr (the base a consuming member access marks benign).
    const DeclRefExpr *LocalRef = nullptr;
    bool IsThis = false;
    /// The chain reaches its storage through a pointer value -- `*p`,
    /// `p->m`, `p[i]`, `this->m` -- rather than through the object itself.
    bool ThroughPointer = false;
    /// The named member steps, leaf first; anonymous-record steps are
    /// transparent.
    SmallVector<const FieldDecl *, 2> Named;
    /// An element step (`a[i]`) occurs in the chain.
    bool Subscript = false;
    /// The derived-to-base path from the object's class to the innermost
    /// member's class.
    BasePath Path;
  };
  /// The shape of the glvalue \p E, or none when its chain bottoms out at
  /// anything but a directly named local, a parameter, or `this` (a global,
  /// a call, a member reached through another member's pointer).
  static std::optional<FlowLeafShape> flowLeafShape(const Expr *E);

  /// A class with a user-provided constructor is trusted (P4222R2 §5.1): its
  /// constructor body may have assigned a member, which local analysis
  /// cannot see, so its members are not flow-tracked for reads.
  static bool hasUserProvidedCtor(const CXXRecordDecl *RD);

  /// Visit the candidate fields of \p RD and of its non-virtual,
  /// constructor-less base classes, recursively, each with its
  /// derived-to-base path from \p RD.
  template <typename Fn>
  static void forEachCandidateUninitField(const CXXRecordDecl *RD, Fn Visit) {
    SmallVector<std::pair<const CXXRecordDecl *, BasePath>, 4> RecordStack;
    RecordStack.push_back({RD, BasePath()});
    while (!RecordStack.empty()) {
      auto [Cur, Path] = RecordStack.pop_back_val();
      for (const FieldDecl *F : Cur->fields())
        Visit(Path, F);
      for (const CXXBaseSpecifier &BS : Cur->bases()) {
        if (BS.isVirtual())
          continue;
        const CXXRecordDecl *BRD = BS.getType()->getAsCXXRecordDecl();
        if (BRD && BRD->hasDefinition() &&
            !hasUserProvidedCtor(BRD->getDefinition())) {
          BasePath BP(Path);
          BP.push_back(BRD->getCanonicalDecl());
          RecordStack.push_back({BRD->getDefinition(), std::move(BP)});
        }
      }
    }
  }

  /// True if \p F is a member the flow analysis tracks: an [[uninit]]
  /// built-in scalar (arithmetic or enum) member with no default member
  /// initializer, std::byte excepted (P4222R2 §4.6).
  static bool isFlowTrackedMemberField(const ASTContext &Ctx,
                                       const FieldDecl *F);
  /// True if \p F, reached through \p Path from \p RD, is a tracked member
  /// of an object of class \p RD.
  static bool isFlowTrackedMemberOf(const ASTContext &Ctx,
                                    const CXXRecordDecl *RD,
                                    ArrayRef<const CXXRecordDecl *> Path,
                                    const FieldDecl *F);

  /// The type-shape half of the read-tracking guards: a non-union,
  /// non-dependent class with no user-provided constructor anywhere in the
  /// contributing subtree. A reference type aliases an object also reachable
  /// other ways and never qualifies.
  static const CXXRecordDecl *getTrackableSlotClass(QualType T);
  /// The declaration-shape half of the read-tracking guards for a local:
  /// \p V is a non-parameter local, not itself [[uninit]]-marked, of a
  /// trackable class. How its declaration initializes the members is the
  /// caller's half.
  static const CXXRecordDecl *getTrackableLocalClass(const VarDecl *V);
  /// The class whose [[uninit]] scalar members the flow analysis tracks for
  /// the local or by-value parameter \p V -- any non-reference,
  /// non-dependent class type of a local-storage variable that is not itself
  /// [[uninit]] -- or null. Reads are tracked for the narrower
  /// getTrackableLocalClass / getTrackableSlotClass subset only; bindings of
  /// a member are judged by flow state for every such object.
  static const CXXRecordDecl *flowTrackedAggregateClass(const VarDecl *V);

  /// The non-lambda instance member function whose object `this` denotes in
  /// \p DC -- walking out of lambda call operators and blocks -- or null.
  static const CXXMethodDecl *enclosingInstanceMethod(const DeclContext *DC);

  /// True if the leaf \p Leaf of a binding source names storage the
  /// std::init CFG pass flow-tracks -- an [[uninit]] local or parameter (or
  /// a subobject of one), a marked local pointer's or reference's referent
  /// (or a subobject of it), or a tracked [[uninit]] scalar member of a
  /// directly named local or of the current object -- while a function body
  /// is being parsed (\p CurContext inside one). \p AsPointerValue says the
  /// leaf is used as a pointer value (a pointer binding, or a read-only alias
  /// binding of a pointer) rather than as a glvalue. The parse-time checks
  /// leave such a source to the pass; a source with no tracked leaf, or one
  /// outside any function body, is judged at parse time without flow state.
  static bool isFlowTrackedLeaf(const ASTContext &Ctx, const Expr *Leaf,
                                bool AsPointerValue,
                                const DeclContext *CurContext);
  /// True if any leaf of \p Src, bound as \p T, is flow-tracked in the
  /// current context (isFlowTrackedLeaf).
  bool hasFlowTrackedLeaf(const Expr *Src, QualType T) const;
  /// True if any leaf of the glvalue \p G is flow-tracked in the current
  /// context: the read-through and subobject-write checks leave such an
  /// access to the CFG pass.
  bool hasFlowTrackedGlvalueLeaf(const Expr *G) const;
  /// The operand a binding of \p Src as \p T classifies, and whether it is
  /// used as a pointer value (\p AsPointerValue): a pointer binding or a
  /// read-only alias of a pointer reads the source's value; a reference
  /// binding names the source glvalue -- unless the source materializes a
  /// temporary, a fresh object that is initialized (null: no tracked
  /// storage) except when pointer-typed, where the temporary's value still
  /// refers to what its operand's does (a read-only alias). The recognizers
  /// classify the same way (glvalueDenotesUninitStorage), so the parse-time
  /// funnel and the CFG pass agree on which leaves a binding has.
  static const Expr *bindingSourceOperand(const Expr *Src, QualType T,
                                          bool &AsPointerValue);

  /// How a bound type aliases a pointer glvalue source (P4222R2 §4.3): a
  /// reference whose referent is a pointer is a read-only alias when the
  /// referent is const-qualified or the reference is an rvalue reference, a
  /// mutable alias otherwise; any other bound type aliases nothing.
  enum class PointerAliasKind { None, ReadOnly, Mutable };
  static PointerAliasKind pointerAliasKind(QualType T);

  /// The recognizers' verdict on a binding source or leaf, without flow
  /// state: Initialized, Uninitialized, Unknown, or Mixed (a conditional
  /// whose arms disagree, P4222R2 §4.9).
  enum class InitSourceState { Initialized, Uninitialized, Unknown, Mixed };
  /// Classify the leaf \p Leaf of a binding source bound as \p T by its form
  /// alone, for the CFG pass's judgment of a binding with tracked leaves;
  /// \p Body is the analyzed function, which decides whether `this` is under
  /// construction.
  InitSourceState classifyInitBindingLeaf(const Expr *Leaf, QualType T,
                                          const Decl *Body) const;
  /// The access a leaf is classified for: a value read (the top-level
  /// [[uninit]] marker is the flow-based read pass's) or a scalar store (the
  /// whole named entity's store is its initialization; storage reached
  /// through [[ref_to_uninit]] is trusted at the top level only).
  enum class InitAccessKind { Read, Write };
  /// Classify the leaf \p Leaf of a read or store glvalue by its form alone
  /// with the recognizers' access preset for \p Kind, for the CFG pass's
  /// judgment of an access with tracked leaves; \p Body as for
  /// classifyInitBindingLeaf.
  InitSourceState classifyInitAccessLeaf(const Expr *Leaf, InitAccessKind Kind,
                                         const Decl *Body) const;

  /// The [[ref_to_uninit]] marking of the pointer object an assignment
  /// target names: true/false for a directly named marked/unmarked pointer
  /// declaration, std::nullopt when the marking is unknown -- a reference to
  /// a pointer aliases an object whose marking it cannot carry, any other
  /// lvalue (*pp, arr[i]) names no declaration at all, and a conditional's
  /// arms may disagree -- where neither direction of the binding check is
  /// sound (P4222R2 §4.3). Peels transparent casts and walks the
  /// pass-through target shapes.
  static std::optional<bool> resolveAssignTargetMarking(const Expr *E);

  /// If \p E (stripped of parens and implicit casts) directly names a
  /// declaration -- a DeclRefExpr or a MemberExpr -- return that declaration;
  /// otherwise null. The std::init checks read [[ref_to_uninit]] /
  /// [[uninit]] markers only off a directly named entity.
  static const ValueDecl *getDirectlyNamedDecl(const Expr *E);

  /// Strip what the std::init recognizers see through on the way to a named
  /// entity: parens, implicit casts, and explicit casts whose operand is a
  /// pointer or glvalue (paper §4.3: a cast of a marked pointer is itself
  /// marked; a reference cast denotes the same storage). Value casts like
  /// `(int)m` are not stripped -- they produce a new value, not the same
  /// storage. Shared with the CFG pass in AnalysisBasedWarnings.cpp, so
  /// recognition and flow tracking see through exactly the same casts.
  static const Expr *ignoreTransparentCasts(const Expr *E);

  /// The construct a pointer or reference binding belongs to, selecting
  /// which declaration can carry [[ref_to_uninit]] and which diagnostic
  /// wording applies (see checkInitProfileBinding).
  enum class InitBindingKind : unsigned {
    /// A variable's initializer, an init-capture included; the target is the
    /// VarDecl.
    Variable,
    /// A data member's initializer -- NSDMI, mem-initializer, or an
    /// aggregate's field element; the target is the FieldDecl.
    DataMember,
    /// A call argument's parameter copy-initialization, or a default
    /// argument at its declaration; the target is the ParmVarDecl, null for
    /// a call with no declared callee.
    Parameter,
    /// A return statement; the target is the function (its return type's
    /// marker) or the lambda call operator.
    Return,
    /// An array, vector, or complex element of an aggregate initializer: no
    /// declaration carries the marker.
    AggregateElement,
    /// A built-in pointer assignment; the target's marking is resolved from
    /// the left operand (checkInitProfilePointerAssignment).
    PointerAssignment,
    /// A thrown pointer copy-initializing the exception object, which cannot
    /// carry the marker.
    Throw,
    /// The written initializer of a scalar new-expression allocating a
    /// pointer; a heap pointer object cannot carry the marker.
    NewInitializer,
    /// A pointer promoted through a `...` parameter, which cannot carry the
    /// marker. Checked from the two C++ promotion loops
    /// (Sema::GatherArgumentsForCall, Sema::BuildCallToObjectOfClassType),
    /// not from Sema::DefaultVariadicArgumentPromotion, whose other callers
    /// re-promote arguments or match ObjC methods and would double-fire.
    VariadicArgument,
    /// A by-copy lambda capture of a pointer into a closure field, which
    /// cannot carry the marker.
    ByCopyCapture,
    /// A by-reference lambda capture of a named variable; the closure's
    /// reference cannot carry the marker.
    ByRefCapture,
    /// A member call's implicit object parameter
    /// (checkInitProfileObjectArgument), which cannot carry the marker.
    ObjectArgument
  };

  /// std::init / ref_to_uninit: a pointer promoted through a `...` parameter
  /// binds an argument that cannot carry the marker (VariadicArgument).
  /// Called from the two C++ promotion loops, Sema::GatherArgumentsForCall
  /// and Sema::BuildCallToObjectOfClassType, with the promoted argument.
  void checkInitProfileVariadicArgument(const Expr *Arg);

  /// The entity-driven entry to the binding funnel: judge the binding an
  /// InitializationSequence performs for \p Entity from \p Init, the
  /// sequence's converted result (a materialized temporary is initialized
  /// memory), called once per sequence from InitializationSequence::Perform
  /// beside checkInitializerLifetime. The entity's kind selects the
  /// InitBindingKind, its target, and its deferral anchor. A braced or
  /// parenthesized list initializing an object is left to the sequences its
  /// elements perform (a reference list-initializes from its lone element); a
  /// SFINAE context, a rebuild of a default argument or initializer, an
  /// implicit member initialization, and an entity kind whose binding can
  /// carry no marker and names no construct of its own are skipped.
  void checkInitProfileBinding(const InitializedEntity &Entity,
                               const InitializationKind &Kind,
                               const Expr *Init);

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
  /// pass; a read with a flow-tracked leaf is the CFG pass's
  /// (ProfilesFrameworkInternals.rst, "Flow-Tracked Storage"). A std::byte
  /// read is exempt (P4222R2 §4.6). Template deferral follows the
  /// expression-check policy (ProfilesFrameworkInternals.rst, "Pattern 1").
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
  /// a below-top-level [[uninit]] marker fires; a store with a flow-tracked
  /// leaf is the CFG pass's (ProfilesFrameworkInternals.rst, "Flow-Tracked
  /// Storage"). A std::byte store is exempt (P4222R2 §4.6). Template
  /// deferral follows the expression-check policy
  /// (ProfilesFrameworkInternals.rst, "Pattern 1").
  void checkInitProfileSubobjectWrite(SourceLocation Loc, const Expr *LHS);

  /// The ObjectArgument derive step of checkInitProfileBinding: a member call
  /// binds its implicit object parameter, which cannot carry the marker, to
  /// \p Object. Called from \c
  /// Sema::PerformImplicitObjectArgumentInitialization, the funnel every
  /// member-call flavor's object argument converts through
  /// -- dot and arrow calls, member operators, functor operator(),
  /// operator->, and conversion operators. Explicit-object member functions
  /// initialize their object as an ordinary parameter and are checked there;
  /// a destructor call is skipped (destruction of uninitialized storage is
  /// the deferred destroy_at slice), as is a static call operator (no
  /// implicit object parameter, like a static member call).
  void checkInitProfileObjectArgument(const Expr *Object,
                                      const CXXMethodDecl *Method);

  /// std::init: the checks every built-in assignment hosts, from
  /// Sema::CheckAssignmentOperands beside checkAssignmentLifetime, with
  /// \p RHS the converted right operand: the compound-assignment old-value
  /// load (read-through -- excluding the shifts, whose LHS promotion already
  /// loads through the lvalue-to-rvalue chokepoint), the subobject-write
  /// check (paper §5.4-§5.6), and for a simple assignment to a pointer the
  /// ref_to_uninit judgment of the source against the assigned-to pointer's
  /// marking (§4.3). \p IsCompound distinguishes `op=` from `=`
  /// (!CompoundType.isNull() at the host site).
  void checkInitProfileAssignmentOperands(BinaryOperatorKind Opc, Expr *LHSExpr,
                                          Expr *RHS, bool IsCompound,
                                          SourceLocation OpLoc);

  /// std::init: the check pair a built-in ++/-- hosts -- the old-value load
  /// (read-through) and the store (subobject-write). Hosts the cluster from
  /// Sema::CreateBuiltinUnaryOp's increment/decrement arm.
  void checkInitProfileIncDec(Expr *Operand, SourceLocation OpLoc);

  /// What a direct callee does to the storage bound to its parameters,
  /// derived from the callee's lifetime attributes and the allocator-callee
  /// table. Each consumer reads only the bits its direction may rely on:
  /// binding *acceptance* (which never diagnoses) reads the union of the
  /// destroy and release bits, the CFG pass's Kill (a diagnostic's firing
  /// basis) reads the trusted release bit only, and double_destroy keys on
  /// the destroy bit only (a storage release leaves no object to destroy
  /// again).
  struct CalleeLifecycleRoles {
    /// [[now_init]]: the callee initializes the storage bound to its
    /// [[ref_to_uninit]] parameters (P4222R2 §6.2).
    bool InitializesRefToUninitParams = false;
    /// [[now_uninit]]: the callee ends the lifetime of the storage bound
    /// to its pointer/reference parameters (§4.4).
    bool DestroysPointerParams = false;
    /// A trusted storage-release callee (builtin ID or operator form):
    /// free, realloc's pointer, replaceable global operator delete.
    bool ReleasesStorageTrusted = false;
    /// A name-only (untrusted) release match -- -fno-builtin /
    /// -ffreestanding strips the ID: the name says what the function is
    /// meant to be, but its semantics cannot be assumed, so this bit feeds
    /// acceptance only.
    bool ReleasesStorageByName = false;
  };
  /// Derive what \p FD does to the storage bound to its parameters: the
  /// lifetime attributes plus the allocator-callee table's storage-release
  /// rows (free, realloc's pointer parameter, replaceable global operator
  /// delete / operator delete[]), split by trust. Shared by the parse-time
  /// funnel and the CFG pass's lifecycle arm.
  static CalleeLifecycleRoles getCalleeLifecycleRoles(const FunctionDecl *FD);

private:
  /// True if `this` in the current context denotes an object under
  /// construction -- the enclosing non-lambda function is a constructor, or
  /// the context is a class (a default member initializer) -- so the
  /// recognizers classify it Unknown rather than Initialized: its members may
  /// not be initialized yet.
  bool thisIsUnderConstruction() const;

  /// std::init / ref_to_uninit (P4222R2 §4.2-§4.3): judge binding \p Src as
  /// \p T to \p Target -- null for a construct with no declaration to carry
  /// the marker -- against the target's marking: a marked target must refer
  /// to uninitialized memory, an unmarked one must not. The one binding
  /// funnel: every host passes its \p Kind, and a kind whose construct cannot
  /// carry the marker is judged unmarked whatever \p Target is. No-op unless
  /// \p T is a non-dependent pointer or reference. \p D, when available,
  /// anchors suppression and template deferral: with it the rule fires on
  /// the instantiation only; without it an instantiation-dependent \p Src
  /// defers to the rebuild and a non-dependent one is judged on the pattern
  /// and again at each instantiation that rebuilds the construct
  /// (ProfilesFrameworkInternals.rst, "Pattern 1"). A Parameter binding of
  /// a [[now_uninit]] or storage-release callee
  /// runs the destroy rules instead -- at parse time for a source with no
  /// flow-tracked leaf, in the CFG pass otherwise
  /// (ProfilesFrameworkInternals.rst, "Flow-Tracked Storage").
  void checkInitProfileBinding(InitBindingKind Kind, SourceLocation Loc,
                               const ValueDecl *Target, QualType T,
                               const Expr *Src, const Decl *D = nullptr);

  /// The PointerAssignment derive step of checkInitProfileBinding, from
  /// checkInitProfileAssignmentOperands: assigning to a pointer must respect
  /// the assigned-to pointer's [[ref_to_uninit]] marking, resolved from the
  /// left operand through transparent casts and conditional, comma, and GNU
  /// ?: target shapes; a no-op for a non-pointer LHS. An
  /// instantiation-dependent LHS defers to the instantiation rebuild (its
  /// marker cannot be read yet); the source's deferral is the funnel's.
  void checkInitProfilePointerAssignment(Expr *LHS, Expr *RHS,
                                         SourceLocation OpLoc);

  /// The classify-and-judge half of checkInitProfileBinding, shared with the
  /// derive steps that resolve the target's marking themselves (a conditional
  /// assignment target, a member call's implicit object parameter): defer,
  /// gate, classify \p Src once as bound to \p T -- a reference to a pointer
  /// aliases the source glvalue (classifyPointerGlvalue) -- and diagnose the
  /// verdict in \p Kind's wording. \p Subject is the entity a kind-specific
  /// wording names (the called method).
  void judgeInitProfileBinding(InitBindingKind Kind, SourceLocation Loc,
                               bool TargetMarked, QualType T, const Expr *Src,
                               const Decl *D,
                               const NamedDecl *Subject = nullptr);

public:
  /// Enumerate the lvalue leaves the store target, binding source, or
  /// lifecycle argument \p E can name: peel transparent casts and
  /// single-element braced initializers, walk conditional arms and comma
  /// right operands, and hand each leaf to \p F. The one target-shape walk
  /// shared by the parse-time checks and the CFG pass's event extraction
  /// (AnalysisBasedWarnings.cpp), so a wrapped lvalue is judged exactly like
  /// its leaf form.
  static void forEachTargetLeaf(const Expr *E,
                                llvm::function_ref<void(const Expr *Leaf)> F);

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

  /// A [[now_init]] function with no [[ref_to_uninit]] parameter is a
  /// vacuous promise (P4222R2 §6.2): diagnose
  /// err_now_init_attr_no_marked_parameter and drop the attribute. Called
  /// from ActOnFunctionDeclarator after CheckFunctionDeclaration has merged
  /// parameter attributes from previous declarations, so a marker written on
  /// any declaration counts (the parse-time attribute handler runs before
  /// merging and cannot see an inherited marker). An inherited [[now_init]]
  /// is skipped: the declaration that wrote it was already checked. Not
  /// profile policy -- fires regardless of -fprofiles, like the parse-time
  /// marker subject checks.
  void checkNowInitVacuity(FunctionDecl *FD);

  /// std::init: attach the lifecycle markers to a matching std::construct_at
  /// (RefToUninit on the first parameter + NowInit) or std::destroy_at
  /// (NowUninit) declaration, so the real library functions work under
  /// enforcement -- there is no portable way to annotate namespace-std
  /// declarations from user code. Keyed on form (a first parameter of
  /// pointer type, dependent or not), so the marker subject rules hold by
  /// construction; iterator-shaped relatives (destroy_n, the uninitialized_*
  /// family) and ranges:: CPOs are out of scope. Idempotent across
  /// redeclarations and future library annotations. Called from
  /// Sema::AddKnownFunctionAttributes; see ProfilesFrameworkInternals.rst.
  void addKnownInitLifecycleAttributes(FunctionDecl *FD);
};

} // namespace clang

#endif // LLVM_CLANG_SEMA_SEMAPROFILES_H
