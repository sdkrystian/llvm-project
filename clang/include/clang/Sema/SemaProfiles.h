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
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
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

  /// True if \p Profile is live in this compilation: enforced by this unit
  /// or an imported module unit (ASTContext::isProfileEnforcedByAnyUnit), or
  /// with a rule diagnostic enabled by a command-line option
  /// (ASTContext::isProfileRuleEnabledByOption). Every check of a profile
  /// runs behind its liveness (the first rung of shouldEmitProfileViolation);
  /// an entry point that does work before its first gate hoists the same
  /// test. Never true for an inert test:: profile. See
  /// ProfilesFrameworkInternals.rst, "Enforcement and Suppression State".
  bool isProfileLive(StringRef Profile);

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

  /// True if a violation of \p Profile at \p Loc diagnosed by \p DiagID is
  /// to be diagnosed: the profile is live (isProfileLive), the rule is
  /// enforced or enabled and not suppressed at \p Loc and \p Loc is not
  /// system-header-exempt (ASTContext::isProfileRuleActiveAt), plus the
  /// parse-time rungs -- a templated \p D never fires (the rule fires on the
  /// instantiation; see ProfilesFrameworkInternals.rst, "Check Sites"), nor
  /// does a site in an unevaluated or discarded context. A \p PostParse
  /// site (a CFG analysis, which has no evaluation context of its own) skips
  /// the context rungs.
  bool shouldEmitProfileViolation(StringRef Profile, unsigned DiagID,
                                  SourceLocation Loc, const Decl *D = nullptr,
                                  bool PostParse = false);

  /// A [[profiles::suppress]] dominion being recorded as diagnostic state:
  /// where it begins and, per diagnostic of the suppressed rules, the change
  /// the begin made (the mapping in effect there to ignored), which the end
  /// reverses. Empty when the attributes suppressed nothing that was not
  /// already ignored.
  struct SuppressionRecord {
    SourceLocation Begin;
    SmallVector<DiagnosticsEngine::DiagnosticMappingChange, 8> Changes;
  };

  /// Record into \p Record that the dominion of the [[profiles::suppress]]
  /// attributes among \p Attrs begins at \p Begin: every diagnostic of each
  /// attribute's rule group (the profile's group when it names no rule) is
  /// mapped to ignored from the expansion site of \p Begin on. A valid
  /// \p Frontier, the last token of the token-cached region being parsed
  /// late, bounds the states recorded ahead that the mapping reaches. See
  /// ProfilesFrameworkInternals.rst, "Enforcement and Suppression State".
  void beginSuppression(const ParsedAttributesView &Attrs, SourceLocation Begin,
                        SourceLocation Frontier, SuppressionRecord &Record);
  /// The same for the ProfilesSuppressAttrs attached to \p D (a template's
  /// templated declaration); an invalid \p Begin starts the dominion at the
  /// attributes themselves.
  void beginSuppression(const Decl *D, SourceLocation Begin,
                        SourceLocation Frontier, SuppressionRecord &Record);
  /// Record that \p Record's dominion ends at \p End: each mapping its begin
  /// replaced is restored from there on, except where a pragma inside the
  /// dominion remapped the diagnostic. An \p End not after the begin
  /// (nothing was consumed) ends the dominion where it began. \p Frontier
  /// is as for beginSuppression.
  void endSuppression(SuppressionRecord &Record, SourceLocation End,
                      SourceLocation Frontier);

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

  /// Run every profile's class-completion checks for a completed class.
  /// Called from \c Sema::CheckCompletedCXXClass, which every
  /// class-completion path (parsing, template instantiation, lambda
  /// completion) reaches. Dependent, invalid, and lambda classes are filtered
  /// here; a check adds its own filters.
  void checkProfileViolationsAtClassFinalization(CXXRecordDecl *RD);

  /// Run every profile's constructor-finalization checks once a constructor's
  /// member-initializer list is complete. Called from
  /// \c ActOnMemInitializers, \c ActOnDefaultCtorInitializers (both also
  /// serving template instantiation through \c InstantiateMemInitializers),
  /// and \c SetDeclDefaulted for an out-of-line '= default'. Class completion
  /// runs before any constructor body is parsed; this hook runs with
  /// \c inits() fully populated. Dependent, invalid, and delegating
  /// constructors are filtered here; a check adds its own filters.
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

  /// True if no marker rule rejects the [[uninit]] on \p Var: it has no
  /// written initializer and its default-initialization runs no code.
  bool isConsistentUninitMarker(const VarDecl *Var);

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
  /// subobject with an indeterminate value. A data member marked [[uninit]]
  /// is treated as acknowledged and skipped (the marker excuses the member,
  /// paper §6.2), so a type whose indeterminate scalars are all marked is
  /// determinate here. Shared by the std::init rules uninit_decl (at the
  /// variable declaration) and ctor_uninit_member (for a class-typed
  /// member). A class with a user-provided default constructor is trusted
  /// (that constructor is checked at its own definition). Dependent and
  /// incomplete types are treated as determinate.
  bool defaultInitLeavesScalarIndeterminate(QualType T);

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

  /// One leaf of an lvalue or pointer shape (forEachTargetLeaf): the
  /// expression \c E, and the arm \c E takes at each conditional nested
  /// below one of its member, element, indirection, or address-of steps (an
  /// arm is taken when it is listed). A plain expression is a leaf with no
  /// nested conditional. \c Arms is valid only while the walk that yields the
  /// leaf runs.
  struct ShapeLeaf {
    const Expr *E;
    ArrayRef<const Expr *> Arms;
    ShapeLeaf(const Expr *E, ArrayRef<const Expr *> Arms = {})
        : E(E), Arms(Arms) {}
  };

  struct HandleLeaf;

  /// Peel what ignoreTransparentCasts peels from \p E, taking the arms
  /// listed in \p Arms, and prepend the derived-to-base steps its casts take
  /// to \p Path in object-to-member order; returns the operand reached.
  static const Expr *peelBaseCasts(const Expr *E, BasePath &Path,
                                   ArrayRef<const Expr *> Arms = {});

  /// One member step of an lvalue chain: the derived-to-base path from the
  /// object the step accesses to \c Field's class, then \c Field. A null
  /// \c Field is a pointer-to-data-member step (\c .* or \c ->*;
  /// asDataMemberPointerAccess), which names no member. Steps compare fields
  /// by canonical declaration, so a definition merged from modules matches.
  struct MemberStep {
    BasePath Bases;
    const FieldDecl *Field = nullptr;
    bool operator==(const MemberStep &O) const {
      return (Field ? Field->getCanonicalDecl() : nullptr) ==
                 (O.Field ? O.Field->getCanonicalDecl() : nullptr) &&
             Bases == O.Bases;
    }
  };
  /// The member steps from an object to a subobject, object first.
  using MemberPath = SmallVector<MemberStep, 1>;

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
    /// The named member steps, object first. Anonymous-record steps are
    /// transparent: the casts on their objects fold into the next named step
    /// toward the leaf.
    MemberPath Members;
    /// The number of steps of \c Members before the first element step.
    unsigned ElementFreePrefix = 0;
    /// An element step (`a[i]`) occurs in the chain.
    bool Subscript = false;
    /// The derived-to-base path the leaf's own casts take from the object
    /// the last step of \c Members designates (from the bottom object when
    /// \c Members is empty).
    BasePath LeafBases;
    /// For a chain through a pointer member of the current object
    /// (\c this->mem.elem->x, \c *elem), the member steps from the current
    /// object to that pointer; \c Members are then the steps below its
    /// referent. Empty otherwise.
    MemberPath PointerMembers;
    /// The chain reaches its storage through a member call that derives a
    /// pointer or reference from a handle (\c *up, \c up->m, \c s[i];
    /// isHandleDerefCall) whose chain bottoms out at a handle variable
    /// (handleChainBase): \c Local is that variable, \c ThroughPointer is
    /// set, and an element call or an offset handle sets \c Subscript. A
    /// chain bottoming out at a pointer sets \c ThroughPointer alone, and one
    /// at storage \c Subscript alone.
    bool ThroughHandle = false;
  };
  /// The shape of the glvalue \p E, taking the arms listed in \p Arms, or
  /// none when its chain bottoms out at anything but a directly named local,
  /// a parameter, or \c this (a global, a call, a member reached through
  /// another member's pointer). An indirection of an address-of step
  /// (\c *&x, \c (&x)->m, \c std::addressof(x)->m) continues from its
  /// operand.
  static std::optional<FlowLeafShape>
  flowLeafShape(const Expr *E, ArrayRef<const Expr *> Arms = {});

  /// A class with a user-provided constructor is trusted (P4222R2 §5.1): its
  /// constructor body may have assigned a member, which local analysis
  /// cannot see, so its members are not flow-tracked for reads.
  static bool hasUserProvidedCtor(const CXXRecordDecl *RD);

  /// Visit the member path of each member the flow analysis tracks in an
  /// object of class \p RD, with whether it is tracked as a whole object
  /// (\p Whole), which the enumeration never descends into. The walk is
  /// depth-first -- a class's fields in order, then its non-virtual bases
  /// without a user-provided constructor -- so the paths under any prefix
  /// are contiguous. Tracked are the [[uninit]] members with no default
  /// member initializer of a built-in scalar (arithmetic or enum) type,
  /// std::byte excepted (P4222R2 §4.6), and, as whole objects, of a
  /// trackable class type (getTrackableSlotClass) or of a complete array
  /// type of such a scalar or class element type. The walk enters a
  /// container -- a named, unmarked, non-reference, non-array member with no
  /// default member initializer whose type is a trackable class -- while a
  /// path has fewer than 8 steps and fewer than 64 paths were visited. An
  /// anonymous struct's members count as the enclosing class's, with no step
  /// of their own; an anonymous union is never entered.
  static void forEachFlowTrackedMember(
      const ASTContext &Ctx, const CXXRecordDecl *RD,
      llvm::function_ref<void(ArrayRef<MemberStep>, bool Whole)> Visit);

  /// One entry of a class's flow-tracked member table: the member path and
  /// whether the member is tracked as a whole object.
  using FlowTrackedMember = std::pair<MemberPath, bool>;
  /// The flow-tracked members of an object of class \p RD, in
  /// forEachFlowTrackedMember order; cached per complete class, and empty
  /// for a class without a definition or still being defined.
  ArrayRef<FlowTrackedMember> flowTrackedMembers(const CXXRecordDecl *RD) const;

  /// Visit the member path of each [[ref_to_uninit]] pointer member of an
  /// object of class \p RD, reached through named, non-reference members of
  /// class type -- user-provided constructors allowed -- and non-virtual
  /// bases, with forEachFlowTrackedMember's walk order, transparent
  /// anonymous structs, and bounds.
  static void forEachMarkedPointerMember(
      const CXXRecordDecl *RD,
      llvm::function_ref<void(ArrayRef<MemberStep>)> Visit);
  /// The paths of forEachMarkedPointerMember for \p RD, cached as
  /// flowTrackedMembers is.
  ArrayRef<MemberPath> markedPointerMembers(const CXXRecordDecl *RD) const;
  /// The index of \p Path in markedPointerMembers(\p RD), if there.
  std::optional<unsigned>
  findMarkedPointerMember(const CXXRecordDecl *RD,
                          ArrayRef<MemberStep> Path) const;

  /// A match of an lvalue's member steps against a class's flow-tracked
  /// member table: the entry's index, and its path's length.
  struct FlowMemberMatch {
    unsigned Index;
    unsigned Length;
  };
  /// The entry of \p RD's flow-tracked member table whose path is a prefix
  /// of the element-free steps of \p Shape (at most one is), if any. The
  /// storage \p Shape names is a subobject of that member when \p Shape has
  /// more steps or an element step.
  std::optional<FlowMemberMatch>
  matchFlowTrackedMember(const CXXRecordDecl *RD,
                         const FlowLeafShape &Shape) const;
  /// The entries of \p RD's flow-tracked member table below the subobject
  /// reached through the member steps \p Prefix and then the derived-to-base
  /// path \p Bases -- the object itself, a base subobject, or a member
  /// enclosing tracked members -- as a contiguous index range; empty when
  /// none is.
  std::pair<unsigned, unsigned>
  flowTrackedMemberRange(const CXXRecordDecl *RD, ArrayRef<MemberStep> Prefix,
                         ArrayRef<const CXXRecordDecl *> Bases) const;

  /// The type-shape half of the read-tracking guards: a non-union,
  /// non-dependent class with no user-provided constructor anywhere in the
  /// contributing subtree. A reference type aliases an object also reachable
  /// other ways and never qualifies.
  static const CXXRecordDecl *getTrackableSlotClass(QualType T);
  /// The class of \p T when default-initializing an object of it runs no
  /// user-provided constructor of it: a non-union, non-dependent class whose
  /// default constructor is not user-provided, whatever other constructors it
  /// declares. The flow analysis enters a member of such a class as a
  /// container.
  static const CXXRecordDecl *getDefaultInitTrackableClass(QualType T);
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

  /// True if \p V is a handle variable whose referent the flow analysis
  /// tracks: a local or parameter of handle type (isHandleType) marked
  /// [[ref_to_uninit]].
  static bool isFlowTrackedHandleVar(const VarDecl *V);
  /// The variable a handle chain \p E bottoms out at -- stepping through
  /// member calls that return the handle itself, copies, a std-table
  /// construction to its first transfer argument (a pointer's value, or
  /// storage), and implicit locals and unmarked local references not reached
  /// by capture, and taking the arms listed in \p Arms -- or null;
  /// \p Partial is set when a step refers to part of the handle, and
  /// \p PastStart, if non-null, when a step positions it past its referent's
  /// start (every such step but a prefix: first(n), subspan(0, n)).
  static const DeclRefExpr *handleChainBase(const Expr *E,
                                            ArrayRef<const Expr *> Arms,
                                            bool &Partial,
                                            bool *PastStart = nullptr);
  /// True if \p E is a member call that derives a pointer or reference from
  /// a handle, its object (\p Obj): \p Element is false for \c operator*,
  /// \c operator->, and a member with no argument returning a pointer
  /// (\c get(), \c data()), which designate the whole referent, and true for
  /// any other (\c s[i], \c front()), which designates an element.
  static bool isHandleDerefCall(const Expr *E, const Expr *&Obj, bool &Element);

  /// The non-lambda instance member function whose object `this` denotes in
  /// \p DC -- walking out of lambda call operators and blocks -- or null.
  static const CXXMethodDecl *enclosingInstanceMethod(const DeclContext *DC);

  /// What a leaf is bound to, for isFlowTrackedLeaf. For a MarkedTarget -- a
  /// [[ref_to_uninit]] variable, parameter, or role parameter -- a directly
  /// named local or by-value parameter with tracked members, as a whole or a
  /// subobject enclosing some of them (flowTrackedMemberRange), is tracked
  /// and judged by its members' state. For a MarkedTarget or a Destroy, the
  /// current object as a whole (\c this, \c *this) in an instance member
  /// function other than a constructor is tracked: the pass tracks its
  /// lifetime when the body binds it so (P4222R2 §4.6). A [[ref_to_uninit]]
  /// pointer member of the current object (markedPointerMembers) is tracked
  /// for every use, as a marked local pointer is. For a ValueCopy -- the
  /// source of an unmarked value copy (isValueCopySource) -- a subobject of
  /// such a local enclosing tracked members is tracked and read by its
  /// members' state; in a constructor body, so are the current object's
  /// subobjects, for a MarkedTarget too.
  enum class FlowLeafUse { Plain, MarkedTarget, Destroy, ValueCopy };

  /// True if the leaf \p Leaf of a binding source names storage the
  /// std::init CFG pass flow-tracks -- an [[uninit]] local or parameter (or
  /// a subobject of one), a marked local pointer's or reference's referent
  /// (or a subobject of it), or a tracked [[uninit]] member (a scalar, or a
  /// class or array object tracked whole) of a directly named local or of the
  /// current object -- while a function body
  /// is being parsed (\p CurContext inside one). \p AsPointerValue says the
  /// leaf is used as a pointer value (a pointer binding, or a read-only alias
  /// binding of a pointer) rather than as a glvalue. Some leaves are
  /// tracked only for some \p Use (FlowLeafUse). The parse-time checks leave
  /// such a source to the pass; a source with no tracked leaf, or one outside
  /// any function body, is judged at parse time without flow state.
  bool isFlowTrackedLeaf(const ShapeLeaf &Leaf, bool AsPointerValue,
                         const DeclContext *CurContext,
                         FlowLeafUse Use = FlowLeafUse::Plain) const;
  /// True if the glvalue \p E of shape \p Shape, the object of class \p RD
  /// or a subobject of it enclosing tracked members
  /// (flowTrackedMemberRange), is tracked whole for \p Use (FlowLeafUse).
  bool isFlowTrackedSubobject(const CXXRecordDecl *RD, const Expr *E,
                              const FlowLeafShape &Shape,
                              FlowLeafUse Use) const;
  /// True if any leaf of \p Src, bound as \p T for \p Use, is flow-tracked
  /// in the current context (isFlowTrackedLeaf); the tracked leaves are
  /// appended to \p Leaves when given. With \p AsRange, \p Src is a range a
  /// known callee's role parameter takes by reference
  /// (HandleLeaf::Mode::Range).
  bool hasFlowTrackedLeaf(const Expr *Src, QualType T,
                          SmallVectorImpl<const Expr *> *Leaves = nullptr,
                          FlowLeafUse Use = FlowLeafUse::Plain,
                          bool AsRange = false) const;
  /// True if the leaf \p L of a handle source names a handle whose referent
  /// the flow analysis tracks (isFlowTrackedHandleVar) while a function body
  /// is being parsed (\p CurContext inside one).
  bool isFlowTrackedHandleLeaf(const HandleLeaf &L,
                               const DeclContext *CurContext) const;
  /// True if any leaf of the glvalue \p G is flow-tracked in the current
  /// context: the read-through and subobject-write checks leave such an
  /// access to the CFG pass. The tracked leaves are appended to \p Leaves
  /// when given.
  bool hasFlowTrackedGlvalueLeaf(
      const Expr *G, SmallVectorImpl<const Expr *> *Leaves = nullptr) const;
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
  /// construction, and \p ValueCopy is judgeInitProfileBinding's.
  InitSourceState classifyInitBindingLeaf(const ShapeLeaf &Leaf, QualType T,
                                          const Decl *Body,
                                          bool ValueCopy = false) const;
  /// The access a leaf is classified for: a value read (the top-level
  /// [[uninit]] marker is the flow-based read pass's) or a scalar store (the
  /// whole named entity's store is its initialization; storage reached
  /// through [[ref_to_uninit]] is trusted at the top level only).
  enum class InitAccessKind { Read, Write };
  /// Classify the leaf \p Leaf of a read or store glvalue by its form alone
  /// with the recognizers' access preset for \p Kind, for the CFG pass's
  /// judgment of an access with tracked leaves; \p Body as for
  /// classifyInitBindingLeaf.
  InitSourceState classifyInitAccessLeaf(const ShapeLeaf &Leaf,
                                         InitAccessKind Kind,
                                         const Decl *Body) const;
  /// Classify the leaf \p Leaf of a handle source (forEachHandleSourceLeaf)
  /// by its form alone, taking the arms its \c Arms list, for the CFG pass's
  /// judgment of a handle binding with tracked leaves; \p Body as for
  /// classifyInitBindingLeaf.
  InitSourceState classifyInitHandleLeaf(const HandleLeaf &Leaf,
                                         const Decl *Body) const;

  /// True if \p P is the source parameter of a copy or move constructor or
  /// of a copy or move assignment operator (after an explicit object
  /// parameter), an operand of a defaulted comparison (isDefaultedComparison),
  /// the source of std::bit_cast, memcpy, or memmove, or an argument of a
  /// Construct row of the constructed class type: an unmarked one copies the
  /// bound object's value whole, a *value copy*, which reads every subobject
  /// (on every path, P4222R2 §1.3) and is trusted below an [[uninit]] data
  /// member where a read of it would be.
  static bool isValueCopySource(const ParmVarDecl *P);
  /// True if \p FD is a defaulted comparison operator, which reads each of
  /// its operands -- its object argument included -- as a value copy.
  static bool isDefaultedComparison(const FunctionDecl *FD);

  /// The [[ref_to_uninit]] marking of the pointer or handle object an
  /// assignment target names: true/false for a directly named marked/unmarked
  /// declaration, std::nullopt when the marking is unknown -- a reference to
  /// a pointer or handle aliases an object whose marking it cannot carry, any
  /// other lvalue (*pp, arr[i]) names no declaration at all, and a
  /// conditional's arms may disagree -- where neither direction of the binding
  /// check is sound (P4222R2 §4.3). Peels transparent casts and walks the
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
  /// storage; a comma's right operand, a single-element braced list's
  /// element, a GNU statement expression's result, a pointer compound
  /// literal's initializer, an opaque value's source, and an identity call's
  /// argument (isIdentityCall), which yield that operand's lvalue or value;
  /// and the arm of a conditional listed in \p Arms. Shared with the CFG pass
  /// in AnalysisBasedWarnings.cpp, so recognition and flow tracking see through
  /// exactly the same forms.
  static const Expr *ignoreTransparentCasts(const Expr *E,
                                            ArrayRef<const Expr *> Arms = {});

  /// The operand of the address-of step \p E -- the operand of a built-in
  /// \c &, or the argument of std::addressof, std::__addressof, or
  /// __builtin_addressof -- or null.
  static const Expr *addressOfOperand(const Expr *E);

  /// The \c .* or \c ->* access through a pointer to data member that \p E
  /// is, or null: a member step the checks cannot name (P4222R2 §5.4 treats
  /// it like any member access). A pointer to member function is excluded.
  static const BinaryOperator *asDataMemberPointerAccess(const Expr *E);

  /// True if \p CE calls one of the standard library functions that return
  /// a reference to their argument (std::move, std::forward,
  /// std::forward_like, std::move_if_noexcept, std::as_const), recognized by
  /// builtin ID.
  static bool isIdentityCall(const CallExpr *CE);

  /// True if \p BuiltinID names a function whose call the std::init checks
  /// see through -- an identity function (isIdentityCall), or std::addressof
  /// and std::__addressof (addressOfOperand): such a call binds nothing, and
  /// its argument is judged where the result is used.
  static bool isShapeTransparentBuiltin(unsigned BuiltinID);

  /// The construct a pointer, reference, or handle binding belongs to,
  /// selecting which declaration can carry [[ref_to_uninit]] and which
  /// diagnostic wording applies (see checkInitProfileBinding).
  enum class InitBindingKind : unsigned {
    /// A variable's initializer, an init-capture included; the target is the
    /// VarDecl.
    Variable,
    /// A data member's initializer -- NSDMI, mem-initializer, or an
    /// aggregate's field element; the target is the FieldDecl. A base
    /// subobject's, which cannot carry the marker, has none.
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

  /// True if an unmarked target of type \p T bound by a \p Kind binding also
  /// binds the handle \p T refers to, judged as an unmarked handle binding
  /// (P4222R2 §4.6; a reference to a class cannot carry the handle's
  /// marking): a reference to a handle (isHandleType), except the implicit
  /// object parameter, a parameter of a comparison, of a + or - operator, or
  /// of std::advance (which compare or position handles), the source of a
  /// copy or move (isCopySource; judged where the copy lands), and an
  /// implicit local (range-for's __range). \p Target is the bound
  /// declaration, if any.
  static bool bindsHandleByReference(InitBindingKind Kind, QualType T,
                                     const ValueDecl *Target);

  /// What ends a lifetime, as the select of the double_destroy and
  /// destroy_uninit diagnostics.
  enum class InitDestroyer : unsigned {
    /// A [[now_uninit]] function or a std lifecycle row (std::destroy_at).
    Function,
    /// An explicit destructor call, or the one a delete-expression runs.
    DestructorCall,
    /// The destructor that ends an [[uninit]] local's scope.
    ScopeExit
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
  /// SFINAE context, code inRebuiltOrSynthesizedCode, an implicit member
  /// initialization, and an entity kind whose binding can carry no marker and
  /// names no construct of its own are skipped.
  void checkInitProfileBinding(const InitializedEntity &Entity,
                               const InitializationKind &Kind,
                               const Expr *Init);

  /// std::init / uninit_read (paper §4.5): diagnose a read *through* a
  /// [[ref_to_uninit]] pointer or reference, whose result is itself
  /// uninitialized. Called from Sema::DefaultLvalueConversion at the single
  /// lvalue-to-rvalue chokepoint, with \p Glvalue the operand being loaded
  /// and \p ValueType its value type; and from the compound-assignment
  /// (Sema::CheckAssignmentOperands) and increment/decrement
  /// (Sema::CreateBuiltinUnaryOp) operator sites and __builtin_bit_cast
  /// (CastOperation::CheckBuiltinBitCast), whose reads build no
  /// lvalue-to-rvalue node (the shift-compounds are excluded there because
  /// their LHS promotion already funnels through the chokepoint). Reuses the
  /// ref_to_uninit recognizer with its read access preset, so a direct read
  /// of a named [[uninit]] object is left to the flow-based uninit_read
  /// pass; a read with a flow-tracked leaf is the CFG pass's
  /// (ProfilesFrameworkInternals.rst, "Flow-Tracked Storage"). A std::byte
  /// read is exempt (P4222R2 §4.6). Template deferral follows the
  /// expression-check policy (ProfilesFrameworkInternals.rst, "Check Sites").
  /// A non-null \p AfterFlowPass judges, by form, a read the flow pass over
  /// that body left unjudged (judgeDeferredInitSiteByForm).
  void checkInitProfileReadThrough(SourceLocation Loc, const Expr *Glvalue,
                                   QualType ValueType,
                                   const Decl *AfterFlowPass = nullptr);

  /// std::init / uninit_write (paper §5.4-§5.6): diagnose a scalar store to a
  /// proper subobject of a named [[uninit]] entity -- delayed piecemeal
  /// initialization, which only whole-object construct_at could make good.
  /// A step of checkInitProfileAssignmentOperands and of
  /// checkInitProfileIncDec, with \p LHS the store target. Reuses the
  /// recognizer with its write access preset: a store to the whole named
  /// entity is its initialization (paper §4.5), and storage reached through
  /// [[ref_to_uninit]] is trusted (the deferred construct_at slice), so only
  /// a below-top-level [[uninit]] marker fires; a store with a flow-tracked
  /// leaf is the CFG pass's (ProfilesFrameworkInternals.rst, "Flow-Tracked
  /// Storage"). A std::byte store is exempt (P4222R2 §4.6). Template
  /// deferral follows the expression-check policy
  /// (ProfilesFrameworkInternals.rst, "Check Sites"). \p AfterFlowPass is
  /// checkInitProfileReadThrough's.
  void checkInitProfileSubobjectWrite(SourceLocation Loc, const Expr *LHS,
                                      const Decl *AfterFlowPass = nullptr);

  /// A std::init check the parse-time funnels left to the flow engine: what
  /// the form judgment needs, and the flow-tracked leaves that caused the
  /// deferral. Verification only -- the engine derives its sites from the
  /// CFG, and every recorded site must have been judged there
  /// (ProfilesFrameworkInternals.rst, "Flow-Tracked Storage (std::init)").
  struct DeferredInitSite {
    enum class Kind { Binding, Destroy, ReadThrough, SubobjectWrite };
    Kind K;
    /// Binding: the construct's kind.
    InitBindingKind BindingKind;
    SourceLocation Loc;
    /// Binding: the target's marking. Destroy: whether destroy_uninit
    /// applies (the callee is no reinitializer and the parameter is
    /// unmarked).
    bool Flag;
    /// Binding and Destroy: the bound type. ReadThrough: the value type.
    QualType T;
    /// The binding source, the read glvalue, or the store target.
    const Expr *Src;
    /// Binding: the entity a kind-specific wording names.
    const NamedDecl *Subject;
    SmallVector<const Expr *, 2> TrackedLeaves;
    /// Destroy: the destroyer, as the select of the destroy diagnostics.
    InitDestroyer Destroyer = InitDestroyer::Function;
    /// Binding: the source is copied by value (isValueCopySource).
    bool ValueCopy = false;
    /// Binding: the source is a range a role parameter takes by reference
    /// (judgeInitProfileBinding's \p AsRange).
    bool AsRange = false;
  };

  /// The body whose flow pass judges a check deferred in \p DC: the
  /// function, method, or block \p DC is, looking through captured regions
  /// (the enclosing function's CFG contains an OpenMP region's statements, not
  /// any other's) -- the check's analysis owner -- or null when \p DC is no
  /// body (a local class).
  static const Decl *flowAnalysisOwner(const DeclContext *DC);

  /// Hand the sites recorded for the analysis owner \p Body to its flow
  /// pass, erasing them.
  SmallVector<DeferredInitSite, 4> takeDeferredInitSites(const Decl *Body);

  /// Drop the sites recorded for \p Body; called once its analysis ran or
  /// was skipped, and before it for a body with an unrecoverable error.
  void discardDeferredInitSites(const Decl *Body);

  /// Judge \p Site, which the flow pass over \p Body left unjudged, by form
  /// alone, as the parse-time funnel would have without the deferral.
  void judgeDeferredInitSiteByForm(const DeferredInitSite &Site,
                                   const Decl *Body);

  /// The ObjectArgument derive step of checkInitProfileBinding: a member call
  /// binds its implicit object parameter, which cannot carry the marker, to
  /// \p Object. Called from \c
  /// Sema::PerformImplicitObjectArgumentInitialization, the funnel every
  /// member-call flavor's object argument converts through
  /// -- dot and arrow calls, member operators, functor operator(),
  /// operator->, and conversion operators. Explicit-object member functions
  /// initialize their object as an ordinary parameter and are checked there;
  /// a destructor call runs the destroy rules instead
  /// (checkInitProfileDestructorCall), and a static call operator is skipped
  /// (no implicit object parameter, like a static member call).
  void checkInitProfileObjectArgument(const Expr *Object,
                                      const CXXMethodDecl *Method);

  /// std::init / double_destroy and destroy_uninit (P4222R2 §4.6): an
  /// explicit destructor call ends the lifetime of its object as
  /// std::destroy_at does, whose effect it is ([specialized.destroy]).
  /// \p Object is the pointer of an arrow call or the object glvalue of a dot
  /// call. Called for a destructor member call from
  /// checkInitProfileObjectArgument and for a pseudo-destructor call from
  /// Sema::BuildCallExpr.
  void checkInitProfileDestructorCall(const Expr *Object);

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

  /// If \p CE reseats a handle -- a one-parameter member operator= of a
  /// non-union class whose source is a class object, as an operator or
  /// called by name, or an owner's reset with a written argument -- set
  /// \p Obj to the handle, or to the pointer an arrow call reaches it
  /// through, \p Src to what it takes, and \p Loc to where the reseat is
  /// judged (the operator, or the argument), and return true.
  static bool getHandleReseat(const CallExpr *CE, const Expr *&Obj,
                              const Expr *&Src, SourceLocation &Loc);
  /// The [[ref_to_uninit]] marking of the handle a reseat's \p Obj
  /// (getHandleReseat) names: a directly named handle's own, and unmarked for
  /// a handle reached through a reference, a pointer, or an lvalue naming no
  /// declaration, none of which can carry the handle's marking
  /// (bindsHandleByReference); std::nullopt when a conditional's arms
  /// disagree.
  static std::optional<bool> resolveHandleReseatMarking(const Expr *Obj);
  /// The type of the handle a reseat's \p Obj (getHandleReseat) names.
  static QualType handleReseatType(const Expr *Obj);
  /// True if \p P is the source parameter of a member operator=, which binds
  /// where it is assigned (checkInitProfileHandleReseat).
  static bool isAssignmentSource(const ParmVarDecl *P);
  /// True if \p P is the source of a copy or move constructor or of an
  /// assignment (isAssignmentSource): the object it binds is copied or moved
  /// from, and no alias of it outlives the call.
  static bool isCopySource(const ParmVarDecl *P);
  /// True if \p FD is a standard library function: declared in namespace std
  /// or a namespace below it, a member included.
  static bool isStdLibraryFunction(const FunctionDecl *FD);
  /// True if \p FD is the standard library function \p Name, a member
  /// included (isStdLibraryFunction), or the call operator of the
  /// std::ranges object \p Name.
  static bool isStdCallable(const FunctionDecl *FD, StringRef Name);

  /// std::init / ref_to_uninit: a handle reseat (getHandleReseat) binds the
  /// handle to its source, judged against the handle's marking
  /// (resolveAssignTargetMarking) like a pointer assignment. Called with the
  /// call \p CE of \p FD from Sema::CreateOverloadedBinOp for an operator=,
  /// beside checkAssignmentLifetime, and from Sema::CheckFunctionCall.
  void checkInitProfileHandleReseat(const FunctionDecl *FD, const CallExpr *CE);

  /// What a known callee does to the storage bound to its role parameter
  /// (KnownCallees in SemaProfiles.cpp; see ProfilesFrameworkInternals.rst,
  /// "The std::init Implementation Map").
  enum class KnownParamRole : uint8_t {
    /// No role parameter.
    None,
    /// The storage must be uninitialized and keeps its state, as for a
    /// [[ref_to_uninit]] parameter (std::now_init, P4222R2 §4.4).
    AssertUninit,
    /// The storage must be uninitialized and is initialized by the call, as
    /// for a [[ref_to_uninit]] parameter of a [[now_init]] function
    /// (std::construct_at, the std::uninitialized_* output range).
    Construct,
    /// The storage is initialized by the call whatever its prior state: an
    /// initializing parameter (memset's destination, va_start's va_list).
    InitializeAnyState,
    /// The call ends the lifetime of the storage, as a [[now_uninit]]
    /// function does (std::destroy_at, std::destroy's first iterator).
    Destroy,
    /// The call releases the storage, which may never have been constructed
    /// (free, realloc's pointer, replaceable global operator delete).
    Release,
  };

  /// What a direct callee does to the storage bound to its parameters: its
  /// lifetime attributes plus its known-callee row's role parameter. Each
  /// consumer reads only the bits its direction may rely on: binding
  /// *acceptance* (which never diagnoses) reads a name-only row like a
  /// trusted one, the CFG pass's Kill and initializing Write (a diagnostic's
  /// firing basis) read a trusted row only, and double_destroy keys on the
  /// destroy role only (a storage release leaves no object to destroy
  /// again).
  struct CalleeLifecycleRoles {
    /// [[now_init]]: the callee initializes the storage bound to its
    /// [[ref_to_uninit]] parameters (P4222R2 §6.2).
    bool InitializesRefToUninitParams = false;
    /// [[now_uninit]]: the callee ends the lifetime of the storage bound
    /// to its pointer/reference parameters (§4.4).
    bool DestroysPointerParams = false;
    /// The known-callee row's role parameter, if the row has one.
    std::optional<unsigned> KnownParam;
    /// What the row does to the storage bound to KnownParam.
    KnownParamRole KnownRole = KnownParamRole::None;
    /// The known-callee row's end iterator of the range at KnownParam, if
    /// the row has one: it bounds the range and is accepted in any state.
    std::optional<unsigned> EndParam;
    /// The KnownParam's Construct role is a default-initialization's
    /// (std::uninitialized_default_construct), which leaves the [[uninit]]
    /// members of the range's objects uninitialized.
    bool DefaultInitCredit = false;
    /// Whether the row matched by builtin ID, form, or namespace-std name
    /// rather than by the name-only fallback, which -fno-builtin /
    /// -ffreestanding leaves: the name says what the function is meant to
    /// be, but its semantics cannot be assumed.
    bool KnownTrusted = false;
    /// The callee constructs or resets a std-table handle from raw storage
    /// (see ProfilesFrameworkInternals.rst, "Handles (std::init)").
    bool HandleTransfers = false;
    /// The parameter whose storage the call may or may not destroy, an
    /// opaque callee's: the pointer of std::allocator_traits<A>::destroy when
    /// A declares its own destroy, which the traits call in place of
    /// destroy_at. The storage is possibly initialized afterwards.
    std::optional<unsigned> OpaqueParam;

    /// The row role of \p P, a parameter of the callee.
    KnownParamRole roleOf(const ParmVarDecl *P) const;
    /// True if the binding to \p P is judged against a [[ref_to_uninit]]
    /// target: \p P carries the marker, or its row role implies one.
    bool isMarkedParam(const ParmVarDecl *P) const;
    /// True if \p P is an initializing parameter, trusted or by name: its
    /// binding accepts storage in any live state.
    bool isInitializingParam(const ParmVarDecl *P) const;
    /// True if \p P is the row's end iterator (EndParam): its binding
    /// accepts storage in any state, and the call neither initializes nor
    /// destroys what it points to.
    bool isEndParam(const ParmVarDecl *P) const;
    /// True if the call ends the lifetime of the storage bound to \p P.
    bool destroysParam(const ParmVarDecl *P) const;
    /// True if the call releases the storage bound to \p P; a name-only
    /// match counts unless \p TrustedOnly.
    bool releasesParam(const ParmVarDecl *P, bool TrustedOnly) const;
    /// True if the call initializes the storage bound to \p P on a trusted
    /// basis: a marked parameter of a [[now_init]] callee, or a trusted
    /// row's Construct or InitializeAnyState role.
    bool initializesParam(const ParmVarDecl *P) const;
    /// True if \p P is a transfer parameter of a std-table handle's
    /// construction or of an owner's reset: its binding is judged where the
    /// handle lands, or at the reset, not here.
    bool transfersParam(const ParmVarDecl *P) const;
  };
  /// True if \p T is std::unique_ptr: an owner whose destruction, reset, or
  /// reassignment destroys the object it refers to.
  static bool isUniqueOwnerType(QualType T);
  /// The value type of the handle type \p T (or a reference to one) as an
  /// iterator or a range, unqualified, looked up in the class and then its
  /// bases: its \c element_type member type (std::unique_ptr<T[]>'s is
  /// \c T), else its \c value_type, else the non-reference type its
  /// \c operator* returns (a member template's included), else, for a range
  /// (std::ranges::subrange), the value type of the iterator its \c begin()
  /// returns; null when unknown.
  static QualType handleValueType(QualType T);
  /// The value type of the range a known-callee role parameter of type \p T
  /// begins: a pointer's pointee, the array a reference refers to, or a
  /// handle's handleValueType; null when unknown.
  static QualType rangeValueType(QualType T);
  /// Derive what \p FD does to the storage bound to its parameters: the
  /// lifetime attributes plus the known-callee row, looked up at the call
  /// so the library's own declarations need no annotation. Shared by the
  /// parse-time funnel and the CFG pass's argument and lifecycle arms.
  static CalleeLifecycleRoles getCalleeLifecycleRoles(const FunctionDecl *FD);

private:
  /// True if `this` in the current context denotes an object under
  /// construction -- the enclosing non-lambda function is a constructor, or
  /// the context is a class (a default member initializer) -- so the
  /// recognizers classify it Unknown rather than Initialized: its members may
  /// not be initialized yet. For a site judged after the flow pass over \p
  /// AfterFlowPass, whether that body's enclosing instance method is a
  /// constructor.
  bool thisIsUnderConstruction(const Decl *AfterFlowPass = nullptr) const;
  /// True if a parse-time read or store trusts data member markers
  /// (UninitAccessOpts::TrustUninitMembers): anywhere but a default member
  /// initializer, which no flow pass judges.
  bool trustsUninitMembers(const Decl *AfterFlowPass = nullptr) const;

  /// Record \p Site, deferred to the flow engine in the current context,
  /// under its analysis owner -- unless no flow pass judges it there: a
  /// dependent context (the instantiation records what it rebuilds), an
  /// evaluation context other than a potentially-evaluated one, a discarded
  /// statement, or an immediate function context. A recorded site the CFG
  /// does not contain is dropped by verifyDeferredInitSites.
  void recordDeferredInitSite(DeferredInitSite Site);

  /// The destroy rules (P4222R2 §4.4, §4.6) for \p Src, bound as \p T and
  /// destroyed by \p Destroyer: a source with a flow-tracked leaf
  /// (isFlowTrackedLeaf, with \p TargetMarked for a [[ref_to_uninit]]
  /// parameter) is the CFG pass's (a Destroy deferred site), any other is
  /// judged by form for destroy_uninit when \p DestroyUninitApplies
  /// (judgeDestroyUninitByForm); destroyed state exists only as flow state,
  /// so double_destroy is the pass's alone. \p D is
  /// judgeInitProfileBinding's.
  void checkInitProfileDestroy(SourceLocation Loc, QualType T, const Expr *Src,
                               const Decl *D, bool DestroyUninitApplies,
                               InitDestroyer Destroyer,
                               bool TargetMarked = false);

  /// The form half of checkInitProfileDestroy: diagnose destroy_uninit when
  /// \p Src, bound as \p T, classifies uninitialized. \p D and
  /// \p AfterFlowPass are judgeInitProfileBinding's.
  void judgeDestroyUninitByForm(SourceLocation Loc, QualType T, const Expr *Src,
                                const Decl *D, const Decl *AfterFlowPass,
                                InitDestroyer Destroyer);

  /// True while the std::init checks run on code that is not judged where it
  /// stands: a default argument or default member initializer rebuilt for one
  /// of its uses (in an aggregate initialization, or for an immediate
  /// invocation or source-location builtin inside it), which was judged where
  /// it was written; and a defaulted function's synthesized body, which has no
  /// tokens of its own -- only the binding of its source object is judged, at
  /// the defaulted function's use.
  bool inRebuiltOrSynthesizedCode() const;

  /// std::init / ref_to_uninit (P4222R2 §4.2-§4.3): judge binding \p Src as
  /// \p T to \p Target -- null for a construct with no declaration to carry
  /// the marker -- against the target's marking: a marked target must refer
  /// to uninitialized memory, an unmarked one must not. The one binding
  /// funnel: every host passes its \p Kind, and a kind whose construct cannot
  /// carry the marker is judged unmarked whatever \p Target is, and an
  /// unmarked reference to a handle also as the handle
  /// (bindsHandleByReference). No-op unless \p T is a non-dependent pointer,
  /// reference, or handle (isHandleType). \p D, when available,
  /// supplies the template-deferral rung: with it the rule fires on
  /// the instantiation only; without it an instantiation-dependent \p Src
  /// defers to the rebuild and a non-dependent one is judged on the pattern
  /// and again at each instantiation that rebuilds the construct
  /// (ProfilesFrameworkInternals.rst, "Check Sites"). A Parameter binding of
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
  /// wording names (the called method). A non-null \p AfterFlowPass judges,
  /// by form, a binding the flow pass over that body left unjudged
  /// (judgeDeferredInitSiteByForm). A \p ValueCopy (isValueCopySource) trusts
  /// [[uninit]] data members as a read does. An \p AsRange source is a range
  /// a known callee's role parameter takes by reference, classified as a
  /// handle range (HandleLeaf::Mode::Range) rather than as a glvalue.
  void judgeInitProfileBinding(InitBindingKind Kind, SourceLocation Loc,
                               bool TargetMarked, QualType T, const Expr *Src,
                               const Decl *D,
                               const NamedDecl *Subject = nullptr,
                               const Decl *AfterFlowPass = nullptr,
                               bool ValueCopy = false, bool AsRange = false);

public:
  /// Enumerate the leaves the store target, binding source, read, or lifecycle
  /// argument \p E can name, and hand each to \p F: peel what
  /// ignoreTransparentCasts peels, and distribute every conditional -- at the
  /// top, or below a member, element, indirection, or address-of step -- into
  /// one leaf per arm, so *(c ? p : q) has the leaves of c ? *p : *q. The one
  /// shape walk shared by the parse-time checks and the CFG pass's event
  /// extraction (AnalysisBasedWarnings.cpp), so a wrapped lvalue is judged
  /// exactly like its leaf form.
  static void
  forEachTargetLeaf(const Expr *E,
                    llvm::function_ref<void(const ShapeLeaf &Leaf)> F);

  /// One leaf of a handle source (forEachHandleSourceLeaf): the expression
  /// \c E the handle's state comes from, how \c E is classified (\c M), and
  /// the arms taken at the conditionals the walk passed (valid only while the
  /// walk that yields the leaf runs). See ProfilesFrameworkInternals.rst,
  /// "Handles (std::init)".
  struct HandleLeaf {
    enum class Mode {
      /// A pointer value passed into a std-table handle: the pointer
      /// recognizer's.
      PointerValue,
      /// An array or non-class lvalue passed by reference into a std-table
      /// handle: the glvalue recognizer's.
      Storage,
      /// A class object passed into a std-table constructor: its storage's
      /// state, then its own handle state.
      Range,
      /// A terminal of the walk, classified as a handle.
      Handle
    };
    const Expr *E;
    Mode M;
    /// The walk reached \c E through a call that refers to part of the handle
    /// (handleSelfReturnObject in SemaProfiles.cpp): the handle refers to
    /// part of \c E's memory.
    bool Partial = false;
    ArrayRef<const Expr *> Arms;
  };

  /// Enumerate the leaves the state of the handle \p Src comes from, and
  /// hand each to \p F: peel parens, full expressions, temporaries, the
  /// no-op, constructor, derived-to-base, and user-defined conversions, and
  /// the pass-through forms; follow a copy or move construction to its
  /// source, a std-table construction to its transfer arguments, a member
  /// call returning the handle itself to its object, and an implicit local
  /// or an unmarked local reference to its initializer; and distribute every
  /// conditional into one leaf per arm. The one walk shared by the
  /// parse-time recognizers and the CFG pass.
  static void
  forEachHandleSourceLeaf(const Expr *Src,
                          llvm::function_ref<void(const HandleLeaf &)> F);

  /// std::init / pointer_marker + union_marker (paper §4.1, §5.6): diagnose
  /// [[uninit]] placed on a pointer, a union variable, or a union member.
  /// \p D must already carry the UninitAttr (the marker location is taken
  /// from it). \p D reaches shouldEmitProfileViolation, so the rule defers on
  /// a templated pattern and is re-checked on the instantiated entity; an
  /// undeduced type defers to checkDeducedMarkers.
  void checkInitProfileMarkerPlacement(const Decl *D);

  /// True if \p T is a *handle* type: a non-dependent, non-reference class
  /// type other than a union, complete or not -- an object that refers to
  /// memory and may carry [[ref_to_uninit]] (a smart pointer, a span, an
  /// iterator; P4222R2 §4.6, §9.3).
  static bool isHandleType(QualType T);

  /// [[ref_to_uninit]] is only meaningful on a pointer or reference to an
  /// object, or on a handle (isHandleType) -- for a function, its return
  /// type. Returns true when \p D is a structured binding
  /// (err_ref_to_uninit_attr_structured_binding) or its type is invalid for
  /// the marker (err_ref_to_uninit_attr_invalid_type), diagnosing at
  /// \p AttrLoc unless \p Diagnose is false. A dependent type returns false:
  /// validation defers to the instantiation re-check in
  /// Sema::InstantiateAttrs, which drops the marker when the substituted type
  /// is invalid -- silently in a SFINAE context (\p Diagnose false there), so
  /// the marker can never affect overload resolution; a dropped marker is
  /// inert. An undeduced type returns false too: validation defers to
  /// checkDeducedMarkers. Not profile policy -- fires regardless of
  /// -fprofiles, like the parse-time handler it serves.
  bool diagnoseInvalidRefToUninitMarker(const Decl *D, SourceLocation AttrLoc,
                                        bool Diagnose = true);

  /// [[uninit]] is meaningless on a reference (it must bind when declared).
  /// Returns true when \p D's type is a reference, diagnosing
  /// err_uninit_attr_invalid_subject at \p AttrLoc unless \p Diagnose is
  /// false. A dependent type returns false: validation defers to the
  /// instantiation re-check in Sema::InstantiateAttrs, which drops the marker
  /// when the substituted type is a reference (silently in a SFINAE context).
  /// An undeduced type returns false too: validation defers to
  /// checkDeducedMarkers. The parameter / structured-binding rejections stay
  /// in the parse-time handler -- they do not depend on the type. Not profile
  /// policy -- fires regardless of -fprofiles.
  bool diagnoseInvalidUninitMarker(const Decl *D, SourceLocation AttrLoc,
                                   bool Diagnose = true);

  /// Validate the [[ref_to_uninit]] and [[uninit]] markers on \p D -- a
  /// variable whose type contains a deduced placeholder, or a function whose
  /// return type was deduced -- against the deduced type: an invalid one is
  /// dropped, after a diagnostic outside a SFINAE context, and a variable's
  /// kept [[uninit]] runs checkInitProfileMarkerPlacement. Called from
  /// Sema::CheckAttributesOnDeducedType once a variable's initializer has
  /// fixed its type, and from Sema::ActOnFinishFunctionBody once a function's
  /// body has fixed its return type. checkNowInitVacuity counts parameters
  /// only, so a dropped return marker never affects it.
  void checkDeducedMarkers(Decl *D);

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

private:
  /// Per profile, whether a rule diagnostic of it is enabled by a
  /// command-line option; see isProfileLive.
  llvm::StringMap<bool> RulesEnabledByOption;

  /// Per analysis owner, the std::init checks deferred to its flow pass
  /// (recordDeferredInitSite).
  llvm::DenseMap<const Decl *, SmallVector<DeferredInitSite, 4>>
      DeferredInitSites;

  /// Per complete class, its flow-tracked member table (flowTrackedMembers).
  mutable llvm::DenseMap<const CXXRecordDecl *,
                         std::unique_ptr<SmallVector<FlowTrackedMember, 4>>>
      FlowTrackedMemberTables;
  /// Per complete class, its marked pointer members (markedPointerMembers).
  mutable llvm::DenseMap<const CXXRecordDecl *,
                         std::unique_ptr<SmallVector<MemberPath, 2>>>
      MarkedPointerMemberTables;
};

} // namespace clang

#endif // LLVM_CLANG_SEMA_SEMAPROFILES_H
