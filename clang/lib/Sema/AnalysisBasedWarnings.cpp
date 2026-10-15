//=== AnalysisBasedWarnings.cpp - Sema warnings based on libAnalysis ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines analysis_warnings::[Policy,Executor].
// Together they are used by Sema to issue warnings based on inexpensive
// static analysis algorithms in libAnalysis.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/AnalysisBasedWarnings.h"
#include "SemaLifetimeSafety.h"
#include "TypeLocBuilder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/Type.h"
#include "clang/Analysis/Analyses/CFGReachabilityAnalysis.h"
#include "clang/Analysis/Analyses/CalledOnceCheck.h"
#include "clang/Analysis/Analyses/Consumed.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeSafety.h"
#include "clang/Analysis/Analyses/ReachableCode.h"
#include "clang/Analysis/Analyses/ThreadSafety.h"
#include "clang/Analysis/Analyses/UninitializedValues.h"
#include "clang/Analysis/Analyses/UnsafeBufferUsage.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/CallGraph.h"
#include "clang/Analysis/FlowSensitive/DataflowWorklist.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/SemaProfiles.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <array>
#include <deque>
#include <iterator>
#include <optional>

using namespace clang;

//===----------------------------------------------------------------------===//
// Unreachable code analysis.
//===----------------------------------------------------------------------===//

namespace {
  class UnreachableCodeHandler : public reachable_code::Callback {
    Sema &S;
    SourceRange PreviousSilenceableCondVal;

  public:
    UnreachableCodeHandler(Sema &s) : S(s) {}

    void HandleUnreachable(reachable_code::UnreachableKind UK, SourceLocation L,
                           SourceRange SilenceableCondVal, SourceRange R1,
                           SourceRange R2, bool HasFallThroughAttr) override {
      // If the diagnosed code is `[[fallthrough]];` and
      // `-Wunreachable-code-fallthrough` is  enabled, suppress `code will never
      // be executed` warning to avoid generating diagnostic twice
      if (HasFallThroughAttr &&
          !S.getDiagnostics().isIgnored(diag::warn_unreachable_fallthrough_attr,
                                        SourceLocation()))
        return;

      // Avoid reporting multiple unreachable code diagnostics that are
      // triggered by the same conditional value.
      if (PreviousSilenceableCondVal.isValid() &&
          SilenceableCondVal.isValid() &&
          PreviousSilenceableCondVal == SilenceableCondVal)
        return;
      PreviousSilenceableCondVal = SilenceableCondVal;

      unsigned diag = diag::warn_unreachable;
      switch (UK) {
        case reachable_code::UK_Break:
          diag = diag::warn_unreachable_break;
          break;
        case reachable_code::UK_Return:
          diag = diag::warn_unreachable_return;
          break;
        case reachable_code::UK_Loop_Increment:
          diag = diag::warn_unreachable_loop_increment;
          break;
        case reachable_code::UK_Other:
          break;
      }

      S.Diag(L, diag) << R1 << R2;

      SourceLocation Open = SilenceableCondVal.getBegin();
      if (Open.isValid()) {
        SourceLocation Close = SilenceableCondVal.getEnd();
        Close = S.getLocForEndOfToken(Close);
        if (Close.isValid()) {
          S.Diag(Open, diag::note_unreachable_silence)
            << FixItHint::CreateInsertion(Open, "/* DISABLES CODE */ (")
            << FixItHint::CreateInsertion(Close, ")");
        }
      }
    }
  };
} // anonymous namespace

/// CheckUnreachable - Check for unreachable code.
static void CheckUnreachable(Sema &S, AnalysisDeclContext &AC) {
  // As a heuristic prune all diagnostics not in the main file.  Currently
  // the majority of warnings in headers are false positives.  These
  // are largely caused by configuration state, e.g. preprocessor
  // defined code, etc.
  //
  // Note that this is also a performance optimization.  Analyzing
  // headers many times can be expensive.
  if (!S.getSourceManager().isInMainFile(AC.getDecl()->getBeginLoc()))
    return;

  UnreachableCodeHandler UC(S);
  reachable_code::FindUnreachableCode(AC, S.getPreprocessor(), UC);
}

namespace {
/// Warn on logical operator errors in CFGBuilder
class LogicalErrorHandler : public CFGCallback {
  Sema &S;

public:
  LogicalErrorHandler(Sema &S) : S(S) {}

  static bool HasMacroID(const Expr *E) {
    if (E->getExprLoc().isMacroID())
      return true;

    // Recurse to children.
    for (const Stmt *SubStmt : E->children())
      if (const Expr *SubExpr = dyn_cast_or_null<Expr>(SubStmt))
        if (HasMacroID(SubExpr))
          return true;

    return false;
  }

  void logicAlwaysTrue(const BinaryOperator *B, bool isAlwaysTrue) override {
    if (HasMacroID(B))
      return;

    unsigned DiagID = isAlwaysTrue
                          ? diag::warn_tautological_negation_or_compare
                          : diag::warn_tautological_negation_and_compare;
    SourceRange DiagRange = B->getSourceRange();
    S.Diag(B->getExprLoc(), DiagID) << DiagRange;
  }

  void compareAlwaysTrue(const BinaryOperator *B,
                         bool isAlwaysTrueOrFalse) override {
    if (HasMacroID(B))
      return;

    SourceRange DiagRange = B->getSourceRange();
    S.Diag(B->getExprLoc(), diag::warn_tautological_overlap_comparison)
        << DiagRange << isAlwaysTrueOrFalse;
  }

  void compareBitwiseEquality(const BinaryOperator *B,
                              bool isAlwaysTrue) override {
    if (HasMacroID(B))
      return;

    SourceRange DiagRange = B->getSourceRange();
    S.Diag(B->getExprLoc(), diag::warn_comparison_bitwise_always)
        << DiagRange << isAlwaysTrue;
  }

  void compareBitwiseOr(const BinaryOperator *B) override {
    if (HasMacroID(B))
      return;

    SourceRange DiagRange = B->getSourceRange();
    S.Diag(B->getExprLoc(), diag::warn_comparison_bitwise_or) << DiagRange;
  }

  static bool hasActiveDiagnostics(DiagnosticsEngine &Diags,
                                   SourceLocation Loc) {
    return !Diags.isIgnored(diag::warn_tautological_overlap_comparison, Loc) ||
           !Diags.isIgnored(diag::warn_comparison_bitwise_or, Loc) ||
           !Diags.isIgnored(diag::warn_tautological_negation_and_compare, Loc);
  }
};
} // anonymous namespace

//===----------------------------------------------------------------------===//
// Check for infinite self-recursion in functions
//===----------------------------------------------------------------------===//

// Returns true if the function is called anywhere within the CFGBlock.
// For member functions, the additional condition of being call from the
// this pointer is required.
static bool hasRecursiveCallInPath(const FunctionDecl *FD, CFGBlock &Block) {
  // Process all the Stmt's in this block to find any calls to FD.
  for (const auto &B : Block) {
    if (B.getKind() != CFGElement::Statement)
      continue;

    const CallExpr *CE = dyn_cast<CallExpr>(B.getAs<CFGStmt>()->getStmt());
    if (!CE || !CE->getCalleeDecl() ||
        CE->getCalleeDecl()->getCanonicalDecl() != FD)
      continue;

    // Skip function calls which are qualified with a templated class.
    if (const DeclRefExpr *DRE =
            dyn_cast<DeclRefExpr>(CE->getCallee()->IgnoreParenImpCasts()))
      if (NestedNameSpecifier NNS = DRE->getQualifier();
          NNS.getKind() == NestedNameSpecifier::Kind::Type)
        if (isa_and_nonnull<TemplateSpecializationType>(NNS.getAsType()))
          continue;

    const CXXMemberCallExpr *MCE = dyn_cast<CXXMemberCallExpr>(CE);
    if (!MCE || isa<CXXThisExpr>(MCE->getImplicitObjectArgument()) ||
        !MCE->getMethodDecl()->isVirtual())
      return true;
  }
  return false;
}

// Returns true if every path from the entry block passes through a call to FD.
static bool checkForRecursiveFunctionCall(const FunctionDecl *FD, CFG *cfg) {
  llvm::SmallPtrSet<CFGBlock *, 16> Visited;
  llvm::SmallVector<CFGBlock *, 16> WorkList;
  // Keep track of whether we found at least one recursive path.
  bool foundRecursion = false;

  const unsigned ExitID = cfg->getExit().getBlockID();

  // Seed the work list with the entry block.
  WorkList.push_back(&cfg->getEntry());

  while (!WorkList.empty()) {
    CFGBlock *Block = WorkList.pop_back_val();

    for (auto I = Block->succ_begin(), E = Block->succ_end(); I != E; ++I) {
      if (CFGBlock *SuccBlock = *I) {
        if (!Visited.insert(SuccBlock).second)
          continue;

        // Found a path to the exit node without a recursive call.
        if (ExitID == SuccBlock->getBlockID())
          return false;

        // If the successor block contains a recursive call, end analysis there.
        if (hasRecursiveCallInPath(FD, *SuccBlock)) {
          foundRecursion = true;
          continue;
        }

        WorkList.push_back(SuccBlock);
      }
    }
  }
  return foundRecursion;
}

static void checkRecursiveFunction(Sema &S, const FunctionDecl *FD,
                                   const Stmt *Body, AnalysisDeclContext &AC) {
  FD = FD->getCanonicalDecl();

  // Only run on non-templated functions and non-templated members of
  // templated classes.
  if (FD->getTemplatedKind() != FunctionDecl::TK_NonTemplate &&
      FD->getTemplatedKind() != FunctionDecl::TK_MemberSpecialization)
    return;

  CFG *cfg = AC.getCFG();
  if (!cfg) return;

  // If the exit block is unreachable, skip processing the function.
  if (cfg->getExit().pred_empty())
    return;

  // Emit diagnostic if a recursive function call is detected for all paths.
  if (checkForRecursiveFunctionCall(FD, cfg))
    S.Diag(Body->getBeginLoc(), diag::warn_infinite_recursive_function);
}

//===----------------------------------------------------------------------===//
// Check for throw in a non-throwing function.
//===----------------------------------------------------------------------===//

/// Determine whether an exception thrown by E, unwinding from ThrowBlock,
/// can reach ExitBlock.
static bool throwEscapes(Sema &S, const CXXThrowExpr *E, CFGBlock &ThrowBlock,
                         CFG *Body) {
  SmallVector<CFGBlock *, 16> Stack;
  llvm::BitVector Queued(Body->getNumBlockIDs());

  Stack.push_back(&ThrowBlock);
  Queued[ThrowBlock.getBlockID()] = true;

  while (!Stack.empty()) {
    CFGBlock &UnwindBlock = *Stack.pop_back_val();

    for (auto &Succ : UnwindBlock.succs()) {
      if (!Succ.isReachable() || Queued[Succ->getBlockID()])
        continue;

      if (Succ->getBlockID() == Body->getExit().getBlockID())
        return true;

      if (auto *Catch =
              dyn_cast_or_null<CXXCatchStmt>(Succ->getLabel())) {
        QualType Caught = Catch->getCaughtType();
        if (Caught.isNull() || // catch (...) catches everything
            !E->getSubExpr() || // throw; is considered cuaght by any handler
            S.handlerCanCatch(Caught, E->getSubExpr()->getType()))
          // Exception doesn't escape via this path.
          break;
      } else {
        Stack.push_back(Succ);
        Queued[Succ->getBlockID()] = true;
      }
    }
  }

  return false;
}

static void visitReachableThrows(
    CFG *BodyCFG,
    llvm::function_ref<void(const CXXThrowExpr *, CFGBlock &)> Visit) {
  llvm::BitVector Reachable(BodyCFG->getNumBlockIDs());
  clang::reachable_code::ScanReachableFromBlock(&BodyCFG->getEntry(), Reachable);
  for (CFGBlock *B : *BodyCFG) {
    if (!Reachable[B->getBlockID()])
      continue;
    for (CFGElement &E : *B) {
      std::optional<CFGStmt> S = E.getAs<CFGStmt>();
      if (!S)
        continue;
      if (auto *Throw = dyn_cast<CXXThrowExpr>(S->getStmt()))
        Visit(Throw, *B);
    }
  }
}

static void EmitDiagForCXXThrowInNonThrowingFunc(Sema &S, SourceLocation OpLoc,
                                                 const FunctionDecl *FD) {
  if (!S.getSourceManager().isInSystemHeader(OpLoc) &&
      FD->getTypeSourceInfo()) {
    S.Diag(OpLoc, diag::warn_throw_in_noexcept_func) << FD;
    if (S.getLangOpts().CPlusPlus11 &&
        (isa<CXXDestructorDecl>(FD) ||
         FD->getDeclName().getCXXOverloadedOperator() == OO_Delete ||
         FD->getDeclName().getCXXOverloadedOperator() == OO_Array_Delete)) {
      if (const auto *Ty = FD->getTypeSourceInfo()->getType()->
                                         getAs<FunctionProtoType>())
        S.Diag(FD->getLocation(), diag::note_throw_in_dtor)
            << !isa<CXXDestructorDecl>(FD) << !Ty->hasExceptionSpec()
            << FD->getExceptionSpecSourceRange();
    } else
      S.Diag(FD->getLocation(), diag::note_throw_in_function)
          << FD->getExceptionSpecSourceRange();
  }
}

static void checkThrowInNonThrowingFunc(Sema &S, const FunctionDecl *FD,
                                        AnalysisDeclContext &AC) {
  CFG *BodyCFG = AC.getCFG();
  if (!BodyCFG)
    return;
  if (BodyCFG->getExit().pred_empty())
    return;
  visitReachableThrows(BodyCFG, [&](const CXXThrowExpr *Throw, CFGBlock &Block) {
    if (throwEscapes(S, Throw, Block, BodyCFG))
      EmitDiagForCXXThrowInNonThrowingFunc(S, Throw->getThrowLoc(), FD);
  });
}

static bool isNoexcept(const FunctionDecl *FD) {
  const auto *FPT = FD->getType()->castAs<FunctionProtoType>();
  if (FPT->isNothrow() || FD->hasAttr<NoThrowAttr>())
    return true;
  return false;
}

/// Checks if the given expression is a reference to a function with
/// 'noreturn' attribute.
static bool isReferenceToNoReturn(const Expr *E) {
  if (auto *DRef = dyn_cast<DeclRefExpr>(E->IgnoreParenCasts()))
    if (auto *FD = dyn_cast<FunctionDecl>(DRef->getDecl()))
      return FD->isNoReturn();
  return false;
}

/// Checks if the given variable, which is assumed to be a function pointer, is
/// initialized with a function having 'noreturn' attribute.
static bool isInitializedWithNoReturn(const VarDecl *VD) {
  if (const Expr *Init = VD->getInit()) {
    if (auto *ListInit = dyn_cast<InitListExpr>(Init);
        ListInit && ListInit->getNumInits() > 0)
      Init = ListInit->getInit(0);
    return isReferenceToNoReturn(Init);
  }
  return false;
}

namespace {

/// Looks for statements, that can define value of the given variable.
struct TransferFunctions : public StmtVisitor<TransferFunctions> {
  const VarDecl *Var;
  std::optional<bool> AllValuesAreNoReturn;

  TransferFunctions(const VarDecl *VD) : Var(VD) {}

  void reset() { AllValuesAreNoReturn = std::nullopt; }

  void VisitDeclStmt(DeclStmt *DS) {
    for (auto *DI : DS->decls())
      if (auto *VD = dyn_cast<VarDecl>(DI))
        if (VarDecl *Def = VD->getDefinition())
          if (Def == Var)
            AllValuesAreNoReturn = isInitializedWithNoReturn(Def);
  }

  void VisitUnaryOperator(UnaryOperator *UO) {
    if (UO->getOpcode() == UO_AddrOf) {
      if (auto *DRef =
              dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenCasts()))
        if (DRef->getDecl() == Var)
          AllValuesAreNoReturn = false;
    }
  }

  void VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->getOpcode() == BO_Assign)
      if (auto *DRef = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenCasts()))
        if (DRef->getDecl() == Var)
          AllValuesAreNoReturn = isReferenceToNoReturn(BO->getRHS());
  }

  void VisitCallExpr(CallExpr *CE) {
    for (CallExpr::arg_iterator I = CE->arg_begin(), E = CE->arg_end(); I != E;
         ++I) {
      const Expr *Arg = *I;
      if (Arg->isGLValue() && !Arg->getType().isConstQualified())
        if (auto *DRef = dyn_cast<DeclRefExpr>(Arg->IgnoreParenCasts()))
          if (auto VD = dyn_cast<VarDecl>(DRef->getDecl()))
            if (VD->getDefinition() == Var)
              AllValuesAreNoReturn = false;
    }
  }
};
} // namespace

// Checks if all possible values of the given variable are functions with
// 'noreturn' attribute.
static bool areAllValuesNoReturn(const VarDecl *VD, const CFGBlock &VarBlk,
                                 AnalysisDeclContext &AC) {
  // The set of possible values of a constant variable is determined by
  // its initializer, unless it is a function parameter.
  if (!isa<ParmVarDecl>(VD) && VD->getType().isConstant(AC.getASTContext())) {
    if (const VarDecl *Def = VD->getDefinition())
      return isInitializedWithNoReturn(Def);
    return false;
  }

  // In multithreaded environment the value of a global variable may be changed
  // asynchronously.
  if (!VD->getDeclContext()->isFunctionOrMethod())
    return false;

  // Check the condition "all values are noreturn". It is satisfied if the
  // variable is set to "noreturn" value in the current block or all its
  // predecessors satisfies the condition.
  using MapTy = llvm::DenseMap<const CFGBlock *, std::optional<bool>>;
  using ValueTy = MapTy::value_type;
  MapTy BlocksToCheck;
  BlocksToCheck[&VarBlk] = std::nullopt;
  const auto BlockSatisfiesCondition = [](ValueTy Item) {
    return Item.getSecond().value_or(false);
  };

  TransferFunctions TF(VD);
  BackwardDataflowWorklist Worklist(*AC.getCFG(), AC);
  llvm::DenseSet<const CFGBlock *> Visited;
  Worklist.enqueueBlock(&VarBlk);
  while (const CFGBlock *B = Worklist.dequeue()) {
    if (Visited.contains(B))
      continue;
    Visited.insert(B);
    // First check the current block.
    for (CFGBlock::const_reverse_iterator ri = B->rbegin(), re = B->rend();
         ri != re; ++ri) {
      if (std::optional<CFGStmt> cs = ri->getAs<CFGStmt>()) {
        const Stmt *S = cs->getStmt();
        TF.reset();
        TF.Visit(const_cast<Stmt *>(S));
        if (TF.AllValuesAreNoReturn) {
          if (!TF.AllValuesAreNoReturn.value())
            return false;
          BlocksToCheck[B] = true;
          break;
        }
      }
    }

    // If all checked blocks satisfy the condition, the check is finished.
    if (llvm::all_of(BlocksToCheck, BlockSatisfiesCondition))
      return true;

    // If this block does not contain the variable definition, check
    // its predecessors.
    if (!BlocksToCheck[B]) {
      Worklist.enqueuePredecessors(B);
      BlocksToCheck.erase(B);
      for (const auto &PredBlk : B->preds())
        if (!BlocksToCheck.contains(PredBlk))
          BlocksToCheck[PredBlk] = std::nullopt;
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Check for missing return value.
//===----------------------------------------------------------------------===//

enum ControlFlowKind {
  UnknownFallThrough,
  NeverFallThrough,
  MaybeFallThrough,
  AlwaysFallThrough,
  NeverFallThroughOrReturn
};

/// CheckFallThrough - Check that we don't fall off the end of a
/// Statement that should return a value.
///
/// \returns AlwaysFallThrough iff we always fall off the end of the statement,
/// MaybeFallThrough iff we might or might not fall off the end,
/// NeverFallThroughOrReturn iff we never fall off the end of the statement or
/// return.  We assume NeverFallThrough iff we never fall off the end of the
/// statement but we may return.  We assume that functions not marked noreturn
/// will return.
static ControlFlowKind CheckFallThrough(AnalysisDeclContext &AC) {
  CFG *cfg = AC.getCFG();
  if (!cfg) return UnknownFallThrough;

  // The CFG leaves in dead things, and we don't want the dead code paths to
  // confuse us, so we mark all live things first.
  llvm::BitVector live(cfg->getNumBlockIDs());
  unsigned count =
      reachable_code::ScanReachableFromBlock(&cfg->getEntry(), live);

  bool AddEHEdges = AC.getAddEHEdges();
  if (!AddEHEdges && count != cfg->getNumBlockIDs())
    // When there are things remaining dead, and we didn't add EH edges
    // from CallExprs to the catch clauses, we have to go back and
    // mark them as live.
    for (const auto *B : *cfg) {
      if (!live[B->getBlockID()]) {
        if (B->preds().empty()) {
          const Stmt *Term = B->getTerminatorStmt();
          if (isa_and_nonnull<CXXTryStmt>(Term))
            // When not adding EH edges from calls, catch clauses
            // can otherwise seem dead.  Avoid noting them as dead.
            count += reachable_code::ScanReachableFromBlock(B, live);
          continue;
        }
      }
    }

  // Now we know what is live, we check the live precessors of the exit block
  // and look for fall through paths, being careful to ignore normal returns,
  // and exceptional paths.
  bool HasLiveReturn = false;
  bool HasFakeEdge = false;
  bool HasPlainEdge = false;
  bool HasAbnormalEdge = false;

  // Ignore default cases that aren't likely to be reachable because all
  // enums in a switch(X) have explicit case statements.
  CFGBlock::FilterOptions FO;
  FO.IgnoreDefaultsWithCoveredEnums = 1;

  for (CFGBlock::filtered_pred_iterator I =
           cfg->getExit().filtered_pred_start_end(FO);
       I.hasMore(); ++I) {
    const CFGBlock &B = **I;
    if (!live[B.getBlockID()])
      continue;

    // Skip blocks which contain an element marked as no-return. They don't
    // represent actually viable edges into the exit block, so mark them as
    // abnormal.
    if (B.hasNoReturnElement()) {
      HasAbnormalEdge = true;
      continue;
    }

    // Destructors can appear after the 'return' in the CFG.  This is
    // normal.  We need to look pass the destructors for the return
    // statement (if it exists).
    CFGBlock::const_reverse_iterator ri = B.rbegin(), re = B.rend();

    for ( ; ri != re ; ++ri)
      if (ri->getAs<CFGStmt>())
        break;

    // No more CFGElements in the block?
    if (ri == re) {
      const Stmt *Term = B.getTerminatorStmt();
      if (Term && (isa<CXXTryStmt>(Term) || isa<ObjCAtTryStmt>(Term))) {
        HasAbnormalEdge = true;
        continue;
      }
      // A labeled empty statement, or the entry block...
      HasPlainEdge = true;
      continue;
    }

    CFGStmt CS = ri->castAs<CFGStmt>();
    const Stmt *S = CS.getStmt();
    if (isa<ReturnStmt>(S) || isa<CoreturnStmt>(S)) {
      HasLiveReturn = true;
      continue;
    }
    if (isa<ObjCAtThrowStmt>(S)) {
      HasFakeEdge = true;
      continue;
    }
    if (isa<CXXThrowExpr>(S)) {
      HasFakeEdge = true;
      continue;
    }
    if (isa<MSAsmStmt>(S)) {
      // TODO: Verify this is correct.
      HasFakeEdge = true;
      HasLiveReturn = true;
      continue;
    }
    if (isa<CXXTryStmt>(S)) {
      HasAbnormalEdge = true;
      continue;
    }
    if (!llvm::is_contained(B.succs(), &cfg->getExit())) {
      HasAbnormalEdge = true;
      continue;
    }
    if (auto *Call = dyn_cast<CallExpr>(S)) {
      const Expr *Callee = Call->getCallee();
      if (Callee->getType()->isPointerType())
        if (auto *DeclRef =
                dyn_cast<DeclRefExpr>(Callee->IgnoreParenImpCasts()))
          if (auto *VD = dyn_cast<VarDecl>(DeclRef->getDecl()))
            if (areAllValuesNoReturn(VD, B, AC)) {
              HasAbnormalEdge = true;
              continue;
            }
    }

    HasPlainEdge = true;
  }
  if (!HasPlainEdge) {
    if (HasLiveReturn)
      return NeverFallThrough;
    return NeverFallThroughOrReturn;
  }
  if (HasAbnormalEdge || HasFakeEdge || HasLiveReturn)
    return MaybeFallThrough;
  // This says AlwaysFallThrough for calls to functions that are not marked
  // noreturn, that don't return.  If people would like this warning to be more
  // accurate, such functions should be marked as noreturn.
  return AlwaysFallThrough;
}

namespace {

struct CheckFallThroughDiagnostics {
  unsigned diag_FallThrough_HasNoReturn = 0;
  unsigned diag_FallThrough_ReturnsNonVoid = 0;
  unsigned diag_NeverFallThroughOrReturn = 0;
  unsigned FunKind; // TODO: use diag::FalloffFunctionKind
  SourceLocation FuncLoc;

  static CheckFallThroughDiagnostics MakeForFunction(Sema &S,
                                                     const Decl *Func) {
    CheckFallThroughDiagnostics D;
    D.FuncLoc = Func->getLocation();
    D.diag_FallThrough_HasNoReturn = diag::warn_noreturn_has_return_expr;
    D.diag_FallThrough_ReturnsNonVoid = diag::warn_falloff_nonvoid;

    // Don't suggest that virtual functions be marked "noreturn", since they
    // might be overridden by non-noreturn functions.
    bool isVirtualMethod = false;
    if (const CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(Func))
      isVirtualMethod = Method->isVirtual();

    // Don't suggest that template instantiations be marked "noreturn"
    bool isTemplateInstantiation = false;
    if (const FunctionDecl *Function = dyn_cast<FunctionDecl>(Func)) {
      isTemplateInstantiation = Function->isTemplateInstantiation();
      if (!S.getLangOpts().CPlusPlus && !S.getLangOpts().C99 &&
          Function->isMain()) {
        D.diag_FallThrough_ReturnsNonVoid = diag::ext_main_no_return;
      }
    }

    if (!isVirtualMethod && !isTemplateInstantiation)
      D.diag_NeverFallThroughOrReturn = diag::warn_suggest_noreturn_function;

    D.FunKind = diag::FalloffFunctionKind::Function;
    return D;
  }

  static CheckFallThroughDiagnostics MakeForCoroutine(const Decl *Func) {
    CheckFallThroughDiagnostics D;
    D.FuncLoc = Func->getLocation();
    D.diag_FallThrough_ReturnsNonVoid = diag::warn_falloff_nonvoid;
    D.FunKind = diag::FalloffFunctionKind::Coroutine;
    return D;
  }

  static CheckFallThroughDiagnostics MakeForBlock() {
    CheckFallThroughDiagnostics D;
    D.diag_FallThrough_HasNoReturn = diag::err_noreturn_has_return_expr;
    D.diag_FallThrough_ReturnsNonVoid = diag::err_falloff_nonvoid;
    D.FunKind = diag::FalloffFunctionKind::Block;
    return D;
  }

  static CheckFallThroughDiagnostics MakeForLambda() {
    CheckFallThroughDiagnostics D;
    D.diag_FallThrough_HasNoReturn = diag::err_noreturn_has_return_expr;
    D.diag_FallThrough_ReturnsNonVoid = diag::warn_falloff_nonvoid;
    D.FunKind = diag::FalloffFunctionKind::Lambda;
    return D;
  }

  bool checkDiagnostics(DiagnosticsEngine &D, bool ReturnsVoid,
                        bool HasNoReturn) const {
    if (FunKind == diag::FalloffFunctionKind::Function) {
      return (ReturnsVoid ||
              D.isIgnored(diag::warn_falloff_nonvoid, FuncLoc)) &&
             (!HasNoReturn ||
              D.isIgnored(diag::warn_noreturn_has_return_expr, FuncLoc)) &&
             (!ReturnsVoid ||
              D.isIgnored(diag::warn_suggest_noreturn_block, FuncLoc));
    }
    if (FunKind == diag::FalloffFunctionKind::Coroutine) {
      return (ReturnsVoid ||
              D.isIgnored(diag::warn_falloff_nonvoid, FuncLoc)) &&
             (!HasNoReturn);
    }
    // For blocks / lambdas.
    return ReturnsVoid && !HasNoReturn;
  }
};

} // anonymous namespace

/// CheckFallThroughForBody - Check that we don't fall off the end of a
/// function that should return a value.  Check that we don't fall off the end
/// of a noreturn function.  We assume that functions and blocks not marked
/// noreturn will return.
static void CheckFallThroughForBody(Sema &S, const Decl *D, const Stmt *Body,
                                    QualType BlockType,
                                    const CheckFallThroughDiagnostics &CD,
                                    AnalysisDeclContext &AC) {

  bool ReturnsVoid = false;
  bool HasNoReturn = false;

  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    if (const auto *CBody = dyn_cast<CoroutineBodyStmt>(Body))
      ReturnsVoid = CBody->getFallthroughHandler() != nullptr;
    else
      ReturnsVoid = FD->getReturnType()->isVoidType();
    HasNoReturn = FD->isNoReturn() || FD->hasAttr<InferredNoReturnAttr>();
  }
  else if (const auto *MD = dyn_cast<ObjCMethodDecl>(D)) {
    ReturnsVoid = MD->getReturnType()->isVoidType();
    HasNoReturn = MD->hasAttr<NoReturnAttr>();
  }
  else if (isa<BlockDecl>(D)) {
    if (const FunctionType *FT =
          BlockType->getPointeeType()->getAs<FunctionType>()) {
      if (FT->getReturnType()->isVoidType())
        ReturnsVoid = true;
      if (FT->getNoReturnAttr())
        HasNoReturn = true;
    }
  }

  DiagnosticsEngine &Diags = S.getDiagnostics();

  // Short circuit for compilation speed.
  if (CD.checkDiagnostics(Diags, ReturnsVoid, HasNoReturn))
      return;
  SourceLocation LBrace = Body->getBeginLoc(), RBrace = Body->getEndLoc();

  // cpu_dispatch functions permit empty function bodies for ICC compatibility.
  if (D->getAsFunction() && D->getAsFunction()->isCPUDispatchMultiVersion())
    return;

  // Either in a function body compound statement, or a function-try-block.
  switch (int FallThroughType = CheckFallThrough(AC)) {
  case UnknownFallThrough:
    break;

  case MaybeFallThrough:
  case AlwaysFallThrough:
    if (HasNoReturn) {
      if (CD.diag_FallThrough_HasNoReturn)
        S.Diag(RBrace, CD.diag_FallThrough_HasNoReturn) << CD.FunKind;
    } else if (!ReturnsVoid && CD.diag_FallThrough_ReturnsNonVoid) {
      // If the final statement is a call to an always-throwing function,
      // don't warn about the fall-through.
      if (D->getAsFunction()) {
        if (const auto *CS = dyn_cast<CompoundStmt>(Body);
            CS && !CS->body_empty()) {
          const Stmt *LastStmt = CS->body_back();
          // Unwrap ExprWithCleanups if necessary.
          if (const auto *EWC = dyn_cast<ExprWithCleanups>(LastStmt)) {
            LastStmt = EWC->getSubExpr();
          }
          if (const auto *CE = dyn_cast<CallExpr>(LastStmt)) {
            if (const FunctionDecl *Callee = CE->getDirectCallee();
                Callee && Callee->hasAttr<InferredNoReturnAttr>()) {
              return; // Don't warn about fall-through.
            }
          }
          // Direct throw.
          if (isa<CXXThrowExpr>(LastStmt)) {
            return; // Don't warn about fall-through.
          }
        }
      }
      bool NotInAllControlPaths = FallThroughType == MaybeFallThrough;
      S.Diag(RBrace, CD.diag_FallThrough_ReturnsNonVoid)
          << CD.FunKind << NotInAllControlPaths;
    }
    break;
  case NeverFallThroughOrReturn:
    if (ReturnsVoid && !HasNoReturn && CD.diag_NeverFallThroughOrReturn) {
      if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
        S.Diag(LBrace, CD.diag_NeverFallThroughOrReturn) << 0 << FD;
      } else if (const ObjCMethodDecl *MD = dyn_cast<ObjCMethodDecl>(D)) {
        S.Diag(LBrace, CD.diag_NeverFallThroughOrReturn) << 1 << MD;
      } else {
        S.Diag(LBrace, CD.diag_NeverFallThroughOrReturn);
      }
    }
    break;
  case NeverFallThrough:
    break;
  }
}

//===----------------------------------------------------------------------===//
// -Wuninitialized
//===----------------------------------------------------------------------===//

namespace {
/// ContainsReference - A visitor class to search for references to
/// a particular declaration (the needle) within any evaluated component of an
/// expression (recursively).
class ContainsReference : public ConstEvaluatedExprVisitor<ContainsReference> {
  bool FoundReference;
  const DeclRefExpr *Needle;

public:
  typedef ConstEvaluatedExprVisitor<ContainsReference> Inherited;

  ContainsReference(ASTContext &Context, const DeclRefExpr *Needle)
    : Inherited(Context), FoundReference(false), Needle(Needle) {}

  void VisitExpr(const Expr *E) {
    // Stop evaluating if we already have a reference.
    if (FoundReference)
      return;

    Inherited::VisitExpr(E);
  }

  void VisitDeclRefExpr(const DeclRefExpr *E) {
    if (E == Needle)
      FoundReference = true;
    else
      Inherited::VisitDeclRefExpr(E);
  }

  bool doesContainReference() const { return FoundReference; }
};
} // anonymous namespace

static bool SuggestInitializationFixit(Sema &S, const VarDecl *VD) {
  QualType VariableTy = VD->getType().getCanonicalType();
  if (VariableTy->isBlockPointerType() &&
      !VD->hasAttr<BlocksAttr>()) {
    S.Diag(VD->getLocation(), diag::note_block_var_fixit_add_initialization)
        << VD->getDeclName()
        << FixItHint::CreateInsertion(VD->getLocation(), "__block ");
    return true;
  }

  // Don't issue a fixit if there is already an initializer.
  if (VD->getInit())
    return false;

  // Don't suggest a fixit inside macros.
  if (VD->getEndLoc().isMacroID())
    return false;

  SourceLocation Loc = S.getLocForEndOfToken(VD->getEndLoc());

  // Suggest possible initialization (if any).
  std::string Init = S.getFixItZeroInitializerForType(VariableTy, Loc);
  if (Init.empty())
    return false;

  S.Diag(Loc, diag::note_var_fixit_add_initialization) << VD->getDeclName()
    << FixItHint::CreateInsertion(Loc, Init);
  return true;
}

/// Create a fixit to remove an if-like statement, on the assumption that its
/// condition is CondVal.
static void CreateIfFixit(Sema &S, const Stmt *If, const Stmt *Then,
                          const Stmt *Else, bool CondVal,
                          FixItHint &Fixit1, FixItHint &Fixit2) {
  if (CondVal) {
    // If condition is always true, remove all but the 'then'.
    Fixit1 = FixItHint::CreateRemoval(
        CharSourceRange::getCharRange(If->getBeginLoc(), Then->getBeginLoc()));
    if (Else) {
      SourceLocation ElseKwLoc = S.getLocForEndOfToken(Then->getEndLoc());
      Fixit2 =
          FixItHint::CreateRemoval(SourceRange(ElseKwLoc, Else->getEndLoc()));
    }
  } else {
    // If condition is always false, remove all but the 'else'.
    if (Else)
      Fixit1 = FixItHint::CreateRemoval(CharSourceRange::getCharRange(
          If->getBeginLoc(), Else->getBeginLoc()));
    else
      Fixit1 = FixItHint::CreateRemoval(If->getSourceRange());
  }
}

/// DiagUninitUse -- Helper function to produce a diagnostic for an
/// uninitialized use of a variable.
static void DiagUninitUse(Sema &S, const VarDecl *VD, const UninitUse &Use,
                          bool IsCapturedByBlock) {
  bool Diagnosed = false;

  switch (Use.getKind()) {
  case UninitUse::Always:
    S.Diag(Use.getUser()->getBeginLoc(), diag::warn_uninit_var)
        << VD->getDeclName() << IsCapturedByBlock
        << Use.getUser()->getSourceRange();
    return;

  case UninitUse::AfterDecl:
  case UninitUse::AfterCall:
    S.Diag(VD->getLocation(), diag::warn_sometimes_uninit_var)
        << VD->getDeclName() << IsCapturedByBlock
        << (Use.getKind() == UninitUse::AfterDecl ? 4 : 5)
        << VD->getLexicalDeclContext() << VD->getSourceRange();
    S.Diag(Use.getUser()->getBeginLoc(), diag::note_uninit_var_use)
        << IsCapturedByBlock << Use.getUser()->getSourceRange();
    return;

  case UninitUse::Maybe:
  case UninitUse::Sometimes:
    // Carry on to report sometimes-uninitialized branches, if possible,
    // or a 'may be used uninitialized' diagnostic otherwise.
    break;
  }

  // Diagnose each branch which leads to a sometimes-uninitialized use.
  for (UninitUse::branch_iterator I = Use.branch_begin(), E = Use.branch_end();
       I != E; ++I) {
    assert(Use.getKind() == UninitUse::Sometimes);

    const Expr *User = Use.getUser();
    const Stmt *Term = I->Terminator;

    // Information used when building the diagnostic.
    unsigned DiagKind;
    StringRef Str;
    SourceRange Range;

    // FixIts to suppress the diagnostic by removing the dead condition.
    // For all binary terminators, branch 0 is taken if the condition is true,
    // and branch 1 is taken if the condition is false.
    int RemoveDiagKind = -1;
    const char *FixitStr =
        S.getLangOpts().CPlusPlus ? (I->Output ? "true" : "false")
                                  : (I->Output ? "1" : "0");
    FixItHint Fixit1, Fixit2;

    switch (Term ? Term->getStmtClass() : Stmt::DeclStmtClass) {
    default:
      // Don't know how to report this. Just fall back to 'may be used
      // uninitialized'. FIXME: Can this happen?
      continue;

    // "condition is true / condition is false".
    case Stmt::IfStmtClass: {
      const IfStmt *IS = cast<IfStmt>(Term);
      DiagKind = 0;
      Str = "if";
      Range = IS->getCond()->getSourceRange();
      RemoveDiagKind = 0;
      CreateIfFixit(S, IS, IS->getThen(), IS->getElse(),
                    I->Output, Fixit1, Fixit2);
      break;
    }
    case Stmt::ConditionalOperatorClass: {
      const ConditionalOperator *CO = cast<ConditionalOperator>(Term);
      DiagKind = 0;
      Str = "?:";
      Range = CO->getCond()->getSourceRange();
      RemoveDiagKind = 0;
      CreateIfFixit(S, CO, CO->getTrueExpr(), CO->getFalseExpr(),
                    I->Output, Fixit1, Fixit2);
      break;
    }
    case Stmt::BinaryOperatorClass: {
      const BinaryOperator *BO = cast<BinaryOperator>(Term);
      if (!BO->isLogicalOp())
        continue;
      DiagKind = 0;
      Str = BO->getOpcodeStr();
      Range = BO->getLHS()->getSourceRange();
      RemoveDiagKind = 0;
      if ((BO->getOpcode() == BO_LAnd && I->Output) ||
          (BO->getOpcode() == BO_LOr && !I->Output))
        // true && y -> y, false || y -> y.
        Fixit1 = FixItHint::CreateRemoval(
            SourceRange(BO->getBeginLoc(), BO->getOperatorLoc()));
      else
        // false && y -> false, true || y -> true.
        Fixit1 = FixItHint::CreateReplacement(BO->getSourceRange(), FixitStr);
      break;
    }

    // "loop is entered / loop is exited".
    case Stmt::WhileStmtClass:
      DiagKind = 1;
      Str = "while";
      Range = cast<WhileStmt>(Term)->getCond()->getSourceRange();
      RemoveDiagKind = 1;
      Fixit1 = FixItHint::CreateReplacement(Range, FixitStr);
      break;
    case Stmt::ForStmtClass:
      DiagKind = 1;
      Str = "for";
      Range = cast<ForStmt>(Term)->getCond()->getSourceRange();
      RemoveDiagKind = 1;
      if (I->Output)
        Fixit1 = FixItHint::CreateRemoval(Range);
      else
        Fixit1 = FixItHint::CreateReplacement(Range, FixitStr);
      break;
    case Stmt::CXXForRangeStmtClass:
      if (I->Output == 1) {
        // The use occurs if a range-based for loop's body never executes.
        // That may be impossible, and there's no syntactic fix for this,
        // so treat it as a 'may be uninitialized' case.
        continue;
      }
      DiagKind = 1;
      Str = "for";
      Range = cast<CXXForRangeStmt>(Term)->getRangeInit()->getSourceRange();
      break;

    // "condition is true / loop is exited".
    case Stmt::DoStmtClass:
      DiagKind = 2;
      Str = "do";
      Range = cast<DoStmt>(Term)->getCond()->getSourceRange();
      RemoveDiagKind = 1;
      Fixit1 = FixItHint::CreateReplacement(Range, FixitStr);
      break;

    // "switch case is taken".
    case Stmt::CaseStmtClass:
      DiagKind = 3;
      Str = "case";
      Range = cast<CaseStmt>(Term)->getLHS()->getSourceRange();
      break;
    case Stmt::DefaultStmtClass:
      DiagKind = 3;
      Str = "default";
      Range = cast<DefaultStmt>(Term)->getDefaultLoc();
      break;
    }

    S.Diag(Range.getBegin(), diag::warn_sometimes_uninit_var)
      << VD->getDeclName() << IsCapturedByBlock << DiagKind
      << Str << I->Output << Range;
    S.Diag(User->getBeginLoc(), diag::note_uninit_var_use)
        << IsCapturedByBlock << User->getSourceRange();
    if (RemoveDiagKind != -1)
      S.Diag(Fixit1.RemoveRange.getBegin(), diag::note_uninit_fixit_remove_cond)
        << RemoveDiagKind << Str << I->Output << Fixit1 << Fixit2;

    Diagnosed = true;
  }

  if (!Diagnosed)
    S.Diag(Use.getUser()->getBeginLoc(), diag::warn_maybe_uninit_var)
        << VD->getDeclName() << IsCapturedByBlock
        << Use.getUser()->getSourceRange();
}

/// Diagnose uninitialized const reference usages.
static bool DiagnoseUninitializedConstRefUse(Sema &S, const VarDecl *VD,
                                             const UninitUse &Use) {
  S.Diag(Use.getUser()->getBeginLoc(), diag::warn_uninit_const_reference)
      << VD->getDeclName() << Use.getUser()->getSourceRange();
  return !S.getDiagnostics().isLastDiagnosticIgnored();
}

/// Diagnose uninitialized const pointer usages.
static bool DiagnoseUninitializedConstPtrUse(Sema &S, const VarDecl *VD,
                                             const UninitUse &Use) {
  S.Diag(Use.getUser()->getBeginLoc(), diag::warn_uninit_const_pointer)
      << VD->getDeclName() << Use.getUser()->getSourceRange();
  return !S.getDiagnostics().isLastDiagnosticIgnored();
}

/// DiagnoseUninitializedUse -- Helper function for diagnosing uses of an
/// uninitialized variable. This manages the different forms of diagnostic
/// emitted for particular types of uses. Returns true if the use was diagnosed
/// as a warning. If a particular use is one we omit warnings for, returns
/// false.
static bool DiagnoseUninitializedUse(Sema &S, const VarDecl *VD,
                                     const UninitUse &Use,
                                     bool alwaysReportSelfInit = false) {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Use.getUser())) {
    // Inspect the initializer of the variable declaration which is
    // being referenced prior to its initialization. We emit
    // specialized diagnostics for self-initialization, and we
    // specifically avoid warning about self references which take the
    // form of:
    //
    //   int x = x;
    //
    // This is used to indicate to GCC that 'x' is intentionally left
    // uninitialized. Proven code paths which access 'x' in
    // an uninitialized state after this will still warn.
    if (const Expr *Initializer = VD->getInit()) {
      if (!alwaysReportSelfInit && DRE == Initializer->IgnoreParenImpCasts())
        return false;

      ContainsReference CR(S.Context, DRE);
      CR.Visit(Initializer);
      if (CR.doesContainReference()) {
        S.Diag(DRE->getBeginLoc(), diag::warn_uninit_self_reference_in_init)
            << VD->getDeclName() << VD->getLocation() << DRE->getSourceRange();
        return !S.getDiagnostics().isLastDiagnosticIgnored();
      }
    }

    DiagUninitUse(S, VD, Use, false);
  } else {
    const BlockExpr *BE = cast<BlockExpr>(Use.getUser());
    if (VD->getType()->isBlockPointerType() && !VD->hasAttr<BlocksAttr>())
      S.Diag(BE->getBeginLoc(),
             diag::warn_uninit_byref_blockvar_captured_by_block)
          << VD->getDeclName()
          << VD->getType().getQualifiers().hasObjCLifetime();
    else
      DiagUninitUse(S, VD, Use, true);
  }

  // Report where the variable was declared when the use wasn't within
  // the initializer of that declaration & we didn't already suggest
  // an initialization fixit.
  if (!SuggestInitializationFixit(S, VD))
    S.Diag(VD->getBeginLoc(), diag::note_var_declared_here)
        << VD->getDeclName();

  return !S.getDiagnostics().isLastDiagnosticIgnored();
}

namespace {
class FallthroughMapper : public DynamicRecursiveASTVisitor {
public:
  FallthroughMapper(Sema &S) : FoundSwitchStatements(false), S(S) {
    ShouldWalkTypesOfTypeLocs = false;
  }

  bool foundSwitchStatements() const { return FoundSwitchStatements; }

  void markFallthroughVisited(const AttributedStmt *Stmt) {
    bool Found = FallthroughStmts.erase(Stmt);
    assert(Found);
    (void)Found;
  }

  typedef llvm::SmallPtrSet<const AttributedStmt *, 8> AttrStmts;

  const AttrStmts &getFallthroughStmts() const { return FallthroughStmts; }

  void fillReachableBlocks(CFG *Cfg) {
    assert(ReachableBlocks.empty() && "ReachableBlocks already filled");
    std::deque<const CFGBlock *> BlockQueue;

    ReachableBlocks.insert(&Cfg->getEntry());
    BlockQueue.push_back(&Cfg->getEntry());
    // Mark all case blocks reachable to avoid problems with switching on
    // constants, covered enums, etc.
    // These blocks can contain fall-through annotations, and we don't want to
    // issue a warn_fallthrough_attr_unreachable for them.
    for (const auto *B : *Cfg) {
      const Stmt *L = B->getLabel();
      if (isa_and_nonnull<SwitchCase>(L) && ReachableBlocks.insert(B).second)
        BlockQueue.push_back(B);
    }

    while (!BlockQueue.empty()) {
      const CFGBlock *P = BlockQueue.front();
      BlockQueue.pop_front();
      for (const CFGBlock *B : P->succs()) {
        if (B && ReachableBlocks.insert(B).second)
          BlockQueue.push_back(B);
      }
    }
  }

  bool checkFallThroughIntoBlock(const CFGBlock &B, int &AnnotatedCnt,
                                 bool IsTemplateInstantiation) {
    assert(!ReachableBlocks.empty() && "ReachableBlocks empty");

    int UnannotatedCnt = 0;
    AnnotatedCnt = 0;

    std::deque<const CFGBlock *> BlockQueue(B.pred_begin(), B.pred_end());
    while (!BlockQueue.empty()) {
      const CFGBlock *P = BlockQueue.front();
      BlockQueue.pop_front();
      if (!P)
        continue;

      const Stmt *Term = P->getTerminatorStmt();
      if (isa_and_nonnull<SwitchStmt>(Term))
        continue; // Switch statement, good.

      const SwitchCase *SW = dyn_cast_or_null<SwitchCase>(P->getLabel());
      if (SW && SW->getSubStmt() == B.getLabel() && P->begin() == P->end())
        continue; // Previous case label has no statements, good.

      const LabelStmt *L = dyn_cast_or_null<LabelStmt>(P->getLabel());
      if (L && L->getSubStmt() == B.getLabel() && P->begin() == P->end())
        continue; // Case label is preceded with a normal label, good.

      if (!ReachableBlocks.count(P)) {
        for (const CFGElement &Elem : llvm::reverse(*P)) {
          if (std::optional<CFGStmt> CS = Elem.getAs<CFGStmt>()) {
            if (const AttributedStmt *AS = asFallThroughAttr(CS->getStmt())) {
              // Don't issue a warning for an unreachable fallthrough
              // attribute in template instantiations as it may not be
              // unreachable in all instantiations of the template.
              if (!IsTemplateInstantiation)
                S.Diag(AS->getBeginLoc(),
                       diag::warn_unreachable_fallthrough_attr);
              markFallthroughVisited(AS);
              ++AnnotatedCnt;
              break;
            }
            // Don't care about other unreachable statements.
          }
        }
          // If there are no unreachable statements, this may be a special
          // case in CFG:
          // case X: {
          //    A a;  // A has a destructor.
          //    break;
          // }
          // // <<<< This place is represented by a 'hanging' CFG block.
          // case Y:
          continue;
      }

        const Stmt *LastStmt = getLastStmt(*P);
        if (const AttributedStmt *AS = asFallThroughAttr(LastStmt)) {
          markFallthroughVisited(AS);
          ++AnnotatedCnt;
          continue; // Fallthrough annotation, good.
        }

        if (!LastStmt) { // This block contains no executable statements.
          // Traverse its predecessors.
          std::copy(P->pred_begin(), P->pred_end(),
                    std::back_inserter(BlockQueue));
          continue;
        }

        ++UnannotatedCnt;
    }
    return !!UnannotatedCnt;
  }

  bool VisitAttributedStmt(AttributedStmt *S) override {
    if (asFallThroughAttr(S))
      FallthroughStmts.insert(S);
    return true;
  }

  bool VisitSwitchStmt(SwitchStmt *S) override {
    FoundSwitchStatements = true;
    return true;
  }

    // We don't want to traverse local type declarations. We analyze their
    // methods separately.
    bool TraverseDecl(Decl *D) override { return true; }

    // We analyze lambda bodies separately. Skip them here.
    bool TraverseLambdaExpr(LambdaExpr *LE) override {
      // Traverse the captures, but not the body.
      for (const auto C : zip(LE->captures(), LE->capture_inits()))
        TraverseLambdaCapture(LE, &std::get<0>(C), std::get<1>(C));
      return true;
    }

  private:

    static const AttributedStmt *asFallThroughAttr(const Stmt *S) {
      if (const AttributedStmt *AS = dyn_cast_or_null<AttributedStmt>(S)) {
        if (hasSpecificAttr<FallThroughAttr>(AS->getAttrs()))
          return AS;
      }
      return nullptr;
    }

    static const Stmt *getLastStmt(const CFGBlock &B) {
      if (const Stmt *Term = B.getTerminatorStmt())
        return Term;
      for (const CFGElement &Elem : llvm::reverse(B))
        if (std::optional<CFGStmt> CS = Elem.getAs<CFGStmt>())
          return CS->getStmt();
      // Workaround to detect a statement thrown out by CFGBuilder:
      //   case X: {} case Y:
      //   case X: ; case Y:
      if (const SwitchCase *SW = dyn_cast_or_null<SwitchCase>(B.getLabel()))
        if (!isa<SwitchCase>(SW->getSubStmt()))
          return SW->getSubStmt();

      return nullptr;
    }

    bool FoundSwitchStatements;
    AttrStmts FallthroughStmts;
    Sema &S;
    llvm::SmallPtrSet<const CFGBlock *, 16> ReachableBlocks;
};
} // anonymous namespace

static StringRef getFallthroughAttrSpelling(Preprocessor &PP,
                                            SourceLocation Loc) {
  TokenValue FallthroughTokens[] = {
    tok::l_square, tok::l_square,
    PP.getIdentifierInfo("fallthrough"),
    tok::r_square, tok::r_square
  };

  TokenValue ClangFallthroughTokens[] = {
    tok::l_square, tok::l_square, PP.getIdentifierInfo("clang"),
    tok::coloncolon, PP.getIdentifierInfo("fallthrough"),
    tok::r_square, tok::r_square
  };

  bool PreferClangAttr = !PP.getLangOpts().CPlusPlus17 && !PP.getLangOpts().C23;

  StringRef MacroName;
  if (PreferClangAttr)
    MacroName = PP.getLastMacroWithSpelling(Loc, ClangFallthroughTokens);
  if (MacroName.empty())
    MacroName = PP.getLastMacroWithSpelling(Loc, FallthroughTokens);
  if (MacroName.empty() && !PreferClangAttr)
    MacroName = PP.getLastMacroWithSpelling(Loc, ClangFallthroughTokens);
  if (MacroName.empty()) {
    if (!PreferClangAttr)
      MacroName = "[[fallthrough]]";
    else if (PP.getLangOpts().CPlusPlus)
      MacroName = "[[clang::fallthrough]]";
    else
      MacroName = "__attribute__((fallthrough))";
  }
  return MacroName;
}

static void DiagnoseSwitchLabelsFallthrough(Sema &S, AnalysisDeclContext &AC,
                                            bool PerFunction) {
  FallthroughMapper FM(S);
  FM.TraverseStmt(AC.getBody());

  if (!FM.foundSwitchStatements())
    return;

  if (PerFunction && FM.getFallthroughStmts().empty())
    return;

  CFG *Cfg = AC.getCFG();

  if (!Cfg)
    return;

  FM.fillReachableBlocks(Cfg);

  for (const CFGBlock *B : llvm::reverse(*Cfg)) {
    const Stmt *Label = B->getLabel();

    if (!isa_and_nonnull<SwitchCase>(Label))
      continue;

    int AnnotatedCnt;

    bool IsTemplateInstantiation = false;
    if (const FunctionDecl *Function = dyn_cast<FunctionDecl>(AC.getDecl()))
      IsTemplateInstantiation = Function->isTemplateInstantiation();
    if (!FM.checkFallThroughIntoBlock(*B, AnnotatedCnt,
                                      IsTemplateInstantiation))
      continue;

    S.Diag(Label->getBeginLoc(),
           PerFunction ? diag::warn_unannotated_fallthrough_per_function
                       : diag::warn_unannotated_fallthrough);

    if (!AnnotatedCnt) {
      SourceLocation L = Label->getBeginLoc();
      if (L.isMacroID())
        continue;

      const Stmt *Term = B->getTerminatorStmt();
      // Skip empty cases.
      while (B->empty() && !Term && B->succ_size() == 1) {
        B = *B->succ_begin();
        Term = B->getTerminatorStmt();
      }
      if (!(B->empty() && isa_and_nonnull<BreakStmt>(Term))) {
        Preprocessor &PP = S.getPreprocessor();
        StringRef AnnotationSpelling = getFallthroughAttrSpelling(PP, L);
        SmallString<64> TextToInsert(AnnotationSpelling);
        TextToInsert += "; ";
        S.Diag(L, diag::note_insert_fallthrough_fixit)
            << AnnotationSpelling
            << FixItHint::CreateInsertion(L, TextToInsert);
      }
      S.Diag(L, diag::note_insert_break_fixit)
          << FixItHint::CreateInsertion(L, "break; ");
    }
  }

  for (const auto *F : FM.getFallthroughStmts())
    S.Diag(F->getBeginLoc(), diag::err_fallthrough_attr_invalid_placement);
}

static bool isInLoop(const ASTContext &Ctx, const ParentMap &PM,
                     const Stmt *S) {
  assert(S);

  do {
    switch (S->getStmtClass()) {
    case Stmt::ForStmtClass:
    case Stmt::WhileStmtClass:
    case Stmt::CXXForRangeStmtClass:
    case Stmt::ObjCForCollectionStmtClass:
      return true;
    case Stmt::DoStmtClass: {
      Expr::EvalResult Result;
      if (!cast<DoStmt>(S)->getCond()->EvaluateAsInt(Result, Ctx))
        return true;
      return Result.Val.getInt().getBoolValue();
    }
    default:
      break;
    }
  } while ((S = PM.getParent(S)));

  return false;
}

static void diagnoseRepeatedUseOfWeak(Sema &S,
                                      const sema::FunctionScopeInfo *CurFn,
                                      const Decl *D,
                                      const ParentMap &PM) {
  typedef sema::FunctionScopeInfo::WeakObjectProfileTy WeakObjectProfileTy;
  typedef sema::FunctionScopeInfo::WeakObjectUseMap WeakObjectUseMap;
  typedef sema::FunctionScopeInfo::WeakUseVector WeakUseVector;
  typedef std::pair<const Stmt *, WeakObjectUseMap::const_iterator>
  StmtUsesPair;

  ASTContext &Ctx = S.getASTContext();

  const WeakObjectUseMap &WeakMap = CurFn->getWeakObjectUses();

  // Extract all weak objects that are referenced more than once.
  SmallVector<StmtUsesPair, 8> UsesByStmt;
  for (WeakObjectUseMap::const_iterator I = WeakMap.begin(), E = WeakMap.end();
       I != E; ++I) {
    const WeakUseVector &Uses = I->second;

    // Find the first read of the weak object.
    WeakUseVector::const_iterator UI = Uses.begin(), UE = Uses.end();
    for ( ; UI != UE; ++UI) {
      if (UI->isUnsafe())
        break;
    }

    // If there were only writes to this object, don't warn.
    if (UI == UE)
      continue;

    // If there was only one read, followed by any number of writes, and the
    // read is not within a loop, don't warn. Additionally, don't warn in a
    // loop if the base object is a local variable -- local variables are often
    // changed in loops.
    if (UI == Uses.begin()) {
      WeakUseVector::const_iterator UI2 = UI;
      for (++UI2; UI2 != UE; ++UI2)
        if (UI2->isUnsafe())
          break;

      if (UI2 == UE) {
        if (!isInLoop(Ctx, PM, UI->getUseExpr()))
          continue;

        const WeakObjectProfileTy &Profile = I->first;
        if (!Profile.isExactProfile())
          continue;

        const NamedDecl *Base = Profile.getBase();
        if (!Base)
          Base = Profile.getProperty();
        assert(Base && "A profile always has a base or property.");

        if (const VarDecl *BaseVar = dyn_cast<VarDecl>(Base))
          if (BaseVar->hasLocalStorage() && !isa<ParmVarDecl>(Base))
            continue;
      }
    }

    UsesByStmt.push_back(StmtUsesPair(UI->getUseExpr(), I));
  }

  if (UsesByStmt.empty())
    return;

  // Sort by first use so that we emit the warnings in a deterministic order.
  SourceManager &SM = S.getSourceManager();
  llvm::sort(UsesByStmt,
             [&SM](const StmtUsesPair &LHS, const StmtUsesPair &RHS) {
               return SM.isBeforeInTranslationUnit(LHS.first->getBeginLoc(),
                                                   RHS.first->getBeginLoc());
             });

  // Classify the current code body for better warning text.
  // This enum should stay in sync with the cases in
  // warn_arc_repeated_use_of_weak and warn_arc_possible_repeated_use_of_weak.
  // FIXME: Should we use a common classification enum and the same set of
  // possibilities all throughout Sema?
  enum {
    Function,
    Method,
    Block,
    Lambda
  } FunctionKind;

  if (isa<sema::BlockScopeInfo>(CurFn))
    FunctionKind = Block;
  else if (isa<sema::LambdaScopeInfo>(CurFn))
    FunctionKind = Lambda;
  else if (isa<ObjCMethodDecl>(D))
    FunctionKind = Method;
  else
    FunctionKind = Function;

  // Iterate through the sorted problems and emit warnings for each.
  for (const auto &P : UsesByStmt) {
    const Stmt *FirstRead = P.first;
    const WeakObjectProfileTy &Key = P.second->first;
    const WeakUseVector &Uses = P.second->second;

    // For complicated expressions like 'a.b.c' and 'x.b.c', WeakObjectProfileTy
    // may not contain enough information to determine that these are different
    // properties. We can only be 100% sure of a repeated use in certain cases,
    // and we adjust the diagnostic kind accordingly so that the less certain
    // case can be turned off if it is too noisy.
    unsigned DiagKind;
    if (Key.isExactProfile())
      DiagKind = diag::warn_arc_repeated_use_of_weak;
    else
      DiagKind = diag::warn_arc_possible_repeated_use_of_weak;

    // Classify the weak object being accessed for better warning text.
    // This enum should stay in sync with the cases in
    // warn_arc_repeated_use_of_weak and warn_arc_possible_repeated_use_of_weak.
    enum {
      Variable,
      Property,
      ImplicitProperty,
      Ivar
    } ObjectKind;

    const NamedDecl *KeyProp = Key.getProperty();
    if (isa<VarDecl>(KeyProp))
      ObjectKind = Variable;
    else if (isa<ObjCPropertyDecl>(KeyProp))
      ObjectKind = Property;
    else if (isa<ObjCMethodDecl>(KeyProp))
      ObjectKind = ImplicitProperty;
    else if (isa<ObjCIvarDecl>(KeyProp))
      ObjectKind = Ivar;
    else
      llvm_unreachable("Unexpected weak object kind!");

    // Do not warn about IBOutlet weak property receivers being set to null
    // since they are typically only used from the main thread.
    if (const ObjCPropertyDecl *Prop = dyn_cast<ObjCPropertyDecl>(KeyProp))
      if (Prop->hasAttr<IBOutletAttr>())
        continue;

    // Show the first time the object was read.
    S.Diag(FirstRead->getBeginLoc(), DiagKind)
        << int(ObjectKind) << KeyProp << int(FunctionKind)
        << FirstRead->getSourceRange();

    // Print all the other accesses as notes.
    for (const auto &Use : Uses) {
      if (Use.getUseExpr() == FirstRead)
        continue;
      S.Diag(Use.getUseExpr()->getBeginLoc(),
             diag::note_arc_weak_also_accessed_here)
          << Use.getUseExpr()->getSourceRange();
    }
  }
}

namespace clang {
namespace {
typedef SmallVector<PartialDiagnosticAt, 1> OptionalNotes;
typedef std::pair<PartialDiagnosticAt, OptionalNotes> DelayedDiag;
typedef std::list<DelayedDiag> DiagList;

struct SortDiagBySourceLocation {
  SourceManager &SM;
  SortDiagBySourceLocation(SourceManager &SM) : SM(SM) {}

  bool operator()(const DelayedDiag &left, const DelayedDiag &right) {
    // Although this call will be slow, this is only called when outputting
    // multiple warnings.
    return SM.isBeforeInTranslationUnit(left.first.first, right.first.first);
  }
};
} // anonymous namespace
} // namespace clang

namespace {
/// Profiles that opt into Clang's CFG-uninitialized-variables analysis. Each
/// entry pairs the profile name with the diagnostic to emit when an
/// uninitialized read is found and not suppressed at the use site. Adding a
/// new profile that wants to ride this analysis is a single row here plus a
/// ProfileRule diagnostic in DiagnosticSemaKinds.td; the three hook columns
/// are optional (see ProfilesFrameworkInternals.rst, "CFG Riders").
struct CFGProfileEntry {
  StringRef Name;
  /// The uninitialized-read diagnostic; 0 opts the row out of the
  /// uninitialized-variables reporter, so it rides the analysis for its hooks
  /// alone.
  unsigned DiagID;
  /// When non-null, exempts a variable from the row's uninitialized-read
  /// rule. Consulted once per variable, before either reporter arm; must not
  /// emit.
  bool (*VarExempt)(Sema &S, const VarDecl *VD) = nullptr;
  /// When non-null, adds the row's extra always-add statement classes to the
  /// CFG build options; applied on both analysis paths.
  void (*ConfigureCFG)(CFG::BuildOptions &Options) = nullptr;
  /// When non-null, a whole-function pass run on both analysis paths after
  /// the uninitialized-variables reporter has flushed. Owns its rules and
  /// diagnostics, gating each check site through shouldEmitProfileViolation;
  /// receives the row so the profile's identity flows from the table.
  void (*ExtraPass)(Sema &S, const Decl *D, AnalysisDeclContext &AC,
                    const CFGProfileEntry &Entry) = nullptr;
};

/// VarExempt hook of the test::cfg_hooks pilot: a variable named with an
/// "exempt" prefix is exempt from the row's uninitialized-read rule.
static bool isTestCFGHooksExemptVar(Sema &, const VarDecl *VD) {
  return VD->getIdentifier() && VD->getName().starts_with("exempt");
}

/// ConfigureCFG hook of the test::cfg_hooks pilot: always-add lambda
/// expressions so the ExtraPass sees one element per lambda in the
/// non-linearized CFG shape.
static void configureTestCFGHooksCFG(CFG::BuildOptions &Options) {
  Options.setAlwaysAdd(Stmt::LambdaExprClass);
}

/// ExtraPass of the test::cfg_hooks pilot: diagnoses every lambda-expression
/// CFG element under its own "lambda" rule. The match keys on a class the
/// row's ConfigureCFG hook always-adds (extraction arms may only match
/// always-add classes or unconditional elements; see
/// ProfilesFrameworkInternals.rst, "CFG Riders").
static void runTestCFGHooksPass(Sema &S, const Decl *, AnalysisDeclContext &AC,
                                const CFGProfileEntry &Entry) {
  CFG *cfg = AC.getCFG();
  if (!cfg)
    return;
  for (const CFGBlock *B : *cfg)
    for (const CFGElement &E : *B)
      if (std::optional<CFGStmt> CS = E.getAs<CFGStmt>())
        if (const auto *LE = dyn_cast<LambdaExpr>(CS->getStmt()))
          if (S.Profiles().shouldEmitProfileViolation(
                  Entry.Name, diag::err_profile_cfg_hooks_test,
                  LE->getBeginLoc(), /*D=*/nullptr, /*PostParse=*/true))
            S.Diag(LE->getBeginLoc(), diag::err_profile_cfg_hooks_test)
                << Entry.Name;
}

/// VarExempt hook of the std::init row: std::byte may be read while
/// uninitialized (P4222R2 §4).
static bool stdInitVarExempt(Sema &S, const VarDecl *VD) {
  return S.Context.getBaseElementType(VD->getType())->isStdByteType();
}

/// ExtraPass of the std::init row: the member read-before-init engine, defined
/// after its parts below.
static void runStdInitMemberReadChecks(Sema &S, const Decl *D,
                                       AnalysisDeclContext &AC,
                                       const CFGProfileEntry &Entry);

constexpr CFGProfileEntry CFGProfiles[] = {
    {"test::uninit_read", diag::err_profile_uninit_read},
    {"test::cfg_hooks", diag::err_profile_cfg_hooks_uninit_read,
     &isTestCFGHooksExemptVar, &configureTestCFGHooksCFG, &runTestCFGHooksPass},
    {"std::init", diag::err_init_uninit_read, &stdInitVarExempt,
     /*ConfigureCFG=*/nullptr, &runStdInitMemberReadChecks},
};

/// Diagnose an uninitialized read of \p vd under the live CFGProfiles rows,
/// reporting a self-init at its root cause and otherwise the first recorded
/// real use; returns true if a diagnostic was emitted. A row whose profile is
/// not live, or whose VarExempt hook exempts \p vd, takes no part in either
/// arm.
static bool
tryDiagnoseProfileUninitRead(Sema &S, AnalysisDeclContext &AC,
                             const VarDecl *vd, bool hasSelfInit,
                             const SmallVectorImpl<UninitUse> &vec) {
  SmallVector<const CFGProfileEntry *, 4> Rows;
  for (const CFGProfileEntry &E : CFGProfiles)
    if (E.DiagID != 0 && S.Profiles().isProfileLive(E.Name) &&
        (!E.VarExempt || !E.VarExempt(S, vd)))
      Rows.push_back(&E);
  if (Rows.empty())
    return false;

  // A self-init (`int x = x;`) reads the uninitialized variable in its own
  // initializer, but records no entry in the uses vec, so check it first --
  // at the root cause, mirroring the default path's self-init preference.
  if (hasSelfInit && vd->getInit()) {
    const Expr *Init = vd->getInit()->IgnoreParenCasts();
    for (const CFGProfileEntry *E : Rows) {
      if (!S.Profiles().shouldEmitProfileViolation(
              E->Name, E->DiagID, Init->getBeginLoc(), /*D=*/nullptr,
              /*PostParse=*/true))
        continue;
      S.Diag(Init->getBeginLoc(), E->DiagID) << E->Name << vd->getDeclName();
      S.Diag(vd->getLocation(), diag::note_var_declared_here)
          << vd->getDeclName();
      return true;
    }
  }

  // If a row's rule is active at any real use site (enforced or enabled, and
  // not suppressed there), emit the profile diagnostic and skip the default
  // warning path entirely.
  for (const UninitUse &U : vec) {
    // A const-reference binding or address-taking use is not a read; it is
    // pointer/reference-binding territory, checked at the binding site.
    if (U.isConstRefOrPtrUse())
      continue;
    for (const CFGProfileEntry *E : Rows) {
      if (!S.Profiles().shouldEmitProfileViolation(
              E->Name, E->DiagID, U.getUser()->getBeginLoc(), /*D=*/nullptr,
              /*PostParse=*/true))
        continue;
      S.Diag(U.getUser()->getBeginLoc(), E->DiagID)
          << E->Name << vd->getDeclName();
      S.Diag(vd->getLocation(), diag::note_var_declared_here)
          << vd->getDeclName();
      return true;
    }
  }
  return false;
}

/// True if \p E denotes the current object: `this` (the implicit/explicit
/// pointer of an arrow access) or `*this` (the object lvalue of a dot
/// access), seen through the transparent casts the recognizers see through
/// (`(Base &)*this`, `(Base *)this`).
static bool isCurrentObjectBase(const Expr *E) {
  E = SemaProfiles::ignoreTransparentCasts(E);
  if (isa<CXXThisExpr>(E))
    return true;
  const auto *UO = dyn_cast<UnaryOperator>(E);
  return UO && UO->getOpcode() == UO_Deref &&
         isa<CXXThisExpr>(
             SemaProfiles::ignoreTransparentCasts(UO->getSubExpr()));
}

using BasePath = SemaProfiles::BasePath;
using MemberStep = SemaProfiles::MemberStep;
using MemberPath = SemaProfiles::MemberPath;

/// One flow-tracked std::init storage entity of the analyzed body: an
/// [[uninit]] member of the current object or of a directly named local or
/// by-value parameter -- a scalar, or a class or array object tracked whole --
/// (directly or inside a member subobject;
/// SemaProfiles::forEachFlowTrackedMember), an [[uninit]] local or parameter
/// as a whole, the referent of a [[ref_to_uninit]] local pointer, reference,
/// or handle (parameters included), which the dataflow identifies per program
/// point (FlowState::Target), the referent of a [[ref_to_uninit]] pointer
/// member of the current object (MemberPointee, keyed by the member's path;
/// SemaProfiles::markedPointerMembers), which enters asserted
/// (FlowState::EntryAsserted), the current object as a whole outside its
/// constructors, which enters live (SemaProfiles::FlowLeafUse), or an
/// [[uninit]] member of a WholeLocal's or Pointee's storage (SubMember), whose
/// state every event on that storage carries along except a
/// default-initialization's credit.
struct TrackedEntity {
  enum class Kind : uint8_t {
    CurrentObjectMember,
    LocalMember,
    WholeLocal,
    Pointee,
    MemberPointee,
    CurrentObject,
    SubMember
  };
  Kind K;
  /// The member is tracked as a whole object (member kinds).
  bool Whole = false;
  /// Read events are extracted for this entity (a constructor body's or a
  /// default-initialized constructor-less local's members).
  bool TrackReads = true;
  /// The entity belongs to an enclosing function (a variable a lambda body
  /// reaches by capture): its entry state is unknown -- possibly assigned,
  /// on no path definitely, and on no path known unassigned -- rather than
  /// unassigned.
  bool UnknownEntry = false;
  /// The member path from the object (member kinds; its key in the object's
  /// SemaProfiles::flowTrackedMembers table) or to a MemberPointee's pointer
  /// member (SemaProfiles::markedPointerMembers), referring into that cached
  /// table; empty otherwise.
  ArrayRef<MemberStep> Steps;
  /// WholeLocal and Pointee: the SubMember entities of the storage's
  /// flow-tracked members, in SubClass's flow-tracked member table order.
  std::pair<unsigned, unsigned> Subs{0, 0};
  const CXXRecordDecl *SubClass = nullptr;
  /// The type of the storage the entity stands for: the member's or the
  /// local's declared type, or the referent type of a marked pointer or
  /// reference (its anonymous referent). A credit or store covers the entity
  /// only through a designation of this type (designationCovers).
  QualType Type;

  /// The member a member entity stands for; null otherwise.
  const FieldDecl *field() const {
    return isMember() ? Steps.back().Field : nullptr;
  }
  /// A member of the current object or of a local.
  bool isMember() const {
    return K == Kind::CurrentObjectMember || K == Kind::LocalMember;
  }
  /// The referent of a marked pointer, reference, or handle, identified per
  /// program point (FlowState::Target).
  bool isPointee() const {
    return K == Kind::Pointee || K == Kind::MemberPointee;
  }
};

/// True if \p T transitively holds a pointer, reference, or member pointer
/// -- through record fields, bases, and array elements -- so storage of that
/// type may denote a tracked member; an incomplete or dependent type may.
static bool typeHoldsPointerOrReference(
    QualType T, llvm::SmallPtrSetImpl<const RecordDecl *> &Visited) {
  if (T->isDependentType() || T->isAnyPointerType() || T->isReferenceType() ||
      T->isMemberPointerType() || T->isBlockPointerType())
    return true;
  if (const ArrayType *AT = T->getAsArrayTypeUnsafe())
    return typeHoldsPointerOrReference(AT->getElementType(), Visited);
  const RecordDecl *RD = T->getAsRecordDecl();
  if (!RD)
    return false;
  RD = RD->getDefinition();
  if (!RD)
    return true;
  if (!Visited.insert(RD).second)
    return false;
  for (const FieldDecl *F : RD->fields())
    if (typeHoldsPointerOrReference(F->getType(), Visited))
      return true;
  if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD))
    for (const CXXBaseSpecifier &BS : CRD->bases())
      if (typeHoldsPointerOrReference(BS.getType(), Visited))
        return true;
  return false;
}

/// \p T canonical and unqualified, with a signed or plain integer type
/// replaced by its unsigned variant (char, signed char, short, int, long,
/// long long, __int128).
static QualType designationKey(const ASTContext &Ctx, QualType T) {
  T = T.getCanonicalType().getUnqualifiedType();
  const auto *BT = T->getAs<BuiltinType>();
  if (!BT)
    return T;
  switch (BT->getKind()) {
  case BuiltinType::Char_S:
  case BuiltinType::Char_U:
  case BuiltinType::SChar:
    return Ctx.UnsignedCharTy;
  case BuiltinType::Short:
    return Ctx.UnsignedShortTy;
  case BuiltinType::Int:
    return Ctx.UnsignedIntTy;
  case BuiltinType::Long:
    return Ctx.UnsignedLongTy;
  case BuiltinType::LongLong:
    return Ctx.UnsignedLongLongTy;
  case BuiltinType::Int128:
    return Ctx.UnsignedInt128Ty;
  default:
    return T;
  }
}

/// The number of elements of \p T as a constant array or a std::array, nested
/// ones multiplied out, or std::nullopt when \p T is neither or a bound is
/// unknown.
static std::optional<uint64_t> designationExtent(const ASTContext &Ctx,
                                                 QualType T) {
  std::optional<uint64_t> N;
  while (true) {
    if (const ConstantArrayType *CAT = Ctx.getAsConstantArrayType(T)) {
      N = N.value_or(1) * CAT->getZExtSize();
      T = CAT->getElementType();
      continue;
    }
    if (T->isArrayType())
      return std::nullopt;
    const auto *CTSD = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
        T->getAsCXXRecordDecl());
    if (!CTSD || !CTSD->getIdentifier() || CTSD->getName() != "array" ||
        !CTSD->isInStdNamespace() || CTSD->getTemplateArgs().size() != 2 ||
        CTSD->getTemplateArgs()[0].getKind() != TemplateArgument::Type ||
        CTSD->getTemplateArgs()[1].getKind() != TemplateArgument::Integral)
      return N;
    N = N.value_or(1) *
        CTSD->getTemplateArgs()[1].getAsIntegral().getZExtValue();
    T = CTSD->getTemplateArgs()[0].getAsType();
  }
}

/// True if a credit, store, or access through a glvalue of type
/// \p Designated covers the whole of storage of type \p Entity: the types
/// agree canonically and unqualified, same-size signed and unsigned integer
/// variants alike, an array or a std::array compared by its base element
/// type -- an array designation also by its extent (designationExtent), so it
/// covers no larger array. Storage of type void (a void pointer's referent)
/// has no type to disagree with. With
/// \p RawExempt -- a [[now_init]] parameter or an initializing parameter,
/// whose callee claims the bytes -- a designated void, char, unsigned char,
/// or std::byte covers any storage. Any other designation names a subobject
/// (ProfilesFrameworkInternals.rst, "Flow-Tracked Storage (std::init)").
static bool designationCovers(const ASTContext &Ctx, QualType Designated,
                              QualType Entity, bool RawExempt) {
  // A std::array counts by its elements, as an array does (P4222R2 §5.5).
  auto BaseElement = [&](QualType T) {
    T = Ctx.getBaseElementType(T);
    while (const auto *CTSD = dyn_cast_or_null<ClassTemplateSpecializationDecl>(
               T->getAsCXXRecordDecl())) {
      if (!CTSD->getIdentifier() || CTSD->getName() != "array" ||
          !CTSD->isInStdNamespace() || CTSD->getTemplateArgs().size() == 0 ||
          CTSD->getTemplateArgs()[0].getKind() != TemplateArgument::Type)
        break;
      T = Ctx.getBaseElementType(CTSD->getTemplateArgs()[0].getAsType());
    }
    return T;
  };
  QualType D = BaseElement(Designated);
  QualType E = BaseElement(Entity);
  if (E->isVoidType())
    return true;
  if (RawExempt && (D->isVoidType() || D->isStdByteType() ||
                    Ctx.hasSameUnqualifiedType(D, Ctx.CharTy) ||
                    Ctx.hasSameUnqualifiedType(D, Ctx.UnsignedCharTy)))
    return true;
  if (std::optional<uint64_t> DN = designationExtent(Ctx, Designated))
    if (std::optional<uint64_t> EN = designationExtent(Ctx, Entity);
        EN && *DN < *EN)
      return false;
  return designationKey(Ctx, D) == designationKey(Ctx, E);
}

/// The derived-to-base path from \p Derived to its base \p Base, if exactly
/// one path of non-virtual bases leads there.
static std::optional<SemaProfiles::BasePath>
uniqueBasePath(const CXXRecordDecl *Derived, const CXXRecordDecl *Base) {
  const CXXRecordDecl *Target = Base->getCanonicalDecl();
  std::optional<SemaProfiles::BasePath> Found;
  unsigned Count = 0;
  SemaProfiles::BasePath Cur;
  auto Walk = [&](auto &Self, const CXXRecordDecl *RD) -> void {
    if (Count > 1)
      return;
    if (RD->getCanonicalDecl() == Target) {
      ++Count;
      Found = Cur;
      return;
    }
    RD = RD->getDefinition();
    if (!RD)
      return;
    for (const CXXBaseSpecifier &B : RD->bases()) {
      const CXXRecordDecl *BRD = B.getType()->getAsCXXRecordDecl();
      if (B.isVirtual() || !BRD)
        continue;
      Cur.push_back(BRD->getCanonicalDecl());
      Self(Self, BRD);
      Cur.pop_back();
    }
  };
  Walk(Walk, Derived);
  if (Count != 1)
    return std::nullopt;
  return Found;
}

/// The SubMember a leaf below a WholeLocal's or Pointee's storage names: the
/// class its member steps start from and their match in that class's
/// flow-tracked member table, mapped onto the storage referred to at each
/// transfer (TrackedStorage::subEntity).
struct SubKey {
  const CXXRecordDecl *Class = nullptr;
  unsigned Index = 0;
  /// The leaf names the member whole, not a subobject of it.
  bool Exact = false;
};

/// The entity table of one analyzed body and the lvalue-shape resolvers
/// over it. Each object's entities are contiguous, so a whole-object event
/// covers a range.
struct TrackedStorage {
  explicit TrackedStorage(Sema &S) : Ctx(S.Context), Profiles(S.Profiles()) {}

  const ASTContext &Ctx;
  const SemaProfiles &Profiles;
  SmallVector<TrackedEntity, 8> Entities;
  /// The contiguous entity range of each tracked local's members.
  llvm::DenseMap<const VarDecl *, std::pair<unsigned, unsigned>> LocalRange;
  /// The class of each local in LocalRange.
  llvm::DenseMap<const VarDecl *, const CXXRecordDecl *> LocalClass;
  /// The WholeLocal or Pointee entity of a marked local or parameter.
  llvm::DenseMap<const VarDecl *, unsigned> LocalEntity;
  /// The current object's range (an instance member function body); empty
  /// otherwise.
  std::pair<unsigned, unsigned> CurrentObjectRange{0, 0};
  /// The class of the current object whose members are tracked; null when
  /// none is.
  const CXXRecordDecl *CurrentObjectClass = nullptr;
  /// The CurrentObject entity, when the body binds the current object to a
  /// lifecycle role or destroys it.
  std::optional<unsigned> CurrentObjectRoot;
  /// The MemberPointee entities, in markedPointerMembers order, and the
  /// current object's class they belong to.
  std::pair<unsigned, unsigned> MemberPointerRange{0, 0};
  const CXXRecordDecl *MemberPointerClass = nullptr;
  /// The tracked local a tracked copy was copy- or move-constructed from,
  /// and the derived-to-base path of the conversion a slicing copy applied
  /// to it (empty for a same-class copy).
  struct CopyOrigin {
    const VarDecl *Src = nullptr;
    BasePath Path;
  };
  llvm::DenseMap<const VarDecl *, CopyOrigin> CopySource;
  /// The marked pointers, references, and handles each ViaPointer reseat of
  /// the body connects (runStdInitMemberReadChecks): only a pointer in
  /// another's class can refer to that one's anonymous referent.
  llvm::EquivalenceClasses<unsigned> AliasClasses;

  /// Append one entity per entry of \p RD's flow-tracked member table
  /// (SemaProfiles::flowTrackedMembers, including the members of its
  /// non-virtual, constructor-less bases, which nothing can have assigned
  /// before the containing object's user code runs), in table order, for the
  /// object \p Base (null: the current object), with the given read-tracking
  /// and entry state. Returns the appended range; a local's range is recorded
  /// only when non-empty.
  std::pair<unsigned, unsigned> addObject(const VarDecl *Base,
                                          const CXXRecordDecl *RD,
                                          bool TrackReads, bool UnknownEntry);

  /// Append a MemberPointee entity for each marked pointer member of the
  /// current object of class \p RD, then the SubMembers of each.
  void addMemberPointerRoots(const CXXRecordDecl *RD) {
    unsigned Begin = Entities.size();
    for (const MemberPath &Path : Profiles.markedPointerMembers(RD)) {
      TrackedEntity E;
      E.K = TrackedEntity::Kind::MemberPointee;
      E.Steps = Path;
      E.Type = Path.back().Field->getType()->getPointeeType();
      E.TrackReads = false;
      Entities.push_back(std::move(E));
    }
    MemberPointerRange = {Begin, (unsigned)Entities.size()};
    MemberPointerClass = RD;
    for (unsigned I = MemberPointerRange.first; I != MemberPointerRange.second;
         ++I)
      addSubMembers(I);
  }
  /// Append the SubMembers of storage entity \p Idx, one per entry of its
  /// class's flow-tracked member table (SemaProfiles::flowTrackedMembers), for
  /// a class whose default-initialization runs no user-provided constructor.
  void addSubMembers(unsigned Idx);
  /// The SubMember of the storage entity \p R that \p K names, if \p R
  /// has the SubMembers of \p K's class or of a class derived from it along
  /// one non-virtual path: a base view of the storage names the derived
  /// class's entry for the member.
  std::optional<unsigned> subEntity(unsigned R, const SubKey &K) const {
    const TrackedEntity &E = Entities[R];
    if (!K.Class || !E.SubClass)
      return std::nullopt;
    if (E.SubClass == K.Class)
      return E.Subs.first + K.Index;
    std::optional<SemaProfiles::BasePath> Path =
        uniqueBasePath(E.SubClass, K.Class);
    ArrayRef<SemaProfiles::FlowTrackedMember> BaseTable =
        Profiles.flowTrackedMembers(K.Class);
    if (!Path || K.Index >= BaseTable.size())
      return std::nullopt;
    MemberPath Steps(BaseTable[K.Index].first);
    BasePath &First = Steps.front().Bases;
    First.insert(First.begin(), Path->begin(), Path->end());
    ArrayRef<SemaProfiles::FlowTrackedMember> Table =
        Profiles.flowTrackedMembers(E.SubClass);
    for (unsigned I = 0, N = Table.size(); I != N; ++I)
      if (Table[I].first == Steps)
        return E.Subs.first + I;
    return std::nullopt;
  }
  /// The SubMember key of the leaf of shape \p Shape below entity \p Idx's
  /// storage.
  SubKey subKey(unsigned Idx, const SemaProfiles::FlowLeafShape &Shape) const {
    const CXXRecordDecl *RD = Entities[Idx].SubClass;
    if (!RD || Shape.Subscript)
      return {};
    std::optional<SemaProfiles::FlowMemberMatch> M =
        Profiles.matchFlowTrackedMember(RD, Shape);
    if (!M)
      return {};
    return {RD, M->Index, M->Length == Shape.Members.size()};
  }

  /// The MemberPointee entity of the pointer member at \p Path, if tracked.
  std::optional<unsigned> memberPointerRoot(ArrayRef<MemberStep> Path) const {
    if (MemberPointerRange.first == MemberPointerRange.second)
      return std::nullopt;
    std::optional<unsigned> I =
        Profiles.findMarkedPointerMember(MemberPointerClass, Path);
    if (!I)
      return std::nullopt;
    return MemberPointerRange.first + *I;
  }

  /// Append the CurrentObject entity of class \p RD.
  void addCurrentObjectRoot(const CXXRecordDecl *RD) {
    TrackedEntity E;
    E.K = TrackedEntity::Kind::CurrentObject;
    E.Type = Ctx.getCanonicalTagType(RD);
    E.TrackReads = false;
    Entities.push_back(std::move(E));
    CurrentObjectRoot = Entities.size() - 1;
  }

  /// Append the WholeLocal or Pointee entity of the marked local or
  /// parameter \p V (once); returns its index.
  unsigned addLocalEntity(TrackedEntity::Kind K, const VarDecl *V,
                          bool UnknownEntry);

  /// The member entity of the object \p Base (null: the current object)
  /// whose member path is \p Steps, if tracked.
  std::optional<unsigned> find(const VarDecl *Base,
                               ArrayRef<MemberStep> Steps) const;

  /// The entity range of the object \p Base (null: the current object) and
  /// its class, or an empty range and null.
  std::pair<std::pair<unsigned, unsigned>, const CXXRecordDecl *>
  objectRange(const VarDecl *Base) const;

  /// What resolving a glvalue found: the tracked entity it names -- Subobject
  /// when it reaches storage below that entity (a member or element of an
  /// [[uninit]] object, a member of a marked pointer's referent, or a
  /// designation that does not cover the entity; see resolve), which a store
  /// does not initialize and a binding judges by the entity's state -- or, for
  /// an access that reaches a tracked aggregate local without naming a tracked
  /// member, the consumed base and whether the access is benign: it cannot
  /// reach a tracked member (an untracked scalar sibling, or a sub-access below
  /// a member whose type holds no pointer, reference, or member pointer), so
  /// the base does not escape.
  struct Resolution {
    std::optional<unsigned> Entity;
    bool Subobject = false;
    const DeclRefExpr *LocalBase = nullptr;
    bool Benign = false;
    /// A pointer-to-data-member step on the current object or a tracked
    /// aggregate local (this->*pm, x.*pm): the member entities it may
    /// access, every one below the object's derived-to-base path. A read of
    /// it reads each of them; a store credits none.
    std::pair<unsigned, unsigned> Opaque{0, 0};
    /// Below a WholeLocal's or Pointee's storage, the SubMember named.
    SubKey Sub;
  };
  /// Resolve the glvalue \p E through SemaProfiles::flowLeafShape: a tracked
  /// member of the current object (this->m, m, (*this).m, this->c.m) or of a
  /// directly named tracked local (V.m, V.c.m, through containers), an
  /// [[uninit]] local or a subobject of it (u, u.x, arr[i]), or a marked local
  /// pointer's, reference's, or handle's referent or a subobject of it (*p,
  /// p->m, r, r.m, *up, up->m; never an element, P4222R2 §5.4), or a marked
  /// pointer member's referent (*elem, mem.elem->x). A sub-access below another
  /// named class or array member of a tracked aggregate (x.in.v, x.arr[0])
  /// names no entity and is benign or an escape by that member's type. The one
  /// resolver behind every event arm, so a leaf affects flow state exactly as
  /// SemaProfiles::isFlowTrackedLeaf says it is tracked. A credit, store, or
  /// access passes its \p Designated type (and \p RawExempt; see
  /// designationCovers): a whole entity reached through a derived-to-base step
  /// or a designation that does not cover it is a subobject. A marked pointer's
  /// or reference's referent is checked against its anonymous referent's type
  /// here and against the storage it refers to by the transfer function
  /// (DefAssignEvent::Designated). With \p Pointee, \p E is a pointer value
  /// and the glvalue resolved is what it points to
  /// (SemaProfiles::flowLeafShape).
  Resolution resolve(const SemaProfiles::ShapeLeaf &Leaf,
                     QualType Designated = QualType(), bool RawExempt = false,
                     bool Pointee = false) const;

  /// Resolve a binding or argument leaf: the glvalue \p E (resolve), or --
  /// with \p AsPointerValue, a leaf used as a pointer value -- the storage
  /// \c &x names, the element pointer arithmetic points to (as \c &p[n]
  /// does), the array a decayed array decays from, or a marked local
  /// pointer's referent (its value). \p Designated and \p RawExempt are
  /// resolve's; for a pointer value, the designated type is the pointee's.
  Resolution resolveFlowLeaf(const SemaProfiles::ShapeLeaf &Leaf,
                             bool AsPointerValue,
                             QualType Designated = QualType(),
                             bool RawExempt = false) const;

  /// True if a designation of type \p Designated covers entity \p Idx whole
  /// (designationCovers); a null \p Designated covers everything.
  bool covers(QualType Designated, unsigned Idx, bool RawExempt) const {
    return Designated.isNull() ||
           designationCovers(Ctx, Designated, Entities[Idx].Type, RawExempt);
  }

  /// The Pointee entity of the marked local pointer \p V (a reference's
  /// referent cannot be reseated and is not one), if tracked.
  std::optional<unsigned> markedPointerObject(const VarDecl *V) const;
  /// The Pointee entity of the marked local handle \p V
  /// (SemaProfiles::isFlowTrackedHandleVar), if tracked.
  std::optional<unsigned> markedHandleObject(const VarDecl *V) const;
  /// The same for the handle object the glvalue \p Leaf directly names
  /// through transparent casts: the object a reseat or an alias binding acts
  /// on.
  std::optional<unsigned>
  markedHandleObject(const SemaProfiles::ShapeLeaf &Leaf) const;
  /// Resolve the Handle leaf \p L of a handle source
  /// (SemaProfiles::forEachHandleSourceLeaf): a tracked handle variable's
  /// referent, a subobject of it when \p L is Partial or \p Designated does
  /// not cover it (designationCovers).
  Resolution resolveHandleLeaf(const SemaProfiles::HandleLeaf &L,
                               QualType Designated = QualType(),
                               bool RawExempt = false) const;
  /// The same for the pointer object the glvalue \p E directly names through
  /// transparent casts -- a marked pointer member of the current object
  /// included (its MemberPointee): the object a store reseats or an alias
  /// binding escapes.
  std::optional<unsigned>
  markedPointerObject(const SemaProfiles::ShapeLeaf &Leaf) const;

  /// The member entities below the subobject \p E names as a whole --
  /// \c this or \c *this, a tracked local, or a member of either enclosing
  /// tracked members -- and then below its base subobject at the
  /// derived-to-base path \p Bases, taking the arms listed in \p Arms; empty
  /// otherwise.
  std::pair<unsigned, unsigned>
  wholeObject(const Expr *E, ArrayRef<const Expr *> Arms = {},
              ArrayRef<const CXXRecordDecl *> Bases = {}) const;

  /// The member entities below the subobject of a tracked local or by-value
  /// parameter the leaf \p Leaf names as a whole -- the object itself, a
  /// base subobject, or a member enclosing tracked members
  /// (SemaProfiles::flowTrackedMemberRange), converted to a base by its own
  /// casts and by \p OuterBases, the derived-to-base steps above the leaf --
  /// or, in a constructor body, of the current object; with
  /// \p AsPointerValue, the leaf is its address. Empty for any other leaf
  /// (SemaProfiles::isFlowTrackedSubobject for \p Use): for a MarkedTarget,
  /// an object whose default-initialization runs code was constructed whole;
  /// a ValueCopy names a member.
  std::pair<unsigned, unsigned>
  objectPart(const SemaProfiles::ShapeLeaf &Leaf, bool AsPointerValue,
             ArrayRef<const CXXRecordDecl *> OuterBases = {},
             SemaProfiles::FlowLeafUse Use =
                 SemaProfiles::FlowLeafUse::MarkedTarget) const;

  /// The entities of the object with entity range \p Range and class \p RD
  /// below its subobject reached through \p Prefix and then \p Bases
  /// (SemaProfiles::flowTrackedMemberRange) -- contiguous, as the
  /// enumeration is depth-first -- or an empty range.
  std::pair<unsigned, unsigned>
  partRange(std::pair<unsigned, unsigned> Range, const CXXRecordDecl *RD,
            ArrayRef<MemberStep> Prefix,
            ArrayRef<const CXXRecordDecl *> Bases) const;
};

std::pair<unsigned, unsigned> TrackedStorage::addObject(const VarDecl *Base,
                                                        const CXXRecordDecl *RD,
                                                        bool TrackReads,
                                                        bool UnknownEntry) {
  unsigned Begin = Entities.size();
  for (const SemaProfiles::FlowTrackedMember &M :
       Profiles.flowTrackedMembers(RD)) {
    TrackedEntity E;
    E.K = Base ? TrackedEntity::Kind::LocalMember
               : TrackedEntity::Kind::CurrentObjectMember;
    E.Steps = M.first;
    E.Whole = M.second;
    E.Type = M.first.back().Field->getType();
    E.TrackReads = TrackReads;
    E.UnknownEntry = UnknownEntry;
    Entities.push_back(std::move(E));
  }
  std::pair<unsigned, unsigned> Range{Begin, (unsigned)Entities.size()};
  if (!Base) {
    CurrentObjectRange = Range;
    CurrentObjectClass = RD;
  } else if (Range.first != Range.second) {
    LocalRange[Base] = Range;
    LocalClass[Base] = RD;
  }
  return Range;
}

unsigned TrackedStorage::addLocalEntity(TrackedEntity::Kind K, const VarDecl *V,
                                        bool UnknownEntry) {
  auto It = LocalEntity.find(V);
  if (It != LocalEntity.end())
    return It->second;
  QualType T = V->getType();
  // A handle's referent is of its value type; with none, of no type.
  if (K == TrackedEntity::Kind::Pointee && SemaProfiles::isHandleType(T)) {
    T = SemaProfiles::handleValueType(T);
    if (T.isNull())
      T = Ctx.VoidTy;
  } else if (K == TrackedEntity::Kind::Pointee) {
    T = T->isPointerType() ? T->getPointeeType() : T.getNonReferenceType();
  }
  TrackedEntity E;
  E.K = K;
  E.Type = T;
  E.TrackReads = false;
  E.UnknownEntry = UnknownEntry;
  unsigned Idx = Entities.size();
  Entities.push_back(std::move(E));
  addSubMembers(Idx);
  return LocalEntity[V] = Idx;
}

void TrackedStorage::addSubMembers(unsigned Idx) {
  const CXXRecordDecl *RD =
      SemaProfiles::getDefaultInitTrackableClass(Entities[Idx].Type);
  if (!RD)
    return;
  ArrayRef<SemaProfiles::FlowTrackedMember> Table =
      Profiles.flowTrackedMembers(RD);
  if (Table.empty())
    return;
  unsigned Begin = Entities.size();
  Entities[Idx].Subs = {Begin, Begin + (unsigned)Table.size()};
  Entities[Idx].SubClass = RD;
  bool UnknownEntry = Entities[Idx].UnknownEntry;
  for (const SemaProfiles::FlowTrackedMember &M : Table) {
    TrackedEntity Sub;
    Sub.K = TrackedEntity::Kind::SubMember;
    Sub.Steps = M.first;
    Sub.Whole = M.second;
    Sub.Type = M.first.back().Field->getType();
    Sub.TrackReads = false;
    Sub.UnknownEntry = UnknownEntry;
    Entities.push_back(std::move(Sub));
  }
}

std::pair<std::pair<unsigned, unsigned>, const CXXRecordDecl *>
TrackedStorage::objectRange(const VarDecl *Base) const {
  if (!Base)
    return {CurrentObjectRange, CurrentObjectClass};
  auto It = LocalRange.find(Base);
  if (It == LocalRange.end())
    return {{0, 0}, nullptr};
  return {It->second, LocalClass.lookup(Base)};
}

std::optional<unsigned> TrackedStorage::find(const VarDecl *Base,
                                             ArrayRef<MemberStep> Steps) const {
  std::pair<unsigned, unsigned> Range = objectRange(Base).first;
  for (unsigned I = Range.first; I != Range.second; ++I)
    if (Entities[I].Steps == Steps)
      return I;
  return std::nullopt;
}

std::pair<unsigned, unsigned>
TrackedStorage::wholeObject(const Expr *E, ArrayRef<const Expr *> Arms,
                            ArrayRef<const CXXRecordDecl *> Bases) const {
  E = SemaProfiles::ignoreTransparentCasts(E, Arms);
  if (isCurrentObjectBase(E))
    return partRange(CurrentObjectRange, CurrentObjectClass, {}, Bases);
  std::optional<SemaProfiles::FlowLeafShape> Shape =
      SemaProfiles::flowLeafShape(E, Arms);
  if (!Shape || Shape->Subscript)
    return {0, 0};
  if (Shape->IsThis)
    return partRange(CurrentObjectRange, CurrentObjectClass, Shape->Members,
                     Bases);
  if (Shape->ThroughPointer)
    return {0, 0};
  auto [Range, RD] = objectRange(Shape->Local);
  return partRange(Range, RD, Shape->Members, Bases);
}

std::pair<unsigned, unsigned>
TrackedStorage::objectPart(const SemaProfiles::ShapeLeaf &Leaf,
                           bool AsPointerValue,
                           ArrayRef<const CXXRecordDecl *> OuterBases,
                           SemaProfiles::FlowLeafUse Use) const {
  BasePath Above(OuterBases.begin(), OuterBases.end());
  const Expr *E = Leaf.E;
  if (AsPointerValue) {
    E = SemaProfiles::addressOfOperand(SemaProfiles::ignoreTransparentCasts(
        SemaProfiles::peelBaseCasts(E, Above, Leaf.Arms), Leaf.Arms));
    if (!E)
      return {0, 0};
  }
  E = SemaProfiles::ignoreTransparentCasts(E, Leaf.Arms);
  if (Use == SemaProfiles::FlowLeafUse::MarkedTarget) {
    const CXXRecordDecl *Named = E->getType()->getAsCXXRecordDecl();
    if (!Named || !Named->hasDefinition() ||
        !Named->hasTrivialDefaultConstructor())
      return {0, 0};
  }
  std::optional<SemaProfiles::FlowLeafShape> Shape =
      SemaProfiles::flowLeafShape(E, Leaf.Arms);
  if (!Shape || Shape->Subscript ||
      (Use == SemaProfiles::FlowLeafUse::ValueCopy && Shape->Members.empty()))
    return {0, 0};
  BasePath Bases(Shape->LeafBases);
  Bases.append(Above.begin(), Above.end());
  if (Shape->IsThis) {
    // A constructor reads the current object's members from the unassigned
    // entry state.
    if (!Shape->PointerMembers.empty() || Shape->Members.empty() ||
        CurrentObjectRange.first == CurrentObjectRange.second ||
        !Entities[CurrentObjectRange.first].TrackReads)
      return {0, 0};
    return partRange(CurrentObjectRange, CurrentObjectClass, Shape->Members,
                     Bases);
  }
  if (Shape->ThroughPointer)
    return {0, 0};
  auto [Range, RD] = objectRange(Shape->Local);
  return partRange(Range, RD, Shape->Members, Bases);
}

std::pair<unsigned, unsigned>
TrackedStorage::partRange(std::pair<unsigned, unsigned> Range,
                          const CXXRecordDecl *RD, ArrayRef<MemberStep> Prefix,
                          ArrayRef<const CXXRecordDecl *> Bases) const {
  if (Range.first == Range.second)
    return {0, 0};
  std::pair<unsigned, unsigned> Part =
      Profiles.flowTrackedMemberRange(RD, Prefix, Bases);
  if (Part.first == Part.second)
    return {0, 0};
  return {Range.first + Part.first, Range.first + Part.second};
}

TrackedStorage::Resolution
TrackedStorage::resolve(const SemaProfiles::ShapeLeaf &Leaf,
                        QualType Designated, bool RawExempt,
                        bool Pointee) const {
  std::optional<SemaProfiles::FlowLeafShape> Shape =
      SemaProfiles::flowLeafShape(Leaf.E, Leaf.Arms, Pointee);
  if (!Shape)
    return {};
  // A designation of a whole entity names a subobject when it takes a
  // derived-to-base step or does not cover the entity's type.
  auto DesignatesPart = [&](unsigned Idx) {
    return !Designated.isNull() &&
           (!Shape->LeafBases.empty() || !covers(Designated, Idx, RawExempt));
  };
  Resolution R;
  // The member entity of the object with entity range \p Range and class
  // \p RD whose path the leaf's steps start with
  // (SemaProfiles::matchFlowTrackedMember); storage below it is a subobject.
  auto ResolveMember = [&](std::pair<unsigned, unsigned> Range,
                           const CXXRecordDecl *RD) {
    std::optional<SemaProfiles::FlowMemberMatch> M =
        Profiles.matchFlowTrackedMember(RD, *Shape);
    if (!M)
      return false;
    assert(Range.second - Range.first ==
               Profiles.flowTrackedMembers(RD).size() &&
           "an object's entities are its class's member table");
    unsigned Idx = Range.first + M->Index;
    R.Entity = Idx;
    R.Subobject = Shape->Members.size() > M->Length || Shape->Subscript ||
                  DesignatesPart(Idx);
    return true;
  };
  // The first pointer-to-data-member step, if any.
  ArrayRef<MemberStep> Steps = Shape->Members;
  unsigned OpaqueStep =
      llvm::find_if(Steps, [](const MemberStep &S) { return !S.Field; }) -
      Steps.begin();
  // The members a step through a pointer to data member may access on the
  // object with entity range \p Range and class \p RD: every tracked member
  // below the object it applies to -- for a last step only, unless
  // \p AnyDepth.
  auto OpaqueRange = [&](std::pair<unsigned, unsigned> Range,
                         const CXXRecordDecl *RD, bool AnyDepth = false) {
    if (!AnyDepth && (OpaqueStep + 1 != Steps.size() || Shape->Subscript))
      return std::pair{0u, 0u};
    return partRange(Range, RD, Steps.take_front(OpaqueStep),
                     Steps[OpaqueStep].Bases);
  };
  if (Shape->IsThis && !Shape->PointerMembers.empty()) {
    // *elem, elem->m: a marked pointer member's referent or a subobject of
    // it; never an element.
    if (Shape->Subscript)
      return {};
    if (std::optional<unsigned> Idx =
            memberPointerRoot(Shape->PointerMembers)) {
      R.Entity = Idx;
      R.Subobject = !Steps.empty() || DesignatesPart(*Idx);
      R.Sub = subKey(*Idx, *Shape);
    }
    return R;
  }
  if (Shape->IsThis) {
    // *this: the current object as a whole, when its lifetime is tracked.
    if (Steps.empty() && !Shape->Subscript && CurrentObjectRoot) {
      R.Entity = CurrentObjectRoot;
      R.Subobject = DesignatesPart(*CurrentObjectRoot);
      return R;
    }
    if (Steps.empty() || ResolveMember(CurrentObjectRange, CurrentObjectClass))
      return R;
    // An access at any depth below the step may reach any of them.
    if (OpaqueStep != Steps.size())
      R.Opaque = OpaqueRange(CurrentObjectRange, CurrentObjectClass,
                             /*AnyDepth=*/true);
    return R;
  }
  const VarDecl *V = Shape->Local;
  // *up, up->m: a marked handle's referent or a subobject of it; never an
  // element (s[i], or through an offset handle), as for a pointer.
  if (Shape->ThroughHandle) {
    if (Shape->Subscript)
      return {};
    if (std::optional<unsigned> Idx = markedHandleObject(V)) {
      R.Entity = Idx;
      R.Subobject = !Shape->Members.empty() || DesignatesPart(*Idx);
      R.Sub = subKey(*Idx, *Shape);
    }
    return R;
  }
  // *p, p->m: a marked pointer's referent or a subobject of it.
  if (Shape->ThroughPointer) {
    if (Shape->Subscript)
      return {};
    if (std::optional<unsigned> Idx = markedPointerObject(V)) {
      R.Entity = Idx;
      R.Subobject = !Shape->Members.empty() || DesignatesPart(*Idx);
      R.Sub = subKey(*Idx, *Shape);
    }
    return R;
  }
  if (auto It = LocalEntity.find(V); It != LocalEntity.end()) {
    // u, u.x, arr[i]: an [[uninit]] local as a whole or a subobject of it;
    // r, r.m: a marked reference's referent or a subobject of it.
    const TrackedEntity &Ent = Entities[It->second];
    if (Ent.K == TrackedEntity::Kind::WholeLocal) {
      R.Entity = It->second;
      R.Subobject = !Shape->Members.empty() || Shape->Subscript ||
                    DesignatesPart(It->second);
      R.Sub = subKey(It->second, *Shape);
    } else if (Ent.K == TrackedEntity::Kind::Pointee &&
               V->getType()->isReferenceType() && !Shape->Subscript) {
      R.Entity = It->second;
      R.Subobject = !Shape->Members.empty() || DesignatesPart(It->second);
      R.Sub = subKey(It->second, *Shape);
    }
    return R;
  }
  // V.m: a member of a tracked aggregate local, or a consuming access of one.
  auto [Range, RD] = objectRange(V);
  if (Range.first == Range.second || Steps.empty())
    return {};
  R.LocalBase = Shape->LocalRef;
  if (ResolveMember(Range, RD)) {
    R.Benign = true;
    return R;
  }
  // x.*pm, x.m.*pm: any member below the object it applies to, modeled by its
  // reads; below such a step, its type is unknown here, so the access escapes
  // the object.
  if (OpaqueStep != Steps.size()) {
    std::pair<unsigned, unsigned> Opaque = OpaqueRange(Range, RD);
    if (OpaqueStep == 0 || Opaque.first != Opaque.second) {
      R.Opaque = Opaque;
      R.Benign = OpaqueStep + 1 == Steps.size() && !Shape->Subscript;
      return R;
    }
  }
  // The member directly on the object.
  const MemberStep &Top = Steps.front();
  // An untracked sibling consumes the base without exposing the object
  // only when it cannot itself reach a tracked member: a pointer or
  // reference member may denote one, and a class or array member may hold
  // such a value.
  QualType FT = Top.Field->getType();
  if (Steps.size() == 1 && !Shape->Subscript) {
    R.Benign = FT->isIntegralOrEnumerationType() || FT->isFloatingType();
    return R;
  }
  llvm::SmallPtrSet<const RecordDecl *, 8> Visited;
  R.Benign = !typeHoldsPointerOrReference(FT, Visited);
  return R;
}

TrackedStorage::Resolution
TrackedStorage::resolveFlowLeaf(const SemaProfiles::ShapeLeaf &Leaf,
                                bool AsPointerValue, QualType Designated,
                                bool RawExempt) const {
  if (!AsPointerValue)
    return resolve(Leaf, Designated, RawExempt);
  const Expr *E = SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms);
  if (const Expr *Op = SemaProfiles::addressOfOperand(E))
    return resolve({Op, Leaf.Arms}, Designated, RawExempt);
  if (SemaProfiles::pointerArithmeticBase(E))
    return resolve({E, Leaf.Arms}, Designated, RawExempt, /*Pointee=*/true);
  // A decayed array is the array it decays from.
  if (E->getType()->isArrayType())
    return resolve({E, Leaf.Arms}, Designated, RawExempt);
  Resolution R;
  if (isa<CXXThisExpr>(E) && CurrentObjectRoot) {
    R.Entity = CurrentObjectRoot;
    R.Subobject = !covers(Designated, *CurrentObjectRoot, RawExempt);
    return R;
  }
  // A Derive call: its argument, as an array or range's storage, a pointer's
  // value, or a range's handle referent; a subobject when it offsets.
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    bool Partial = false;
    if (const Expr *Arg = SemaProfiles::derivedArgument(CE, Partial)) {
      QualType AT = Arg->getType();
      if (AT->isPointerType()) {
        R = resolveFlowLeaf({Arg, Leaf.Arms}, /*AsPointerValue=*/true,
                            Designated, RawExempt);
      } else {
        R = resolve({Arg, Leaf.Arms}, Designated, RawExempt);
        bool ChainPartial = false;
        if (!R.Entity && SemaProfiles::isHandleType(AT))
          if (const DeclRefExpr *Base =
                  SemaProfiles::handleChainBase(Arg, Leaf.Arms, ChainPartial))
            if (std::optional<unsigned> Idx =
                    markedHandleObject(dyn_cast<VarDecl>(Base->getDecl()))) {
              R.Entity = Idx;
              R.Subobject =
                  ChainPartial || !covers(Designated, *Idx, RawExempt);
            }
      }
      if (R.Entity && Partial) {
        R.Subobject = true;
        R.Sub = SubKey();
      }
      return R;
    }
  }
  // A std::array's begin or data: the array it points into -- through a
  // pointer, the pointer's referent -- a subobject from its end.
  bool PastStart;
  if (const Expr *Arr = SemaProfiles::stdArrayAddressObject(E, PastStart)) {
    R = Arr->getType()->isPointerType()
            ? resolveFlowLeaf({Arr, Leaf.Arms}, /*AsPointerValue=*/true,
                              Designated, RawExempt)
            : resolve({Arr, Leaf.Arms}, Designated, RawExempt);
    R.Subobject |= PastStart;
    return R;
  }
  // A pointer a marked handle's member returns to its whole referent, or
  // through a prefix of it to a subobject; an element's, or one past the
  // referent's start, names no entity.
  const Expr *Obj;
  bool Element;
  if (E->getType()->isPointerType() &&
      SemaProfiles::isHandleDerefCall(E, Obj, Element)) {
    bool Partial = false, PastStart = false;
    if (const DeclRefExpr *Base =
            SemaProfiles::handleChainBase(Obj, Leaf.Arms, Partial, &PastStart);
        Base && !Element && !PastStart)
      if (std::optional<unsigned> Idx =
              markedHandleObject(dyn_cast<VarDecl>(Base->getDecl()))) {
        R.Entity = Idx;
        R.Subobject = Partial || !covers(Designated, *Idx, RawExempt);
      }
    return R;
  }
  if (std::optional<unsigned> Idx = markedPointerObject({E, Leaf.Arms})) {
    R.Entity = Idx;
    R.Subobject = !covers(Designated, *Idx, RawExempt);
  }
  return R;
}

std::optional<unsigned>
TrackedStorage::markedPointerObject(const VarDecl *V) const {
  auto It = LocalEntity.find(V);
  if (It == LocalEntity.end() ||
      Entities[It->second].K != TrackedEntity::Kind::Pointee ||
      !V->getType()->isPointerType())
    return std::nullopt;
  return It->second;
}

std::optional<unsigned>
TrackedStorage::markedHandleObject(const VarDecl *V) const {
  auto It = LocalEntity.find(V);
  if (It == LocalEntity.end() ||
      Entities[It->second].K != TrackedEntity::Kind::Pointee ||
      !SemaProfiles::isHandleType(V->getType()))
    return std::nullopt;
  return It->second;
}

std::optional<unsigned>
TrackedStorage::markedHandleObject(const SemaProfiles::ShapeLeaf &Leaf) const {
  const auto *V = dyn_cast_or_null<VarDecl>(SemaProfiles::getDirectlyNamedDecl(
      SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms)));
  return V ? markedHandleObject(V) : std::nullopt;
}

TrackedStorage::Resolution
TrackedStorage::resolveHandleLeaf(const SemaProfiles::HandleLeaf &L,
                                  QualType Designated, bool RawExempt) const {
  Resolution R;
  if (L.M != SemaProfiles::HandleLeaf::Mode::Handle)
    return R;
  const auto *DRE = dyn_cast<DeclRefExpr>(L.E->IgnoreParenImpCasts());
  const auto *V = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
  if (std::optional<unsigned> Idx = V ? markedHandleObject(V) : std::nullopt) {
    R.Entity = Idx;
    R.Subobject = L.Partial || !covers(Designated, *Idx, RawExempt);
  }
  return R;
}

std::optional<unsigned>
TrackedStorage::markedPointerObject(const SemaProfiles::ShapeLeaf &Leaf) const {
  const Expr *E = SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms);
  // A marked pointer member of the current object.
  if (isa<MemberExpr>(E) && E->getType()->isPointerType()) {
    std::optional<SemaProfiles::FlowLeafShape> Shape =
        SemaProfiles::flowLeafShape(E, Leaf.Arms);
    if (!Shape || !Shape->IsThis || !Shape->PointerMembers.empty() ||
        Shape->Subscript || !Shape->LeafBases.empty())
      return std::nullopt;
    return memberPointerRoot(Shape->Members);
  }
  const auto *V =
      dyn_cast_or_null<VarDecl>(SemaProfiles::getDirectlyNamedDecl(E));
  return V ? markedPointerObject(V) : std::nullopt;
}

/// Per-block ordered events recovered from the CFG by the std::init dataflow
/// engine and replayed over a FlowState in element order. Read: a value load of
/// entity Idx (read-tracked members). Write: Idx is assigned. ReadWrite: a
/// compound assignment or built-in ++/--, a Read then a Write. MayWrite: a
/// store whose target names Idx on one of several arms. Copy: Idx takes entity
/// Aux's state (the tracked-copy transfer). Kill: a storage release (free,
/// realloc, operator delete, a delete-expression) leaves Idx unassigned.
/// Destroy: a [[now_uninit]] destruction, a destructor call, or a scope-exit
/// destructor (DestroySites[Aux]), judged by the destroy rules; its storage is
/// then unassigned and destroyed until stored again -- for an argument naming
/// several storages, each may be. Reseat: the marked pointer, reference, or
/// handle Idx refers, from here, to the storage DefAssignEvent::RT describes
/// and, when a marked binding names an identified referent (Entity,
/// ViaPointer), asserts it unassigned: Must and May cleared, MayUnassigned set,
/// MayDestroyed and Esc kept. Escape: Idx may have been destroyed or released
/// by one of several arms or an opaque callee: possibly assigned or not, and
/// not destroyed. Reset: Idx belongs to a local whose declaration runs here,
/// which begins a new object each time (a loop's next iteration, a backward
/// goto): unassigned, not escaped, and not destroyed. AggregateEscape: the
/// tracked aggregate local owning member Idx escapes as a whole (a non-benign
/// DeclRefExpr), so reads of the member are trusted from here (the local read
/// leniency) while its assignment state is unchanged. Binding: a pointer,
/// reference, or handle binding whose source has a tracked leaf
/// (BindingSites[Aux]), judged by the binding rule. ReadThrough /
/// SubobjectWrite: a read through, or a store below, tracked storage
/// (AccessSites[Aux]), judged by uninit_read / uninit_write. An event on a
/// Pointee entity acts on the storage the pointer refers to at its program
/// point (referent). Every event but a Reset and a scope-exit Destroy carries
/// the anchor expression the diagnostic and the suppression walk use. A Reset
/// or AggregateEscape whose Aux is above Idx acts on each entity of the range
/// [Idx, Aux).
enum class DefAssignEventKind {
  Read,
  Write,
  ReadWrite,
  MayWrite,
  Copy,
  Kill,
  Destroy,
  Reseat,
  Escape,
  Reset,
  AggregateEscape,
  Binding,
  ReadThrough,
  SubobjectWrite
};
/// What a Reseat makes its marked pointer, reference, or handle refer to.
enum class ReseatTarget {
  /// Its own anonymous referent, whose state is reset to unassigned: the
  /// source is one the analysis does not track (an allocation, pointer
  /// arithmetic, a null pointer, a decayed array, a subobject, a conditional
  /// with no tracked arm).
  Fresh,
  /// Its own anonymous referent in unknown state: a mutable alias of the
  /// pointer object was handed out (P4222R2 §4.3).
  Escaped,
  /// The entity DefAssignEvent::Aux (`&u`, `&a.m`, `&m`, `u` for a
  /// reference), which the binding's marker asserts uninitialized.
  Entity,
  /// What the marked pointer, reference, or handle DefAssignEvent::Aux refers
  /// to there (q, &*q, &r, *q for a reference), which the binding's
  /// marker asserts uninitialized; an unidentified referent asserts nothing.
  ViaPointer,
  /// Not identified: a conditional source with a tracked arm, or a store
  /// whose target names several pointers or a marked pointer on only some
  /// of its arms.
  Unknown,
  /// The same referent, from a position past its start (a handle's ++, --,
  /// +=, -=, std::advance): a credit or destroy through it reaches part of
  /// the referent (FlowState::Offset).
  Offset,
  /// Nothing: a std::unique_ptr moved from, released, reset, assigned null,
  /// or constructed null. The referent it had stays where a move or release
  /// handed it, and its destroys destroy nothing (FlowState::Null).
  Null
};
struct DefAssignEvent {
  DefAssignEventKind Kind;
  unsigned Idx;
  const Expr *E;
  /// Copy: the source entity. Reseat: the entity or pointer RT names.
  /// Binding: the BindingSite index. Destroy: the DestroySite index.
  /// ReadThrough and SubobjectWrite: the AccessSite index. Reset and
  /// AggregateEscape: the end of the entity range, when above Idx.
  unsigned Aux = 0;
  /// Reseat: what Idx refers to from here.
  ReseatTarget RT = ReseatTarget::Fresh;
  /// Write and MayWrite: the credit's or store's designated type (null: no
  /// type check) and its RawExempt (designationCovers). A Pointee's write
  /// reaches storage its marked pointer or reference refers to only when the
  /// designation covers that storage.
  QualType Designated;
  bool RawExempt = false;
  /// Write and MayWrite: the SubMember of the storage Idx refers to that the
  /// event writes instead of the storage. Otherwise an event on a storage
  /// entity also applies to its SubMembers -- except, for a
  /// SkipUninitMembers write (a default-initialization's credit), the
  /// writes.
  SubKey Sub;
  bool SkipUninitMembers = false;
};

/// The form classification of a binding leaf: the parse-time recognizers'
/// verdict for a leaf no entity tracks, or the flow state's for a tracked
/// one.
enum class LeafState { Initialized, Uninitialized, Unknown, Mixed };

/// Combine two leaves' states as the recognizers combine conditional arms:
/// equal states keep; Mixed absorbs; Initialized with Uninitialized is
/// Mixed (P4222R2 §4.9); Unknown yields to Uninitialized and absorbs
/// Initialized.
static LeafState combineLeafStates(LeafState A, LeafState B) {
  if (A == B)
    return A;
  if (A == LeafState::Mixed || B == LeafState::Mixed)
    return LeafState::Mixed;
  if (A == LeafState::Unknown)
    return B == LeafState::Uninitialized ? B : LeafState::Unknown;
  if (B == LeafState::Unknown)
    return A == LeafState::Uninitialized ? A : LeafState::Unknown;
  return LeafState::Mixed;
}

/// One leaf of a binding source: the tracked entity it names, judged by that
/// entity's flow state, or, for a leaf no entity tracks, its form
/// classification.
struct BindingLeaf {
  std::optional<unsigned> Entity;
  LeafState State = LeafState::Unknown;
  /// The member entities of a leaf naming a tracked aggregate, or a
  /// subobject enclosing some of its tracked members, as a whole (the
  /// marked-target variant of SemaProfiles::isFlowTrackedLeaf); empty
  /// otherwise.
  std::pair<unsigned, unsigned> Range{0, 0};
  /// The SubMember the leaf names below Entity's storage, and whether the
  /// leaf names that storage whole (leafState).
  SubKey Sub;
  bool WholeLeaf = false;
  /// A store site's whole-referent leaf of a marked pointer or reference: the
  /// store's designated type. The store is the referent's initialization when
  /// the designation covers the storage referred to there, and a subobject
  /// store otherwise.
  QualType WholeIf;
};

/// A ref_to_uninit binding site derived from a CFG element: the construct's
/// kind and wording, the target's marking, the bound type's reference-ness,
/// and the source's leaves.
struct BindingSite {
  SemaProfiles::InitBindingKind Kind;
  SourceLocation Loc;
  bool TargetMarked;
  bool IsReference;
  /// The entity a kind-specific wording names (the called method, the
  /// captured variable).
  const NamedDecl *Subject = nullptr;
  SmallVector<BindingLeaf, 2> Leaves;
  /// The source parameter of a copy or move constructor or assignment
  /// operator (SemaProfiles::isValueCopySource): an unmarked one's copy reads
  /// the source whole, so it must be initialized on every path (P4222R2 §1.3,
  /// §9.2), and is trusted below a member that is not read-tracked.
  bool EveryPath = false;
  /// The bound type when it is a handle (SemaProfiles::isHandleType), which
  /// the diagnostics name; null for a pointer or reference.
  QualType HandleType;
};

/// A destruction site derived from a call element -- a [[now_uninit]] or
/// destroy-row argument, an explicit or pseudo-destructor call's object, or the
/// operand of a delete-expression that runs a destructor -- or from the
/// scope-exit destructor of an [[uninit]] local, and the destruction of a
/// marked std::unique_ptr's referent by its scope exit, reset, reassignment, or
/// destructor call: the argument's leaves (a tracked entity, judged by its flow
/// state, or a leaf no entity tracks, classified by form), the entities the
/// destruction affects, and whether it is exempt from destroy_uninit -- a
/// reinitializer (both lifecycle attributes: destroy-then-construct on fresh
/// storage is its purpose, and the exemption is call-wide), a parameter that
/// itself carries [[ref_to_uninit]] (the marker declares that the parameter
/// takes possibly-uninitialized storage: the annotation spelling for an
/// unrecognized storage-release function), or an owner's referent of a
/// trivially destructible or unknown type. The rules are applied only when a
/// leaf names tracked storage (Judged), the parse-time funnel having judged
/// every other destroy; the transfer applies either way.
struct DestroySite {
  SourceLocation Loc;
  bool Exempt = false;
  bool Judged = false;
  /// What ends the lifetime, for the diagnostics' wording.
  SemaProfiles::InitDestroyer Destroyer = SemaProfiles::InitDestroyer::Function;
  /// A scope-exit destructor's variable: the suppression gate is its
  /// declaration, and a note points there.
  const VarDecl *ScopeExitVar = nullptr;
  /// The argument's arms name different storage: each may have been
  /// destroyed, none definitely.
  bool Conditional = false;
  /// The marked std::unique_ptr whose destruction, reset, or reassignment
  /// destroys its referent: the site is skipped while it is null
  /// (FlowState::Null).
  std::optional<unsigned> Owner;
  SmallVector<BindingLeaf, 2> Leaves;
  /// The entities destroyed: the whole entity each leaf names, or every
  /// entity of an object destroyed as a whole (`this`, `*this`, `&x`, `x`).
  SmallVector<unsigned, 2> Affected;
};

/// A read through, or a store below, tracked storage (P4222R2 §4.5, §5.4)
/// derived from a load, assignment, or increment element: the leaves that
/// resolve to the referent of a marked pointer or reference (`*p`, `p->m`,
/// `r`, `r.m`) or to a subobject of an [[uninit]] local (`u.x`, `arr[i]`),
/// judged by their flow state, and the leaves no entity tracks, classified
/// by form. A whole [[uninit]] local's value read is the flow-based local
/// pass's and a tracked member's the Read event's, so such leaves count as
/// initialized here, and a store to a whole entity is a Write, not a
/// subobject write.
struct AccessSite {
  SourceLocation Loc;
  /// The diagnostic's provenance select, fixed by the leaves' spelling: for
  /// a read, 1 when every judged leaf is a subobject of an [[uninit]]
  /// object, 0 when one is reached through a marked pointer or reference;
  /// for a store, 0 for the [[uninit]] object, 1 for the marked pointer or
  /// reference.
  unsigned Select = 0;
  /// A store's "not a member access" flag (an element).
  bool Flag = false;
  SmallVector<BindingLeaf, 2> Leaves;
};

#define DEBUG_TYPE "std-init-flow"
/// The events extracted from the analyzed bodies (DefAssignEvent).
STATISTIC(NumFlowEvents, "Number of std::init flow events extracted");
/// The leaves two flow states' joins combine cell by cell (SharedArray::meet).
STATISTIC(NumFlowStateLeafJoins,
          "Number of std::init flow-state leaves joined cell by cell");
/// The trie nodes the flow states allocate (SharedArray).
STATISTIC(NumFlowStateNodes, "Number of std::init flow-state nodes allocated");
/// The pointers a reseat to a fresh or escaped referent checks for an alias
/// of the referent it replaces (TrackedStorage::AliasClasses).
STATISTIC(NumReseatAliasChecks,
          "Number of std::init pointers checked for a replaced referent");
#undef DEBUG_TYPE

/// FlowState::Target of a marked pointer or reference whose referent is not
/// identified: paths bind it differently, or the anonymous referent it
/// copied from another pointer was replaced. Nothing fires through such a
/// pointer in either direction, a store through it credits nothing, and a
/// marked binding to it asserts nothing.
static constexpr unsigned UnknownTarget = ~1u;
/// The stored FlowState::Target of a marked pointer or reference that refers
/// to its own anonymous referent: one value for every entity, so the initial
/// targets share their nodes.
static constexpr unsigned SelfTarget = ~2u;

/// A fixed-size array of \p T whose copies share structure: a 64-ary trie of
/// reference-counted nodes, copied on write. A copy costs one pointer and a
/// write the nodes on one path; a comparison or a join of two arrays visits
/// only the nodes they do not share.
template <typename T> class SharedArray {
  /// The index bits one level of the trie consumes.
  static constexpr unsigned Shift = 6;
  /// The children of an inner node, and the cells of a leaf.
  static constexpr unsigned Fan = 1u << Shift;

  /// A trie node; its destructor is virtual so that RefCountedBase deletes
  /// either kind.
  struct Node : llvm::RefCountedBase<Node> {
    Node() { ++NumFlowStateNodes; }
    Node(const Node &) : Node() {}
    virtual ~Node() = default;
  };
  using NodePtr = llvm::IntrusiveRefCntPtr<Node>;
  /// The cells of Fan consecutive indices.
  struct Leaf final : Node {
    std::array<T, Fan> Cells;
  };
  /// The subtries of Fan consecutive index ranges.
  struct Inner final : Node {
    std::array<NodePtr, Fan> Kids;
  };

  /// The root; null for a default-constructed array.
  NodePtr Root;
  /// The number of inner levels above the leaves: at most 5 for 32-bit
  /// indices, which bounds every recursion below.
  unsigned Height = 0;

  /// The node \p N as a leaf.
  static Leaf &leaf(Node *N) { return *static_cast<Leaf *>(N); }
  /// The node \p N as an inner node.
  static Inner &inner(Node *N) { return *static_cast<Inner *>(N); }
  /// The child of an inner node at level \p Level whose subtrie holds the
  /// index \p I.
  static unsigned slot(unsigned I, unsigned Level) {
    return (I >> (Shift * Level)) & (Fan - 1);
  }
  /// Make the node \p P points to, at level \p Level, unshared.
  static void own(NodePtr &P, unsigned Level) {
    if (P->UseCount() == 1)
      return;
    if (Level == 0)
      P = NodePtr(new Leaf(leaf(P.get())));
    else
      P = NodePtr(new Inner(inner(P.get())));
  }
  /// The join of the subtries \p A and \p B at level \p Level, cell by cell
  /// with \p Fn: shared subtries are the join's own, and a node equal to an
  /// operand's is that operand's.
  template <typename F>
  static NodePtr meetNode(const NodePtr &A, const NodePtr &B, unsigned Level,
                          F &Fn) {
    if (A == B)
      return A;
    bool SameA = true, SameB = true;
    if (Level == 0) {
      ++NumFlowStateLeafJoins;
      const Leaf &LA = leaf(A.get()), &LB = leaf(B.get());
      std::array<T, Fan> Cells;
      for (unsigned I = 0; I != Fan; ++I) {
        Cells[I] = Fn(LA.Cells[I], LB.Cells[I]);
        SameA &= Cells[I] == LA.Cells[I];
        SameB &= Cells[I] == LB.Cells[I];
      }
      if (SameA)
        return A;
      if (SameB)
        return B;
      auto *N = new Leaf();
      N->Cells = Cells;
      return NodePtr(N);
    }
    const Inner &IA = inner(A.get()), &IB = inner(B.get());
    std::array<NodePtr, Fan> Kids;
    for (unsigned I = 0; I != Fan; ++I) {
      Kids[I] = meetNode(IA.Kids[I], IB.Kids[I], Level - 1, Fn);
      SameA &= Kids[I] == IA.Kids[I];
      SameB &= Kids[I] == IB.Kids[I];
    }
    if (SameA)
      return A;
    if (SameB)
      return B;
    auto *N = new Inner();
    N->Kids = std::move(Kids);
    return NodePtr(N);
  }
  /// True if the subtries \p A and \p B at level \p Level hold equal cells.
  static bool equalNode(const NodePtr &A, const NodePtr &B, unsigned Level) {
    if (A == B)
      return true;
    if (Level == 0)
      return leaf(A.get()).Cells == leaf(B.get()).Cells;
    for (unsigned I = 0; I != Fan; ++I)
      if (!equalNode(inner(A.get()).Kids[I], inner(B.get()).Kids[I], Level - 1))
        return false;
    return true;
  }

public:
  /// An array without nodes, which only an assignment gives cells.
  SharedArray() = default;
  /// \p N copies of \p Fill, in one node per level.
  SharedArray(unsigned N, T Fill) {
    auto *L = new Leaf();
    L->Cells.fill(Fill);
    Root = NodePtr(L);
    for (uint64_t Cap = Fan; Cap < N; Cap <<= Shift, ++Height) {
      auto *I = new Inner();
      I->Kids.fill(Root);
      Root = NodePtr(I);
    }
  }
  /// The cell \p I.
  T get(unsigned I) const {
    Node *N = Root.get();
    for (unsigned L = Height; L; --L)
      N = inner(N).Kids[slot(I, L)].get();
    return leaf(N).Cells[I & (Fan - 1)];
  }
  /// Store \p V in the cell \p I, copying the shared nodes on its path; a
  /// store of the value already there copies nothing.
  void set(unsigned I, T V) {
    if (get(I) == V)
      return;
    own(Root, Height);
    NodePtr *P = &Root;
    for (unsigned L = Height; L; --L) {
      P = &inner(P->get()).Kids[slot(I, L)];
      own(*P, L - 1);
    }
    leaf(P->get()).Cells[I & (Fan - 1)] = V;
  }
  /// Join the array \p O, of the same size, into this one cell by cell with
  /// \p Fn.
  template <typename F> void meet(const SharedArray &O, F Fn) {
    Root = meetNode(Root, O.Root, Height, Fn);
  }
  bool operator==(const SharedArray &O) const {
    return equalNode(Root, O.Root, Height);
  }
};

/// The dataflow state of every tracked entity at a program point -- one word
/// of flags per entity (Bit) and, for a Pointee entity, the entity the marked
/// pointer or reference refers to (Target) -- or the identity of the join:
/// the state of a predecessor not yet reached, which has no nodes. The join
/// intersects the JoinIntersects flags, unites the others, and keeps a Target
/// the predecessors agree on.
struct FlowState {
  /// The per-entity flags.
  enum Bit : uint16_t {
    /// Assigned on every path.
    Must = 1u << 0,
    /// Assigned on some path.
    May = 1u << 1,
    /// Unassigned on some path that knows its state: a path entering from an
    /// enclosing function, or past a mutable alias of a marked pointer, does
    /// not.
    MayUnassigned = 1u << 2,
    /// Escaped as a local aggregate on every path: the read leniency for
    /// locals (ProfilesFrameworkInternals.rst, "Known Gaps (std::init)").
    Esc = 1u << 3,
    /// Destroyed on some path and not stored since.
    MayDestroyed = 1u << 4,
    /// Uninitialized on some path only by a marked pointer member's entry
    /// assertion: a MemberPointee's own referent at entry, until a store,
    /// credit, destroy, release, or reseat gives evidence.
    EntryAsserted = 1u << 5,
    /// Storage whose latest credit, on some path, was a
    /// default-initialization (DefAssignEvent::SkipUninitMembers): its
    /// [[uninit]] members it has no SubMembers for are indeterminate until a
    /// store names one.
    MembersPending = 1u << 6,
    /// A marked handle positioned past its referent's start on some path
    /// (ReseatTarget::Offset).
    Offset = 1u << 7,
    /// A marked std::unique_ptr null on every path (ReseatTarget::Null).
    Null = 1u << 8,
    /// A marked std::unique_ptr null on some path: its destroys are no
    /// double_destroy, as the nulling path destroys nothing there.
    MayNull = 1u << 9,
  };
  /// The flags the join intersects; it unites the others.
  static constexpr uint16_t JoinIntersects = Must | Esc | Null;

  /// False for the identity of the join.
  bool Reached = false;
  /// The flags of each entity.
  SharedArray<uint16_t> Bits;
  /// Per entity, meaningful for a Pointee: SelfTarget when the pointer refers
  /// to an anonymous referent whose state is the entity's flags, another
  /// entity's index when it refers to that entity's storage (an [[uninit]]
  /// local, a tracked member, or another pointer's anonymous referent), or
  /// UnknownTarget.
  SharedArray<unsigned> Target;

  /// The identity of the join.
  FlowState() = default;
  /// Every one of \p N entities unassigned (MayUnassigned only), and every
  /// marked pointer or reference referring to its own anonymous referent.
  explicit FlowState(unsigned N)
      : Reached(true), Bits(N, MayUnassigned), Target(N, SelfTarget) {}

  /// True if entity \p I has the flag \p B.
  bool has(unsigned I, Bit B) const { return Bits.get(I) & B; }
  /// The flags of entity \p I.
  uint16_t bits(unsigned I) const { return Bits.get(I); }
  /// Give entity \p I the flags \p Set and clear its other flags of \p Clear.
  void update(unsigned I, uint16_t Set, uint16_t Clear = 0) {
    Bits.set(I, uint16_t((Bits.get(I) & ~Clear) | Set));
  }
  /// What the Pointee entity \p I refers to (Target).
  unsigned target(unsigned I) const {
    unsigned T = Target.get(I);
    return T == SelfTarget ? I : T;
  }
  /// Make the Pointee entity \p I refer to \p T.
  void setTarget(unsigned I, unsigned T) {
    Target.set(I, T == I ? SelfTarget : T);
  }
  /// Join the predecessor state \p O into this one: an unreached state is
  /// skipped, and a reached one copied into an unreached one.
  void meet(const FlowState &O) {
    if (!O.Reached)
      return;
    if (!Reached) {
      *this = O;
      return;
    }
    Bits.meet(O.Bits, [](uint16_t A, uint16_t B) -> uint16_t {
      return ((A & B) & JoinIntersects) | ((A | B) & ~JoinIntersects);
    });
    Target.meet(O.Target, [](unsigned A, unsigned B) {
      return A == B ? A : UnknownTarget;
    });
  }
  bool operator==(const FlowState &O) const {
    return Reached == O.Reached &&
           (!Reached || (Bits == O.Bits && Target == O.Target));
  }
  bool operator!=(const FlowState &O) const { return !(*this == O); }
};

/// A violation the reporting replay collected, emitted through the shared
/// gate once the replay is complete.
struct PendingViolation {
  unsigned DiagID;
  SourceLocation Loc;
  /// Binding: the pointer/reference/handle select; ReadThrough and
  /// SubobjectWrite: the provenance select; Destroy: the destroyer
  /// (SemaProfiles::InitDestroyer).
  unsigned Select = 0;
  /// SubobjectWrite: the "not a member access" flag.
  bool Flag = false;
  /// The entity a kind-specific binding wording names; null for the generic
  /// wordings.
  const NamedDecl *Subject = nullptr;
  /// A scope-exit destructor's variable (DestroySite::ScopeExitVar).
  const VarDecl *ScopeExitVar = nullptr;
  /// Binding: the handle type the handle select names
  /// (BindingSite::HandleType).
  QualType HandleType;
};

/// The entity the event or leaf index \p Idx denotes under \p St: itself for
/// a non-Pointee entity, the storage a marked pointer or reference refers to
/// there for a Pointee one, or none when that referent is not identified.
static std::optional<unsigned> referent(unsigned Idx, const FlowState &St,
                                        const TrackedStorage &Storage) {
  if (!Storage.Entities[Idx].isPointee())
    return Idx;
  unsigned T = St.target(Idx);
  if (T == UnknownTarget)
    return std::nullopt;
  return T;
}

/// The state of one binding leaf under \p St: its entity's referent's bits,
/// Unknown when the referent is not identified. Storage assigned on some
/// path but not every one is Uninitialized for a consumer that requires
/// initialization on \p EveryPath (P4222R2 §1.3) when a path that knows its
/// state leaves it unassigned, and Unknown otherwise.
static LeafState leafState(const BindingLeaf &L, const FlowState &St,
                           const TrackedStorage &Storage, bool EveryPath,
                           bool WithSubs = true) {
  // An object or subobject named whole is initialized when every member is,
  // and uninitialized when none may be -- for \p EveryPath, a value copy,
  // when a read-tracked member may be unassigned.
  if (L.Range.first != L.Range.second) {
    bool AllMust = true, AnyMay = false, AnyUnassigned = false;
    for (unsigned I = L.Range.first; I != L.Range.second; ++I) {
      bool Must = St.has(I, FlowState::Must) ||
                  (EveryPath && !Storage.Entities[I].TrackReads);
      AllMust &= Must;
      AnyMay |= St.has(I, FlowState::May);
      AnyUnassigned |= !Must && St.has(I, FlowState::MayUnassigned);
    }
    if (AllMust)
      return LeafState::Initialized;
    if (EveryPath)
      return AnyUnassigned ? LeafState::Uninitialized : LeafState::Unknown;
    return AnyMay ? LeafState::Unknown : LeafState::Uninitialized;
  }
  if (!L.Entity)
    return L.State;
  std::optional<unsigned> R = referent(*L.Entity, St, Storage);
  if (!R)
    return LeafState::Unknown;
  auto StateOf = [&](unsigned I) {
    if (St.has(I, FlowState::Must))
      return LeafState::Initialized;
    if (EveryPath ? St.has(I, FlowState::MayUnassigned)
                  : !St.has(I, FlowState::May))
      return LeafState::Uninitialized;
    return LeafState::Unknown;
  };
  // The storage with its SubMembers takes the most uninitialized state
  // among them: whole, all of them; below the storage, the SubMember the
  // leaf names.
  auto Worse = [](LeafState A, LeafState B) {
    if (A == LeafState::Uninitialized || B == LeafState::Uninitialized)
      return LeafState::Uninitialized;
    return A == LeafState::Unknown || B == LeafState::Unknown
               ? LeafState::Unknown
               : LeafState::Initialized;
  };
  LeafState S = StateOf(*R);
  if (!WithSubs)
    return S;
  LeafState StorageState = S;
  const TrackedEntity &E = Storage.Entities[*R];
  if (L.WholeLeaf) {
    for (unsigned I = E.Subs.first; I != E.Subs.second; ++I)
      S = Worse(S, StateOf(I));
  } else if (std::optional<unsigned> Sub = Storage.subEntity(*R, L.Sub)) {
    S = Worse(S, StateOf(*Sub));
  } else if (L.Sub.Class && St.has(*R, FlowState::MembersPending)) {
    // An [[uninit]] member the storage has no SubMember for, which a
    // default-initialization left indeterminate.
    S = Worse(S, LeafState::Uninitialized);
  }
  // A plain binding of live storage whose SubMembers alone are unassigned
  // binds an object, not raw memory -- unless it names one SubMember exactly.
  if (!EveryPath && StorageState == LeafState::Initialized &&
      S == LeafState::Uninitialized && (L.WholeLeaf || !L.Sub.Exact))
    return LeafState::Unknown;
  return S;
}

/// Judge a binding site against \p St with the funnel's verdict rule: a
/// marked target rejects a source that is initialized (on every path) or
/// Mixed, an unmarked one a source that is uninitialized (on every path) or
/// Mixed -- for a whole copy, a source left uninitialized on some path
/// (BindingSite::EveryPath); an Unknown source fires in neither direction.
static void judgeBindingSite(const BindingSite &Site, const FlowState &St,
                             const TrackedStorage &Storage,
                             SmallVectorImpl<PendingViolation> &Out) {
  using K = SemaProfiles::InitBindingKind;
  if (Site.Leaves.empty())
    return;
  bool EveryPath = Site.EveryPath && !Site.TargetMarked;
  // A value copy of a member's storage is trusted where its reads are.
  auto State = [&](const BindingLeaf &L) {
    if (EveryPath && L.Entity) {
      const TrackedEntity &Ent = Storage.Entities[*L.Entity];
      if (Ent.field() && !Ent.TrackReads)
        return LeafState::Initialized;
    }
    // A marked target is judged by the storage alone -- a default-initialized
    // object is alive whatever its SubMembers' state -- unless it names one
    // SubMember exactly, which is judged by that member's own state.
    return leafState(L, St, Storage, EveryPath,
                     /*WithSubs=*/!Site.TargetMarked ||
                         (!L.WholeLeaf && L.Sub.Exact));
  };
  LeafState Acc = State(Site.Leaves.front());
  for (const BindingLeaf &L : llvm::drop_begin(Site.Leaves))
    Acc = combineLeafStates(Acc, State(L));
  // The shape select of the binding diagnostics: pointer, reference, handle.
  unsigned Shape = Site.IsReference ? 1 : !Site.HandleType.isNull() ? 2 : 0;
  if (Site.TargetMarked) {
    if (Acc == LeafState::Initialized || Acc == LeafState::Mixed) {
      PendingViolation V{diag::err_init_ref_to_uninit_requires_uninit, Site.Loc,
                         Shape};
      V.HandleType = Site.HandleType;
      Out.push_back(V);
    }
    return;
  }
  if (Acc != LeafState::Uninitialized && Acc != LeafState::Mixed)
    return;
  unsigned DiagID = diag::err_init_uninit_requires_ref_to_uninit;
  const NamedDecl *Subject = nullptr;
  if (Site.Kind == K::ByRefCapture) {
    DiagID = diag::err_init_uninit_ref_capture;
    Subject = Site.Subject;
  } else if (Site.Kind == K::ObjectArgument) {
    DiagID = diag::err_init_member_call_on_uninit;
    Subject = Site.Subject;
  }
  PendingViolation V{DiagID, Site.Loc, Shape, false, Subject};
  V.HandleType = Site.HandleType;
  Out.push_back(V);
}

/// Judge a destroy site against \p St. Storage destroyed on some path and not
/// stored since (every leaf's entity MayDestroyed; a leaf no entity tracks is
/// never destroyed) is double_destroy, since that path destroys it twice
/// (P4222R2 §1, §9.2), whatever the exemption -- a reinitializer's destroy
/// half is as invalid on destroyed storage as anyone else's, and construct_at
/// is the sanctioned recovery path. Otherwise a non-exempt
/// destroy of storage uninitialized on every path, or of a Mixed combination
/// of arms (P4222R2 §4.9), is destroy_uninit: destruction makes an object
/// uninitialized, so a first destroy of never-constructed storage is as much
/// an access to raw memory as a second one (P4222R2 §4.4).
static void judgeDestroySite(const DestroySite &Site, const FlowState &St,
                             const TrackedStorage &Storage,
                             SmallVectorImpl<PendingViolation> &Out) {
  bool AllDestroyed = true;
  std::optional<LeafState> Acc;
  for (const BindingLeaf &L : Site.Leaves) {
    // Through an offset handle the destroy reaches an element, which is not
    // judged (SemaProfiles::designatesReferentElement).
    if (L.Entity && St.has(*L.Entity, FlowState::Offset))
      continue;
    if (L.Range.first != L.Range.second) {
      for (unsigned I = L.Range.first; I != L.Range.second; ++I)
        AllDestroyed &= St.has(I, FlowState::MayDestroyed);
    } else {
      std::optional<unsigned> R =
          L.Entity ? referent(*L.Entity, St, Storage) : std::nullopt;
      // At its scope exit, a member of a local that escaped after the
      // member's destroy may have been reconstructed since. (An explicit
      // destroy's own argument escapes the local just before the destroy.)
      AllDestroyed &=
          R && St.has(*R, FlowState::MayDestroyed) &&
          !(Site.Destroyer == SemaProfiles::InitDestroyer::ScopeExit &&
            St.has(*R, FlowState::Esc));
      // A marked pointer member's entry assertion alone is no evidence that
      // a destroy takes uninitialized storage, nor is the state of a member
      // of an escaped local.
      if (R && (St.has(*R, FlowState::EntryAsserted) ||
                St.has(*R, FlowState::Esc))) {
        Acc = Acc ? combineLeafStates(*Acc, LeafState::Unknown)
                  : LeafState::Unknown;
        continue;
      }
    }
    LeafState S =
        leafState(L, St, Storage, /*EveryPath=*/false, /*WithSubs=*/false);
    Acc = Acc ? combineLeafStates(*Acc, S) : S;
  }
  // An owner null on some path destroys nothing on it (FlowState::MayNull).
  if (Site.Owner && St.has(*Site.Owner, FlowState::MayNull))
    AllDestroyed = false;
  if (!Acc && AllDestroyed)
    return;
  auto Push = [&](unsigned DiagID) {
    PendingViolation V{DiagID, Site.Loc, static_cast<unsigned>(Site.Destroyer)};
    V.ScopeExitVar = Site.ScopeExitVar;
    Out.push_back(V);
  };
  if (AllDestroyed) {
    Push(diag::err_init_double_destroy);
    return;
  }
  if (Acc && !Site.Exempt &&
      (*Acc == LeafState::Uninitialized || *Acc == LeafState::Mixed))
    Push(diag::err_init_destroy_uninit);
}

/// Judge an access site against \p St: uninit_read (\p IsWrite false) or
/// uninit_write fires when a leaf is uninitialized -- a tracked leaf on some
/// path (P4222R2 §1.3: every path must initialize it), a form-classified leaf
/// affirmatively or as a Mixed combination (P4222R2 §4.9) -- exactly as the
/// recognizers' combination of conditional arms fires on any uninitialized
/// arm.
static void judgeAccessSite(const AccessSite &Site, bool IsWrite,
                            const FlowState &St, const TrackedStorage &Storage,
                            SmallVectorImpl<PendingViolation> &Out) {
  bool Fires = false;
  for (const BindingLeaf &L : Site.Leaves) {
    if (!L.WholeIf.isNull()) {
      std::optional<unsigned> R = referent(*L.Entity, St, Storage);
      if (!R || *R == *L.Entity ||
          Storage.covers(L.WholeIf, *R, /*RawExempt=*/false))
        continue;
    }
    // Below a local's member tracked whole, an escape of the local trusts the
    // access, as it trusts a read of a scalar member (Read).
    if (L.Entity && Storage.Entities[*L.Entity].Whole &&
        Storage.Entities[*L.Entity].K == TrackedEntity::Kind::LocalMember &&
        St.has(*L.Entity, FlowState::Esc))
      continue;
    // A store below the storage is judged by the storage alone; a read
    // needs the SubMember it reads too.
    LeafState S = leafState(L, St, Storage, /*EveryPath=*/true,
                            /*WithSubs=*/!IsWrite);
    Fires |= S == LeafState::Uninitialized || S == LeafState::Mixed;
  }
  if (!Fires)
    return;
  if (IsWrite)
    Out.push_back({diag::err_init_uninit_subobject_write, Site.Loc, Site.Select,
                   Site.Flag});
  else
    Out.push_back({diag::err_init_uninit_read_through, Site.Loc, Site.Select});
}

/// Replay a block's events over \p St: the engine's one block-level transfer
/// function, shared by the fixpoint and the reporting replay so the two can
/// never disagree. An event or site leaf on a Pointee entity is resolved to
/// the pointer's referent under \p St first (referent) and skipped when that
/// is not identified; Read, ReadWrite, Copy, and AggregateEscape only ever
/// carry member entities. With \p Offending and \p Violations non-null (the
/// reporting replay), a read of a read-tracked entity that is not definitely
/// assigned nor escaped is collected, and every judged event's verdict is
/// taken against the state at its program point.
static void
applyDefAssignEvents(ArrayRef<DefAssignEvent> BlockEvents,
                     ArrayRef<BindingSite> Sites, ArrayRef<DestroySite> DSites,
                     ArrayRef<AccessSite> ASites, const TrackedStorage &Storage,
                     FlowState &St,
                     std::vector<SmallVector<const Expr *, 2>> *Offending,
                     SmallVectorImpl<PendingViolation> *Violations) {
  // Apply \p F to the storage entity \p R and to its SubMembers.
  auto WithSubs = [&](unsigned R, auto F) {
    F(R);
    for (unsigned I = Storage.Entities[R].Subs.first,
                  N = Storage.Entities[R].Subs.second;
         I != N; ++I)
      F(I);
  };
  auto Write = [&](unsigned I) {
    St.update(I, FlowState::Must | FlowState::May,
              FlowState::MayUnassigned | FlowState::MayDestroyed |
                  FlowState::EntryAsserted | FlowState::MembersPending);
  };
  // The storage a Write or MayWrite reaches: Idx's referent, when the
  // designation covers it (a Pointee's own anonymous referent was checked at
  // extraction) -- or, for an event naming a SubMember, that member of the
  // referent when the designation covers it.
  auto WriteTarget = [&](const DefAssignEvent &Ev) -> std::optional<unsigned> {
    std::optional<unsigned> R = referent(Ev.Idx, St, Storage);
    if (R && Ev.Sub.Class) {
      std::optional<unsigned> S = Storage.subEntity(*R, Ev.Sub);
      if (!S || !Ev.Sub.Exact ||
          !Storage.covers(Ev.Designated, *S, Ev.RawExempt))
        return std::nullopt;
      return S;
    }
    if (R && *R != Ev.Idx && !Storage.covers(Ev.Designated, *R, Ev.RawExempt))
      return std::nullopt;
    // A marked pointer to a base class reaches only the base subobject of a
    // referent of a derived class.
    if (R && *R != Ev.Idx)
      if (const CXXRecordDecl *PRD =
              Storage.Entities[Ev.Idx].Type->getAsCXXRecordDecl())
        if (const CXXRecordDecl *RRD =
                Storage.Entities[*R].Type->getAsCXXRecordDecl();
            RRD && RRD->hasDefinition() && RRD->isDerivedFrom(PRD))
          return std::nullopt;
    return R;
  };
  // Write \p R and, unless the event is a default-initialization's credit or
  // names a SubMember, the SubMembers of its storage.
  auto WriteStorage = [&](const DefAssignEvent &Ev, unsigned R,
                          llvm::function_ref<void(unsigned)> F) {
    if (Ev.Sub.Class || Ev.SkipUninitMembers)
      F(R);
    else
      WithSubs(R, F);
  };
  auto MayWrite = [&](unsigned I) {
    St.update(I, FlowState::May, FlowState::EntryAsserted);
  };
  for (const DefAssignEvent &Ev : BlockEvents) {
    switch (Ev.Kind) {
    case DefAssignEventKind::Read:
      if (Offending && !St.has(Ev.Idx, FlowState::Must) &&
          !St.has(Ev.Idx, FlowState::Esc))
        (*Offending)[Ev.Idx].push_back(Ev.E);
      break;
    case DefAssignEventKind::Write:
      if (std::optional<unsigned> R = WriteTarget(Ev)) {
        // Through an offset handle a credit reaches part of the referent.
        if (St.has(Ev.Idx, FlowState::Offset)) {
          WriteStorage(Ev, *R, MayWrite);
          break;
        }
        WriteStorage(Ev, *R, Write);
        if (Ev.SkipUninitMembers)
          St.update(*R, FlowState::MembersPending);
      } else if (Ev.Sub.Class && Ev.Sub.Exact) {
        // A store naming one member of storage with no SubMember for it (a
        // void referent viewed as a class) ends its pending members.
        if (std::optional<unsigned> R = referent(Ev.Idx, St, Storage);
            R && !Storage.subEntity(*R, Ev.Sub))
          St.update(*R, 0, FlowState::MembersPending);
      }
      break;
    case DefAssignEventKind::ReadWrite:
      if (Offending && !St.has(Ev.Idx, FlowState::Must) &&
          !St.has(Ev.Idx, FlowState::Esc))
        (*Offending)[Ev.Idx].push_back(Ev.E);
      Write(Ev.Idx);
      break;
    case DefAssignEventKind::MayWrite:
      if (std::optional<unsigned> R = WriteTarget(Ev))
        WriteStorage(Ev, *R, MayWrite);
      break;
    case DefAssignEventKind::Copy: {
      constexpr uint16_t Copied =
          FlowState::Must | FlowState::May | FlowState::MayUnassigned |
          FlowState::Esc | FlowState::MayDestroyed | FlowState::EntryAsserted |
          FlowState::MembersPending;
      St.update(Ev.Idx, St.bits(Ev.Aux) & Copied, Copied);
      break;
    }
    case DefAssignEventKind::Kill:
      if (std::optional<unsigned> R = referent(Ev.Idx, St, Storage))
        WithSubs(*R, [&](unsigned I) {
          St.update(I, FlowState::MayUnassigned,
                    FlowState::Must | FlowState::May | FlowState::Esc |
                        FlowState::EntryAsserted | FlowState::MembersPending);
        });
      break;
    case DefAssignEventKind::Destroy: {
      const DestroySite &Site = DSites[Ev.Aux];
      if (Site.Owner && St.has(*Site.Owner, FlowState::Null))
        break;
      if (Violations && Site.Judged)
        judgeDestroySite(Site, St, Storage, *Violations);
      for (unsigned A : Site.Affected) {
        std::optional<unsigned> R = referent(A, St, Storage);
        if (!R)
          continue;
        // Through an offset handle a destroy reaches an element, which
        // destroys nothing tracked.
        if (St.has(A, FlowState::Offset))
          continue;
        uint16_t Set = FlowState::MayUnassigned | FlowState::MayDestroyed;
        uint16_t Clear = FlowState::Must | FlowState::EntryAsserted |
                         FlowState::MembersPending;
        if (Site.Conditional)
          Set |= FlowState::May;
        else
          Clear |= FlowState::May | FlowState::Esc;
        WithSubs(*R, [&](unsigned I) { St.update(I, Set, Clear); });
      }
      break;
    }
    case DefAssignEventKind::Reseat: {
      unsigned NewTarget = Ev.Idx;
      switch (Ev.RT) {
      case ReseatTarget::Fresh:
      case ReseatTarget::Escaped:
        // The entity's flags describe the new anonymous referent from here,
        // so a pointer that copied this one while it referred to the old one
        // loses its referent; only one in its alias class can have.
        for (unsigned K : Storage.AliasClasses.members(Ev.Idx)) {
          if (K == Ev.Idx)
            continue;
          ++NumReseatAliasChecks;
          if (St.target(K) == Ev.Idx)
            St.setTarget(K, UnknownTarget);
        }
        WithSubs(Ev.Idx, [&](unsigned I) {
          St.update(I,
                    Ev.RT == ReseatTarget::Escaped ? FlowState::May
                                                   : FlowState::MayUnassigned,
                    FlowState::Must | FlowState::May |
                        FlowState::MayUnassigned | FlowState::MayDestroyed |
                        FlowState::EntryAsserted | FlowState::MembersPending);
        });
        break;
      case ReseatTarget::Entity:
        NewTarget = Ev.Aux;
        break;
      case ReseatTarget::ViaPointer:
        NewTarget = St.target(Ev.Aux);
        break;
      case ReseatTarget::Unknown:
        NewTarget = UnknownTarget;
        break;
      case ReseatTarget::Offset:
      case ReseatTarget::Null:
        NewTarget = St.target(Ev.Idx);
        break;
      }
      // The binding's marker asserts the referent uninitialized; see
      // ReseatTarget.
      if ((Ev.RT == ReseatTarget::Entity ||
           Ev.RT == ReseatTarget::ViaPointer) &&
          NewTarget != UnknownTarget)
        WithSubs(NewTarget, [&](unsigned I) {
          St.update(I, FlowState::MayUnassigned,
                    FlowState::Must | FlowState::May |
                        FlowState::MembersPending);
        });
      St.setTarget(Ev.Idx, NewTarget);
      // A copy of an offset handle is offset too.
      uint16_t Own = 0;
      if (Ev.RT == ReseatTarget::Offset || (Ev.RT == ReseatTarget::ViaPointer &&
                                            St.has(Ev.Aux, FlowState::Offset)))
        Own |= FlowState::Offset;
      if (Ev.RT == ReseatTarget::Null)
        Own |= FlowState::Null | FlowState::MayNull;
      St.update(Ev.Idx, Own,
                FlowState::Offset | FlowState::Null | FlowState::MayNull);
      break;
    }
    case DefAssignEventKind::Escape:
      if (std::optional<unsigned> R = referent(Ev.Idx, St, Storage))
        WithSubs(*R, [&](unsigned I) {
          St.update(I, FlowState::May | FlowState::MayUnassigned,
                    FlowState::Must | FlowState::MayDestroyed |
                        FlowState::EntryAsserted | FlowState::MembersPending);
        });
      break;
    case DefAssignEventKind::Reset:
      for (unsigned R = Ev.Idx, E = std::max(Ev.Idx + 1, Ev.Aux); R != E; ++R)
        WithSubs(R, [&](unsigned I) {
          St.update(I, FlowState::MayUnassigned,
                    FlowState::Must | FlowState::May | FlowState::Esc |
                        FlowState::MayDestroyed | FlowState::EntryAsserted |
                        FlowState::MembersPending);
        });
      break;
    case DefAssignEventKind::AggregateEscape:
      for (unsigned I = Ev.Idx, E = std::max(Ev.Idx + 1, Ev.Aux); I != E; ++I)
        St.update(I, FlowState::Esc);
      break;
    case DefAssignEventKind::Binding:
      if (Violations)
        judgeBindingSite(Sites[Ev.Aux], St, Storage, *Violations);
      break;
    case DefAssignEventKind::ReadThrough:
    case DefAssignEventKind::SubobjectWrite:
      if (Violations)
        judgeAccessSite(ASites[Ev.Aux],
                        Ev.Kind == DefAssignEventKind::SubobjectWrite, St,
                        Storage, *Violations);
      break;
    }
  }
}

/// The forward dataflow of the std::init engine: nothing is assigned at
/// function entry (an entity of an enclosing function is may-assigned and in no
/// known state), a block's entry is the join of its predecessors' exits (an
/// entity is definitely assigned at a point only if assigned on every incoming
/// path, the paper's all-branches rule, P4222R2 §1.3), and a block's transfer
/// is the event replay above -- the same replay the reporting pass uses. Every
/// transfer is monotone in each lattice (Read, MayWrite, and AggregateEscape
/// only set bits, Write and ReadWrite set Must and May and clear MayUnassigned
/// and MayDestroyed, Copy projects the source's bits, Kill, Destroy, Reseat,
/// Escape, and Reset are constant functions of their bits -- a Reseat's
/// assertion clears its referent's Must and May like Kill), so a block's exit
/// only ever descends from the join identity in a finite lattice and the
/// iteration terminates. A pointer's Target lattice is flat, UnknownTarget its
/// top, and meet only moves toward UnknownTarget; a Reseat is a constant
/// function of its ReseatTarget, ViaPointer a projection of another pointer's
/// Target, and the invalidation of a replaced anonymous referent's aliases
/// maps one value to UnknownTarget, so the same argument covers Target. The
/// join identity is the unreached state (FlowState::Reached), which the join
/// skips; unprocessed (unreachable) predecessors keep it, so unreachable code
/// is never flagged. Returns, per
/// read-tracked entity, the reads at program points where the entity is not
/// definitely assigned nor escaped, and appends every other rule's violations
/// to \p Violations.
static std::vector<SmallVector<const Expr *, 2>>
runDefiniteAssignment(CFG &cfg, AnalysisDeclContext &AC,
                      const TrackedStorage &Storage,
                      ArrayRef<SmallVector<DefAssignEvent, 4>> Events,
                      ArrayRef<BindingSite> Sites, ArrayRef<DestroySite> DSites,
                      ArrayRef<AccessSite> ASites,
                      SmallVectorImpl<PendingViolation> &Violations) {
  const unsigned NumTracked = Storage.Entities.size();
  const unsigned NumBlocks = cfg.getNumBlockIDs();
  std::vector<FlowState> EntryState(NumBlocks);
  std::vector<FlowState> ExitState(NumBlocks);
  FlowState FunctionEntry(NumTracked);
  for (unsigned I = 0; I != NumTracked; ++I) {
    if (Storage.Entities[I].UnknownEntry)
      FunctionEntry.update(I, FlowState::May, FlowState::MayUnassigned);
    else if (Storage.Entities[I].K == TrackedEntity::Kind::CurrentObject)
      FunctionEntry.update(I, FlowState::Must | FlowState::May,
                           FlowState::MayUnassigned);
    else if (Storage.Entities[I].K == TrackedEntity::Kind::MemberPointee)
      FunctionEntry.update(I, FlowState::EntryAsserted);
  }
  const CFGBlock &CFGEntry = cfg.getEntry();
  ForwardDataflowWorklist Worklist(cfg, AC);
  Worklist.enqueueBlock(&CFGEntry);
  llvm::BitVector Visited(NumBlocks, false);
  while (const CFGBlock *B = Worklist.dequeue()) {
    FlowState In;
    if (B == &CFGEntry) {
      In = FunctionEntry;
    } else {
      // Join the reached predecessors' exits as a balanced tree, the way a
      // binary counter carries: neighbouring predecessors share all but
      // their own changes, so a join at rank R combines states that differ
      // in about 2^R nodes, and at most one partial join per rank is alive.
      SmallVector<std::pair<FlowState, unsigned>, 8> Ranks;
      for (const CFGBlock *Pred : B->preds()) {
        if (!Pred || !ExitState[Pred->getBlockID()].Reached)
          continue;
        FlowState Cur = ExitState[Pred->getBlockID()];
        unsigned Rank = 0;
        for (; !Ranks.empty() && Ranks.back().second == Rank; ++Rank) {
          Cur.meet(Ranks.back().first);
          Ranks.pop_back();
        }
        Ranks.push_back({std::move(Cur), Rank});
      }
      for (; !Ranks.empty(); Ranks.pop_back())
        In.meet(Ranks.back().first);
    }
    assert(In.Reached && "a dequeued block has a reached predecessor");
    EntryState[B->getBlockID()] = In;
    applyDefAssignEvents(Events[B->getBlockID()], Sites, DSites, ASites,
                         Storage, In, /*Offending=*/nullptr,
                         /*Violations=*/nullptr);
    // Enqueue-skip subtlety: a block whose computed exit *equals* its stored
    // exit enqueues no successors. ExitState starts as the unreached state,
    // which no computed exit equals (FlowState::Reached), so a block's first
    // visit always propagates; the FirstVisit disjunct below still defines
    // Visited, exactly the blocks reachable from the entry. Unreachable blocks
    // are never enqueued (only successors of dequeued blocks are), so they
    // keep the unreached state: an unreachable destroy spoils no reachable
    // join. The reporting replay below walks only the visited blocks: a
    // destroy followed by a read inside an unreachable block would otherwise
    // flag unreachable code. Seeding ExitState or EntryState differently, or
    // replaying unvisited blocks, would each break the others' assumption;
    // change them together or not at all.
    bool FirstVisit = !Visited[B->getBlockID()];
    Visited[B->getBlockID()] = true;
    if (FirstVisit || In != ExitState[B->getBlockID()]) {
      ExitState[B->getBlockID()] = In;
      Worklist.enqueueSuccessors(B);
    }
  }

  // Replay each visited (reachable) block from its fixpoint entry state and
  // collect the reads of a not-yet-assigned entity and every judged event's
  // verdict at its program point.
  std::vector<SmallVector<const Expr *, 2>> Offending(NumTracked);
  for (const CFGBlock *B : cfg) {
    if (!Visited[B->getBlockID()])
      continue;
    FlowState St = EntryState[B->getBlockID()];
    applyDefAssignEvents(Events[B->getBlockID()], Sites, DSites, ASites,
                         Storage, St, &Offending, &Violations);
  }
  return Offending;
}

/// Report at the first offending read (in source order) that is not
/// suppressed, once per read-tracked entity, mirroring the local-variable
/// reporter. The profile name and rule arrive from the std::init CFGProfiles
/// row.
static void reportMemberReadsBeforeInit(
    Sema &S, MutableArrayRef<SmallVector<const Expr *, 2>> Offending,
    const TrackedStorage &Storage, StringRef Name) {
  for (unsigned I = 0, N = Offending.size(); I != N; ++I) {
    if (Offending[I].empty())
      continue;
    llvm::sort(Offending[I], [&](const Expr *A, const Expr *B) {
      return S.SourceMgr.isBeforeInTranslationUnit(A->getBeginLoc(),
                                                   B->getBeginLoc());
    });
    for (const Expr *R : Offending[I]) {
      if (!S.Profiles().shouldEmitProfileViolation(
              Name, diag::err_init_member_read_before_init, R->getBeginLoc(),
              /*D=*/nullptr, /*PostParse=*/true))
        continue;
      const FieldDecl *F = Storage.Entities[I].field();
      S.Diag(R->getBeginLoc(), diag::err_init_member_read_before_init)
          << Name << F->getDeclName();
      S.Diag(F->getLocation(), diag::note_init_uninit_member_here)
          << F->getDeclName();
      break;
    }
  }
}

/// Emit the collected binding, destroy, read-through, and subobject-write
/// violations in source order, each through the shared gate at its own
/// location.
static void reportFlowViolations(Sema &S,
                                 MutableArrayRef<PendingViolation> Violations,
                                 StringRef Name) {
  llvm::stable_sort(
      Violations, [&](const PendingViolation &A, const PendingViolation &B) {
        return S.SourceMgr.isBeforeInTranslationUnit(A.Loc, B.Loc);
      });
  for (const PendingViolation &V : Violations) {
    // A scope-exit destructor is gated at its variable's declaration.
    SourceLocation GateLoc =
        V.ScopeExitVar ? V.ScopeExitVar->getLocation() : V.Loc;
    if (!S.Profiles().shouldEmitProfileViolation(Name, V.DiagID, GateLoc,
                                                 /*D=*/nullptr,
                                                 /*PostParse=*/true))
      continue;
    if (V.Subject)
      S.Diag(V.Loc, V.DiagID) << Name << V.Subject;
    else if (V.DiagID == diag::err_init_uninit_subobject_write)
      S.Diag(V.Loc, V.DiagID) << Name << V.Flag << V.Select;
    else if (!V.HandleType.isNull())
      S.Diag(V.Loc, V.DiagID) << Name << V.Select << V.HandleType;
    else
      S.Diag(V.Loc, V.DiagID) << Name << V.Select;
    if (V.ScopeExitVar)
      S.Diag(V.ScopeExitVar->getLocation(), diag::note_init_scope_exit_destroy)
          << V.ScopeExitVar;
  }
}

/// The recognizers' form verdict on a binding leaf as the engine's leaf
/// state.
static LeafState formState(SemaProfiles::InitSourceState S) {
  switch (S) {
  case SemaProfiles::InitSourceState::Initialized:
    return LeafState::Initialized;
  case SemaProfiles::InitSourceState::Uninitialized:
    return LeafState::Uninitialized;
  case SemaProfiles::InitSourceState::Unknown:
    return LeafState::Unknown;
  case SemaProfiles::InitSourceState::Mixed:
    return LeafState::Mixed;
  }
  llvm_unreachable("unknown InitSourceState");
}

/// Zip the arguments of the call-like expression \p CallLike with its callee's
/// parameters and hand each pair to \p F: the parameter (null when no
/// declaration carries it), the type the argument is bound as, the argument,
/// and whether the argument is variadic (bound as its own type). A CallExpr
/// skips a member operator's object argument (an explicit-object member
/// declares it as parameter 0) and takes the parameter types from the
/// prototype of a call with no declared callee (through a function pointer or
/// a pointer to member function); a CXXConstructExpr zips with its
/// constructor; a CXXNewExpr zips its placement arguments with its
/// allocation function's parameters after the implicit size, type-identity,
/// and alignment ones; an ObjCMessageExpr zips with its method, a variadic
/// method's extra arguments binding nothing (they are default-promoted, never
/// copy-initialized). A defaulted argument, judged at its declaration, is
/// skipped. A builtin with custom type checking binds nothing except
/// __builtin_operator_new / __builtin_operator_delete, whose arguments bind
/// type-only parameters, and a known callee's role parameter (va_start's
/// va_list), which binds its declared parameter; a builtin that never evaluates
/// its arguments binds nothing. The one parameter walk behind every argument
/// arm of extractStdInitEvents and appendLifecycleCallEvents.
static void forEachArgumentBinding(
    const ASTContext &Ctx, const Expr *CallLike,
    llvm::function_ref<void(const ParmVarDecl *Parm, QualType ParamTy,
                            const Expr *Arg, bool Variadic)>
        F) {
  // Zip \p Args with \p Params from parameter \p First on; an argument past
  // the last parameter is variadic.
  auto Zip = [&](ArrayRef<ParmVarDecl *> Params, unsigned First, auto &&Args) {
    unsigned PI = First;
    for (const Expr *Arg : Args) {
      unsigned I = PI++;
      if (isa<CXXDefaultArgExpr>(Arg))
        continue;
      if (I < Params.size())
        F(Params[I], Params[I]->getType(), Arg, /*Variadic=*/false);
      else
        F(nullptr, Arg->getType(), Arg, /*Variadic=*/true);
    }
  };
  if (const auto *CE = dyn_cast<CallExpr>(CallLike)) {
    const FunctionDecl *Callee = CE->getDirectCallee();
    unsigned ID = Callee ? Callee->getBuiltinID() : 0;
    // A call the checks see through binds nothing.
    if (Callee && SemaProfiles::bindsNoArgument(Callee))
      return;
    if (ID && Ctx.BuiltinInfo.hasCustomTypechecking(ID)) {
      if (ID == Builtin::BI__builtin_operator_new ||
          ID == Builtin::BI__builtin_operator_delete) {
        for (const Expr *Arg : CE->arguments())
          F(nullptr, Arg->getType(), Arg, /*Variadic=*/false);
        return;
      }
      // A known callee's role parameter (va_start's va_list) is checked
      // against its declaration like an ordinary argument.
      if (std::optional<unsigned> I =
              SemaProfiles::getCalleeLifecycleRoles(Callee).KnownParam;
          I && *I < CE->getNumArgs()) {
        const ParmVarDecl *Parm = Callee->getParamDecl(*I);
        F(Parm, Parm->getType(), CE->getArg(*I), /*Variadic=*/false);
      }
      return;
    }
    if (CE->isUnevaluatedBuiltinCall(Ctx))
      return;
    unsigned ArgOffset = 0;
    if (const auto *MD = dyn_cast_or_null<CXXMethodDecl>(Callee);
        MD && isa<CXXOperatorCallExpr>(CE) &&
        !MD->isExplicitObjectMemberFunction())
      ArgOffset = 1;
    auto Args = llvm::drop_begin(CE->arguments(), ArgOffset);
    if (Callee) {
      Zip(Callee->parameters(), 0, Args);
      return;
    }
    QualType CalleeTy = CE->getCallee()->getType();
    if (CalleeTy->isSpecificPlaceholderType(BuiltinType::BoundMember))
      CalleeTy = Expr::findBoundMemberType(CE->getCallee());
    else if (CalleeTy->isAnyPointerType() || CalleeTy->isBlockPointerType() ||
             CalleeTy->isReferenceType())
      CalleeTy = CalleeTy->getPointeeType();
    const auto *Proto =
        CalleeTy.isNull() ? nullptr : CalleeTy->getAs<FunctionProtoType>();
    if (!Proto)
      return;
    unsigned PI = 0;
    for (const Expr *Arg : Args) {
      unsigned I = PI++;
      if (isa<CXXDefaultArgExpr>(Arg))
        continue;
      if (I < Proto->getNumParams())
        F(nullptr, Proto->getParamType(I), Arg, /*Variadic=*/false);
      else
        F(nullptr, Arg->getType(), Arg, /*Variadic=*/true);
    }
  } else if (const auto *CCE = dyn_cast<CXXConstructExpr>(CallLike)) {
    Zip(CCE->getConstructor()->parameters(), 0, CCE->arguments());
  } else if (const auto *NE = dyn_cast<CXXNewExpr>(CallLike)) {
    if (const FunctionDecl *OperatorNew = NE->getOperatorNew())
      Zip(OperatorNew->parameters(), NE->getNumImplicitArgs(),
          NE->placement_arguments());
  } else if (const auto *ME = dyn_cast<ObjCMessageExpr>(CallLike)) {
    if (const ObjCMethodDecl *MD = ME->getMethodDecl())
      for (auto [Parm, Arg] : llvm::zip(MD->parameters(), ME->arguments()))
        F(Parm, Parm->getType(), Arg, /*Variadic=*/false);
  }
}

/// The part of \p Whole, the entity range of the object \p G a credit's
/// argument leaf \p Leaf names as a whole, that a credit designated as
/// \p Designated reaches: the entities below the base subobject the
/// argument's derived-to-base casts convert to (TrackedStorage::wholeObject;
/// \p OuterPath holds the casts above the leaf, which follow the leaf's own)
/// when \p Designated covers that subobject's class (designationCovers), and
/// none otherwise.
static std::pair<unsigned, unsigned>
designatedWholeRange(const TrackedStorage &Storage,
                     const SemaProfiles::ShapeLeaf &Leaf, bool AsPointerValue,
                     const Expr *G, std::pair<unsigned, unsigned> Whole,
                     ArrayRef<const CXXRecordDecl *> OuterPath,
                     QualType Designated, bool RawExempt) {
  if (Whole.first == Whole.second)
    return Whole;
  BasePath CastPath(OuterPath);
  const Expr *Inner = SemaProfiles::peelBaseCasts(Leaf.E, CastPath, Leaf.Arms);
  if (const Expr *Op = SemaProfiles::addressOfOperand(Inner);
      AsPointerValue && Op)
    SemaProfiles::peelBaseCasts(Op, CastPath, Leaf.Arms);
  QualType Class = G->getType();
  if (Class->isPointerType())
    Class = Class->getPointeeType();
  if (!CastPath.empty())
    Class = Storage.Ctx.getCanonicalTagType(CastPath.back());
  if (!designationCovers(Storage.Ctx, Designated, Class, RawExempt))
    return {0, 0};
  return Storage.wholeObject(G, Leaf.Arms, CastPath);
}

/// True if \p P is the Construct role parameter of a known-callee row whose
/// callee takes a pack of constructor arguments (std::construct_at,
/// std::ranges::construct_at, std::allocator_traits::construct): the call
/// constructs one object, so of an array it constructs the first element.
static bool constructsOneObject(const SemaProfiles::CalleeLifecycleRoles &Roles,
                                const ParmVarDecl *P) {
  if (Roles.roleOf(P) != SemaProfiles::KnownParamRole::Construct)
    return false;
  const auto *FD = dyn_cast<FunctionDecl>(P->getDeclContext());
  const FunctionTemplateDecl *FTD = FD ? FD->getPrimaryTemplate() : nullptr;
  return FTD && llvm::any_of(FTD->getTemplatedDecl()->parameters(),
                             [](const ParmVarDecl *Param) {
                               return Param->isParameterPack();
                             });
}

/// The lifecycle effects of the call-like \p CallLike on the storage bound to
/// its parameters (\p Roles, derived by the caller; the pairs come from
/// forEachArgumentBinding). A [[now_init]] callee writes the storage
/// bound to each of its [[ref_to_uninit]] parameters (P4222R2 §6.2): a
/// member passed as `&m` / `m`, the storage a marked pointer refers to
/// passed as `p`, a decayed [[uninit]] array, an [[uninit]] local passed as
/// `&u` / `u`, or every tracked member of an object passed as a whole
/// (`this`, `*this`, `&x`, `x`); an argument whose arms name several
/// storages may-writes each. A trusted initializing parameter (memset's
/// destination) is written the same way.
/// A [[now_uninit]] callee destroys the storage bound to each pointer or
/// reference parameter (no marker key: the attribute's contract covers every
/// such argument) -- a DestroySite judged by the destroy rules, then
/// destroyed and unassigned: destruction makes the storage uninitialized
/// again (P4222R2 §4.4), and a destroy under a branch already spoils a read
/// at the join. A trusted storage-release callee (free, realloc's pointer, a
/// replaceable global operator delete) kills the same storage without a
/// judgment: a release ends no object's lifetime, so it records no destroyed
/// state and takes storage in any state. An argument whose arms name several
/// storages escapes each (the chosen arm is unknown, so each may have been
/// destroyed). Destroys and kills are appended before writes -- append order
/// is replay order -- so a dual-attributed reinitializer nets to assigned. An
/// allocator's own destroy (CalleeLifecycleRoles::OpaqueParam) escapes the
/// storage of its argument as an argument naming several storages does. A
/// plain callee earns nothing. The bindings judged at this call precede
/// these effects in the block, so they see the pre-call state. A judged
/// destroy site's flow-tracked leaves are added to \p JudgedLeaves. A
/// non-null \p DestroyedObject -- the object of an explicit destructor call or
/// the operand of a delete-expression that runs a destructor (a pointer, or a
/// glvalue bound by reference) -- is destroyed first, like std::destroy_at's
/// argument (P4222R2 §4.6).
static void
appendLifecycleCallEvents(Sema &S, const Decl *D, const Expr *CallLike,
                          const SemaProfiles::CalleeLifecycleRoles &Roles,
                          const TrackedStorage &Storage,
                          SmallVectorImpl<DefAssignEvent> &BlockEvents,
                          SmallVectorImpl<DestroySite> &DSites,
                          llvm::SmallPtrSetImpl<const Expr *> &JudgedLeaves,
                          const Expr *DestroyedObject = nullptr) {
  const auto *DC = cast<DeclContext>(D);
  // Resolve the leaves of \p Arg, bound to a parameter of type \p ParamTy
  // (SemaProfiles::bindingSourceOperand decides whether they are pointer
  // values or glvalues): per leaf, the whole entity it names, the entities of
  // an object it names as a whole, or nothing; and whether any leaf is
  // flow-tracked.
  struct ArgLeaf {
    const Expr *E;
    TrackedStorage::Resolution R;
    std::pair<unsigned, unsigned> Whole{0, 0};
    bool Tracked = false;
    /// The member entities of an object a leaf tracked for a marked
    /// parameter names as a whole (TrackedStorage::objectPart).
    std::pair<unsigned, unsigned> Part{0, 0};
    /// The form state of a leaf no entity tracks.
    LeafState Form = LeafState::Unknown;
    /// The leaf names an element of what a pointer or handle refers to
    /// (SemaProfiles::designatesReferentElement, or an offset handle leaf).
    bool Element = false;
  };
  // Resolve the leaves of \p Arg; a credit passes its \p Designated type and
  // RawExempt (TrackedStorage::resolve). A credit of an object passed as a
  // whole reaches only the entities of the base subobject its argument
  // converts to, and none when \p Designated is not that subobject's class.
  // \p Use is the parameter's (SemaProfiles::isFlowTrackedLeaf).
  auto ResolveArg = [&](const Expr *Arg, QualType ParamTy,
                        SmallVectorImpl<ArgLeaf> &Leaves,
                        QualType Designated = QualType(),
                        bool RawExempt = false,
                        SemaProfiles::FlowLeafUse Use =
                            SemaProfiles::FlowLeafUse::Plain) {
    bool AnyTracked = false;
    // One leaf, used as a pointer value or a glvalue, below the casts
    // \p OuterPath; a leaf no entity tracks takes \p Form's state.
    auto AddLeaf = [&](const SemaProfiles::ShapeLeaf &Leaf, bool AsPointerValue,
                       ArrayRef<const CXXRecordDecl *> OuterPath,
                       llvm::function_ref<LeafState()> Form) {
      ArgLeaf L{Leaf.E, Storage.resolveFlowLeaf(Leaf, AsPointerValue,
                                                Designated, RawExempt)};
      // A credit through a conversion to a base class names the base
      // subobject, whatever its designated type.
      if (L.R.Entity && !Designated.isNull()) {
        BasePath CastPath(OuterPath);
        SemaProfiles::peelBaseCasts(Leaf.E, CastPath, Leaf.Arms);
        if (!CastPath.empty()) {
          L.R.Subobject = true;
          L.R.Sub = SubKey();
        }
      }
      L.Tracked = S.Profiles().isFlowTrackedLeaf(Leaf, AsPointerValue, DC, Use);
      L.Element = SemaProfiles::designatesReferentElement(
          Leaf.E, AsPointerValue, Leaf.Arms);
      if (L.Tracked && Use == SemaProfiles::FlowLeafUse::MarkedTarget &&
          !L.R.Entity)
        L.Part = Storage.objectPart(Leaf, AsPointerValue, OuterPath);
      AnyTracked |= L.Tracked;
      // The current object's lifetime entity stands beside its members.
      if (!L.R.Entity || L.R.Entity == Storage.CurrentObjectRoot) {
        const Expr *G = SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms);
        if (const Expr *Op = SemaProfiles::addressOfOperand(G);
            AsPointerValue && Op)
          G = SemaProfiles::ignoreTransparentCasts(Op, Leaf.Arms);
        L.Whole = Storage.wholeObject(G, Leaf.Arms);
        if (!Designated.isNull())
          L.Whole =
              designatedWholeRange(Storage, Leaf, AsPointerValue, G, L.Whole,
                                   OuterPath, Designated, RawExempt);
        L.Form = Form();
      }
      Leaves.push_back(L);
    };
    // A handle's storage is what its walk reaches through a transfer leaf
    // (SemaProfiles::forEachHandleSourceLeaf), a Partial leaf's a subobject
    // of it; a range taken by reference is its own storage, else what its
    // walk reaches.
    if (SemaProfiles::isHandleType(ParamTy.getNonReferenceType())) {
      using HandleLeaf = SemaProfiles::HandleLeaf;
      SemaProfiles::HandleWalkMemo Memo;
      auto Visit = [&](auto &Self, const HandleLeaf &HL) -> void {
        auto FormOf = [&](const SemaProfiles::ShapeLeaf &Leaf) {
          return formState(S.Profiles().classifyInitHandleLeaf(
              {Leaf.E, HL.M, HL.Partial, Leaf.Arms}, D));
        };
        if (HL.M == HandleLeaf::Mode::Handle) {
          ArgLeaf L{HL.E, Storage.resolveHandleLeaf(HL, Designated, RawExempt)};
          L.Tracked = S.Profiles().isFlowTrackedHandleLeaf(HL, DC);
          L.Element = HL.Partial;
          AnyTracked |= L.Tracked;
          if (!L.R.Entity)
            L.Form = FormOf(HL.E);
          Leaves.push_back(L);
          return;
        }
        SemaProfiles::forEachTargetLeaf(
            HL.E, [&](const SemaProfiles::ShapeLeaf &Leaf) {
              AddLeaf(Leaf, HL.M == HandleLeaf::Mode::PointerValue, {},
                      [&] { return FormOf(Leaf); });
              ArgLeaf &L = Leaves.back();
              L.Element |= HL.Partial;
              if (HL.Partial && L.R.Entity) {
                L.R.Subobject = true;
                L.R.Sub = SubKey();
              }
              if (HL.M == HandleLeaf::Mode::Range && !L.R.Entity &&
                  L.Whole.first == L.Whole.second) {
                Leaves.pop_back();
                SemaProfiles::forEachHandleSourceLeaf(
                    Leaf.E,
                    [&](const HandleLeaf &Inner) {
                      // An offset range offsets what it refers to.
                      Self(Self,
                           HandleLeaf{Inner.E, Inner.M,
                                      Inner.Partial || HL.Partial, Inner.Arms});
                    },
                    &Memo);
              }
            });
      };
      if (ParamTy->isReferenceType())
        Visit(Visit, HandleLeaf{Arg, HandleLeaf::Mode::Range});
      else
        SemaProfiles::forEachHandleSourceLeaf(
            Arg, [&](const HandleLeaf &HL) { Visit(Visit, HL); }, &Memo);
      return AnyTracked;
    }
    bool AsPointerValue;
    const Expr *Operand =
        SemaProfiles::bindingSourceOperand(Arg, ParamTy, AsPointerValue);
    if (!Operand)
      return false;
    // The derived-to-base casts above the leaves, which the leaf walk peels.
    BasePath OuterPath;
    SemaProfiles::peelBaseCasts(Operand, OuterPath);
    SemaProfiles::forEachTargetLeaf(
        Operand, [&](const SemaProfiles::ShapeLeaf &Leaf) {
          AddLeaf(Leaf, AsPointerValue, OuterPath, [&] {
            return formState(
                S.Profiles().classifyInitBindingLeaf(Leaf, ParamTy, D));
          });
        });
    return AnyTracked;
  };

  // The leaves name one storage: each names the same whole entity, or the
  // same object as a whole.
  auto NameOneStorage = [](ArrayRef<ArgLeaf> Leaves) {
    return llvm::all_of(Leaves, [&](const ArgLeaf &L) {
      const ArgLeaf &First = Leaves.front();
      return L.R.Entity == First.R.Entity &&
             L.R.Subobject == First.R.Subobject && L.Whole == First.Whole;
    });
  };
  // The storage an argument names, as events of \p Kind: the resolved whole
  // entity, or every entity of the object passed as a whole. A credit passes
  // its \p Designated type and RawExempt (ResolveArg).
  auto AppendFor = [&](const Expr *Arg, QualType ParamTy,
                       DefAssignEventKind Kind,
                       QualType Designated = QualType(), bool RawExempt = false,
                       bool SkipUninitMembers = false) {
    SmallVector<ArgLeaf, 2> Leaves;
    ResolveArg(Arg, ParamTy, Leaves, Designated, RawExempt);
    if (Leaves.empty())
      return;
    if (NameOneStorage(Leaves))
      Leaves.truncate(1);
    else
      Kind = Kind == DefAssignEventKind::Write ? DefAssignEventKind::MayWrite
                                               : DefAssignEventKind::Escape;
    auto Push = [&](unsigned Idx, SubKey Sub = SubKey()) {
      DefAssignEvent Ev{Kind, Idx, CallLike};
      Ev.Designated = Designated;
      Ev.RawExempt = RawExempt;
      Ev.Sub = Sub;
      Ev.SkipUninitMembers = SkipUninitMembers;
      BlockEvents.push_back(Ev);
    };
    for (const ArgLeaf &L : Leaves) {
      if (L.R.Entity && !L.R.Subobject)
        Push(*L.R.Entity);
      // A credit of part of the storage may have initialized it, so it is no
      // longer uninitialized on every path; a read still needs every part.
      if (L.R.Entity && L.R.Subobject && Kind == DefAssignEventKind::Write)
        BlockEvents.push_back(
            {DefAssignEventKind::MayWrite, *L.R.Entity, CallLike});
      // A credit naming a SubMember whole credits that member.
      if (L.R.Entity && L.R.Subobject && L.R.Sub.Exact)
        Push(*L.R.Entity, L.R.Sub);
      if (L.R.Entity && L.R.Entity != Storage.CurrentObjectRoot)
        continue;
      // The member entities of an object passed whole are its [[uninit]]
      // members, which a default-initialization leaves uninitialized.
      if (SkipUninitMembers)
        continue;
      for (unsigned Idx = L.Whole.first; Idx != L.Whole.second; ++Idx)
        Push(Idx);
    }
  };
  // The argument \p Arg, bound as \p T (to a [[ref_to_uninit]] parameter
  // when \p Marked), destroyed by \p Destroyer: one destroy site.
  auto AppendDestroy = [&](const Expr *Arg, QualType T, bool Exempt,
                           SemaProfiles::InitDestroyer Destroyer,
                           bool Marked = false) {
    SmallVector<ArgLeaf, 2> Leaves;
    DestroySite Site;
    Site.Loc = Arg->IgnoreImplicit()->getExprLoc();
    Site.Exempt = Exempt;
    Site.Destroyer = Destroyer;
    Site.Judged = ResolveArg(Arg, T, Leaves, QualType(), /*RawExempt=*/false,
                             Marked ? SemaProfiles::FlowLeafUse::MarkedTarget
                                    : SemaProfiles::FlowLeafUse::Destroy);
    Site.Conditional = !NameOneStorage(Leaves);
    for (const ArgLeaf &L : Leaves) {
      if (L.Tracked)
        JudgedLeaves.insert(L.E);
      // An element of what a pointer or handle refers to is not judged and
      // destroys nothing tracked (SemaProfiles::designatesReferentElement).
      if (L.Element)
        continue;
      BindingLeaf Leaf;
      if (L.R.Entity) {
        Leaf.Entity = L.R.Entity;
        if (!L.R.Subobject && !llvm::is_contained(Site.Affected, *L.R.Entity))
          Site.Affected.push_back(*L.R.Entity);
      } else {
        Leaf.State = L.Form;
        Leaf.Range = L.Part;
      }
      if (!L.R.Entity || L.R.Entity == Storage.CurrentObjectRoot)
        for (unsigned Idx = L.Whole.first; Idx != L.Whole.second; ++Idx)
          if (!llvm::is_contained(Site.Affected, Idx))
            Site.Affected.push_back(Idx);
      Site.Leaves.push_back(Leaf);
    }
    if ((!Site.Judged || Site.Leaves.empty()) && Site.Affected.empty())
      return;
    DSites.push_back(std::move(Site));
    BlockEvents.push_back({DefAssignEventKind::Destroy, 0, CallLike,
                           (unsigned)(DSites.size() - 1)});
  };
  auto ForEachParam =
      [&](llvm::function_ref<void(const ParmVarDecl *, const Expr *)> F) {
        forEachArgumentBinding(S.Context, CallLike,
                               [&](const ParmVarDecl *P, QualType,
                                   const Expr *Arg, bool Variadic) {
                                 if (P && !Variadic)
                                   F(P, Arg);
                               });
      };
  if (DestroyedObject) {
    QualType OT = DestroyedObject->getType();
    AppendDestroy(
        DestroyedObject,
        OT->isPointerType() ? OT : S.Context.getLValueReferenceType(OT),
        /*Exempt=*/false, SemaProfiles::InitDestroyer::DestructorCall);
  }
  if (Roles.DestroysParams || Roles.KnownParam)
    ForEachParam([&](const ParmVarDecl *P, const Expr *Arg) {
      QualType PT = P->getType();
      if (!PT->isPointerType() && !PT->isReferenceType() &&
          !SemaProfiles::isHandleType(PT))
        return;
      if (Roles.destroysParam(P))
        AppendDestroy(
            Arg, PT,
            Roles.InitializesRefToUninitParams || Roles.isMarkedParam(P),
            SemaProfiles::InitDestroyer::Function, Roles.isMarkedParam(P));
      else if (Roles.releasesParam(P, /*TrustedOnly=*/true))
        AppendFor(Arg, PT, DefAssignEventKind::Kill);
    });
  if (Roles.OpaqueParam)
    ForEachParam([&](const ParmVarDecl *P, const Expr *Arg) {
      if (P->getFunctionScopeIndex() == *Roles.OpaqueParam)
        AppendFor(Arg, P->getType(), DefAssignEventKind::Escape);
    });
  // A credit designates the parameter's pointee or referenced type; a
  // [[now_init]] or initializing parameter claims raw bytes
  // (designationCovers).
  if (Roles.InitializesRefToUninitParams || Roles.KnownParam)
    ForEachParam([&](const ParmVarDecl *P, const Expr *Arg) {
      if (!Roles.initializesParam(P))
        return;
      QualType PT = P->getType();
      // A class iterator or range designates its value type.
      QualType Designated =
          PT->isPointerType() ? PT->getPointeeType() : PT.getNonReferenceType();
      if (SemaProfiles::isHandleType(Designated))
        if (QualType VT = SemaProfiles::rangeValueType(PT); !VT.isNull())
          Designated = VT;
      bool RawExempt =
          (Roles.InitializesRefToUninitParams && Roles.isMarkedParam(P)) ||
          Roles.isInitializingParam(P);
      if (constructsOneObject(Roles, P) && Designated->isObjectType() &&
          !Designated->isIncompleteType())
        Designated = S.Context.getConstantArrayType(
            Designated, llvm::APInt(64, 1), nullptr, ArraySizeModifier::Normal,
            0);
      AppendFor(Arg, PT, DefAssignEventKind::Write, Designated, RawExempt,
                /*SkipUninitMembers=*/Roles.DefaultInitCredit &&
                    Roles.roleOf(P) == SemaProfiles::KnownParamRole::Construct);
    });
}

/// Append to \p Out the children of \p S that the engine's CFG evaluates
/// with the enclosing body: a lambda's capture initializers (its body too
/// with \p EnterLambdaBodies), an OpenMP directive's clause expressions and
/// region statement (as CFGBuilder::VisitOMPExecutableDirective visits them),
/// an opaque value's source (the CFG evaluates it where the opaque value's
/// owner puts it), and every child of any other statement -- of a non-OpenMP
/// captured statement, its capture initializers only.
static void appendAnalyzedChildren(const Stmt *S,
                                   SmallVectorImpl<const Stmt *> &Out,
                                   bool EnterLambdaBodies = false) {
  if (const auto *OVE = dyn_cast<OpaqueValueExpr>(S)) {
    Out.push_back(OVE->getSourceExpr());
    return;
  }
  if (const auto *LE = dyn_cast<LambdaExpr>(S)) {
    llvm::append_range(Out, LE->capture_inits());
    if (EnterLambdaBodies)
      Out.push_back(LE->getBody());
    return;
  }
  if (const auto *D = dyn_cast<OMPExecutableDirective>(S)) {
    llvm::append_range(
        Out, OMPExecutableDirective::used_clauses_children(D->clauses()));
    if (!D->isStandaloneDirective())
      Out.push_back(D->getRawStmt());
    return;
  }
  llvm::append_range(Out, S->children());
}

/// Call \p F once on each statement of the body the engine analyzes over
/// \p D, parents first: a constructor's written initializers, then the body --
/// a coroutine's user-written body, from which AnalysisDeclContext builds the
/// CFG -- descending through appendAnalyzedChildren. An opaque value's source
/// is reached from the opaque value and from its owner, and is visited at the
/// first. The harvest, the exemption walk, and the constructor-body set take
/// the body's statements from here (ProfilesFrameworkInternals.rst,
/// "Flow-Tracked Storage (std::init)").
static void forEachAnalyzedStmt(const Decl *D,
                                llvm::function_ref<void(const Stmt *)> F) {
  const Stmt *Body = D->getBody();
  if (const auto *Coro = dyn_cast_or_null<CoroutineBodyStmt>(Body))
    Body = Coro->getBody();
  SmallVector<const Stmt *, 32> Stack(1, Body);
  if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(D))
    for (const CXXCtorInitializer *Init : llvm::reverse(Ctor->inits()))
      if (Init->isWritten())
        Stack.push_back(Init->getInit());
  SmallVector<const Stmt *, 8> Children;
  llvm::SmallPtrSet<const Stmt *, 32> Visited;
  while (!Stack.empty()) {
    const Stmt *Cur = Stack.pop_back_val();
    if (!Cur || !Visited.insert(Cur).second)
      continue;
    F(Cur);
    Children.clear();
    appendAnalyzedChildren(Cur, Children);
    Stack.append(Children.rbegin(), Children.rend());
  }
}

/// Call \p Read with each glvalue a read in the body of the lambda \p LE
/// loads -- an lvalue-to-rvalue conversion's operand, a compound assignment's
/// left operand, an increment's or decrement's operand -- and the reading
/// expression, once per source range: in the body, in the call operators of a
/// generic lambda instantiated so far (its pattern's dependent reads carry no
/// load), and in the nested lambdas and OpenMP regions appendAnalyzedChildren
/// reaches, except below a nested lambda \p Skip returns true for.
static void forEachLambdaBodyRead(
    const LambdaExpr *LE, llvm::function_ref<bool(const LambdaExpr *)> Skip,
    llvm::function_ref<void(const Expr *, const Expr *)> Read) {
  SmallVector<const Stmt *, 16> Stack(1, LE->getBody());
  auto PushSpecializations = [&](const LambdaExpr *L) {
    if (const FunctionTemplateDecl *FTD = L->getDependentCallOperator())
      for (const FunctionDecl *Spec : FTD->specializations())
        Stack.push_back(Spec->getBody());
  };
  PushSpecializations(LE);
  llvm::SmallPtrSet<const Stmt *, 16> Visited;
  llvm::DenseSet<std::pair<SourceLocation, SourceLocation>> ReadRanges;
  while (!Stack.empty()) {
    const Stmt *Cur = Stack.pop_back_val();
    if (!Cur || !Visited.insert(Cur).second)
      continue;
    if (const auto *NestedLE = dyn_cast<LambdaExpr>(Cur)) {
      if (Skip(NestedLE))
        continue;
      PushSpecializations(NestedLE);
    }
    const Expr *G = nullptr;
    if (const auto *BodyICE = dyn_cast<ImplicitCastExpr>(Cur);
        BodyICE && BodyICE->getCastKind() == CK_LValueToRValue)
      G = BodyICE->getSubExpr();
    else if (const auto *BodyBO = dyn_cast<BinaryOperator>(Cur);
             BodyBO && BodyBO->isCompoundAssignmentOp())
      G = BodyBO->getLHS();
    else if (const auto *BodyUO = dyn_cast<UnaryOperator>(Cur);
             BodyUO && BodyUO->isIncrementDecrementOp())
      G = BodyUO->getSubExpr();
    if (G && ReadRanges.insert({Cur->getBeginLoc(), Cur->getEndLoc()}).second)
      Read(G, cast<Expr>(Cur));
    appendAnalyzedChildren(Cur, Stack, /*EnterLambdaBodies=*/true);
  }
}

/// Append a Read at \p At of each entity among \p Entities the read of \p G
/// resolves to (TrackedStorage::resolve): the entity it names, or each one a
/// pointer-to-data-member step may access.
static void appendResolvedReads(const TrackedStorage &Storage, const Expr *G,
                                const Expr *At,
                                llvm::function_ref<bool(unsigned)> Entities,
                                SmallVectorImpl<DefAssignEvent> &BlockEvents) {
  TrackedStorage::Resolution R = Storage.resolve(G);
  auto Read = [&](unsigned Idx) {
    if (Storage.Entities[Idx].TrackReads && Entities(Idx))
      BlockEvents.push_back({DefAssignEventKind::Read, Idx, At});
  };
  if (R.Entity)
    Read(*R.Entity);
  for (unsigned Idx = R.Opaque.first; Idx != R.Opaque.second; ++Idx)
    Read(Idx);
}

// The lambda body is a separate function and never appears in the enclosing
// constructor's CFG, but a this-capturing lambda can read members the moment it
// is created (it may be invoked immediately). A *this capture copy-constructs
// the whole object at creation, reading every member right there: unless the
// class has a user-provided copy constructor (\p StarThisCopyTrusted; see
// ProfilesFrameworkInternals.rst, "Known Gaps (std::init)"), append one Read
// per tracked member at the LambdaExpr, and never body-scan -- body accesses go
// to the *copy*, so attributing them to the original would be wrong either way.
// A plain this capture instead reads nothing at creation: treat every member
// read in its body (forEachLambdaBodyRead) as a Read at the LambdaExpr's
// program point, except that a nested lambda capturing *this gets the
// whole-object reads at its own LambdaExpr (its creation runs when the outer
// body does, which may be immediately) and is not descended into. Writes in a
// body earn no assignment credit (the lambda may never run), consistent with
// the intersection semantics; a lambda stored now and called only after the
// member is assigned is still flagged (accepted imprecision). Capture
// initializers are ordinary CFG elements, already handled by the caller's
// other arms.
static void appendThisCaptureLambdaReadEvents(
    const LambdaExpr *LE, const TrackedStorage &Storage,
    bool StarThisCopyTrusted, SmallVectorImpl<DefAssignEvent> &BlockEvents) {
  auto GetThisCaptureKind =
      [](const LambdaExpr *L) -> std::optional<LambdaCaptureKind> {
    for (const LambdaCapture &C : L->captures())
      if (C.capturesThis())
        return C.getCaptureKind();
    return std::nullopt;
  };
  auto AppendWholeObjectReads = [&](const LambdaExpr *At) {
    for (unsigned I = Storage.CurrentObjectRange.first,
                  N = Storage.CurrentObjectRange.second;
         I != N; ++I)
      if (Storage.Entities[I].TrackReads)
        BlockEvents.push_back({DefAssignEventKind::Read, I, At});
  };
  std::optional<LambdaCaptureKind> Kind = GetThisCaptureKind(LE);
  if (!Kind)
    return;
  if (*Kind == LCK_StarThis) {
    if (!StarThisCopyTrusted)
      AppendWholeObjectReads(LE);
    return;
  }
  forEachLambdaBodyRead(
      LE,
      [&](const LambdaExpr *Nested) {
        if (GetThisCaptureKind(Nested) != LCK_StarThis)
          return false;
        if (!StarThisCopyTrusted)
          AppendWholeObjectReads(Nested);
        return true;
      },
      [&](const Expr *G, const Expr *At) {
        appendResolvedReads(
            Storage, G, At,
            [&](unsigned Idx) {
              return Storage.Entities[Idx].K ==
                     TrackedEntity::Kind::CurrentObjectMember;
            },
            BlockEvents);
      });
}

static const DeclRefExpr *getLocalCopySourceRef(const VarDecl *V,
                                                BasePath &CastPath);

/// A by-copy capture of a read-tracked local copies it into the closure where
/// the lambda \p LE is created, as a tracked copy's declaration copies its
/// source, and so does an init-capture copying one of its own class: a member
/// read of the copy in the body (forEachLambdaBodyRead) is a Read of the
/// local's member at the LambdaExpr; writes in the body earn no credit, as for
/// a this capture.
static void
appendByCopyCaptureReadEvents(const LambdaExpr *LE,
                              const TrackedStorage &Storage,
                              SmallVectorImpl<DefAssignEvent> &BlockEvents) {
  SmallVector<std::pair<unsigned, unsigned>, 2> Copied;
  SmallVector<std::pair<const VarDecl *, const VarDecl *>, 2> InitCopies;
  for (const LambdaCapture &C : LE->captures()) {
    if (!C.capturesVariable() || C.getCaptureKind() != LCK_ByCopy)
      continue;
    const auto *V = dyn_cast<VarDecl>(C.getCapturedVar());
    if (!V)
      continue;
    if (V->isInitCapture()) {
      BasePath CastPath;
      const DeclRefExpr *SrcRef = getLocalCopySourceRef(V, CastPath);
      const auto *Src = SrcRef ? dyn_cast<VarDecl>(SrcRef->getDecl()) : nullptr;
      if (Src && CastPath.empty() && Storage.LocalRange.count(Src))
        InitCopies.push_back({V, Src});
      continue;
    }
    if (auto It = Storage.LocalRange.find(V); It != Storage.LocalRange.end())
      Copied.push_back(It->second);
  }
  if (Copied.empty() && InitCopies.empty())
    return;
  forEachLambdaBodyRead(
      LE, [](const LambdaExpr *) { return false; },
      [&](const Expr *G, const Expr *At) {
        appendResolvedReads(
            Storage, G, At,
            [&](unsigned Idx) {
              return llvm::any_of(Copied, [&](std::pair<unsigned, unsigned> R) {
                return Idx >= R.first && Idx < R.second;
              });
            },
            BlockEvents);
        std::optional<SemaProfiles::FlowLeafShape> Shape =
            SemaProfiles::flowLeafShape(G);
        for (auto [Capture, Src] : InitCopies) {
          if (!Shape || Shape->Local != Capture)
            continue;
          if (std::optional<SemaProfiles::FlowMemberMatch> M =
                  Storage.Profiles.matchFlowTrackedMember(
                      Storage.LocalClass.lookup(Src), *Shape)) {
            unsigned Idx = Storage.LocalRange.lookup(Src).first + M->Index;
            if (Storage.Entities[Idx].TrackReads)
              BlockEvents.push_back({DefAssignEventKind::Read, Idx, At});
          }
        }
      });
}

/// The statements of a constructor body the engine takes current-object events
/// from (forEachAnalyzedStmt): the body plus each *written* member/base
/// initializer expression (the CFG is built with AddInitializers=true, so those
/// run as CFG elements in execution order, and a written initializer's own
/// member is assigned at its CFGInitializer element, so declaration order
/// decides what an initializer may read), and each single-declarator DeclStmt
/// \p cfg synthesizes from a multi-declarator one of the body. An NSDMI's
/// subexpressions are excluded although AddCXXDefaultInitExprInCtors puts them
/// in the CFG, so a read of a tracked member inside another member's default
/// initializer stays undetected (see ProfilesFrameworkInternals.rst,
/// "Known Gaps (std::init)").
static void collectCtorBodyStmts(const CXXConstructorDecl *Ctor, const CFG &cfg,
                                 llvm::SmallPtrSetImpl<const Stmt *> &Body) {
  forEachAnalyzedStmt(Ctor, [&](const Stmt *S) { Body.insert(S); });
  for (const auto &[Synthetic, Source] : cfg.synthetic_stmts())
    if (Body.count(Source))
      Body.insert(Synthetic);
}

/// The element a braced aggregate initializer \p Init gives the subobject at
/// \p Path below the object it initializes, through nested braced lists; null
/// when a step is not such an element (a constructor call, a class with
/// bases, a designator the list does not reach).
static const Expr *aggregateInitializerFor(const Expr *Init,
                                           ArrayRef<MemberStep> Path) {
  for (const MemberStep &S : Path) {
    const auto *ILE = dyn_cast_or_null<InitListExpr>(Init->IgnoreImplicit());
    if (!ILE || !S.Field || !S.Bases.empty())
      return nullptr;
    const auto *RD = dyn_cast<CXXRecordDecl>(S.Field->getParent());
    if (!RD || RD->getNumBases() ||
        S.Field->getFieldIndex() >= ILE->getNumInits())
      return nullptr;
    Init = ILE->getInit(S.Field->getFieldIndex());
  }
  return Init;
}

/// Recover the per-block std::init events of \p cfg over \p Storage: the one
/// extraction loop of the engine. Every store arm resolves through
/// TrackedStorage::resolve and distributes a comma, conditional, or GNU ?:
/// lvalue to its leaves (SemaProfiles::forEachTargetLeaf): a load reads
/// whichever member the chosen arm names; a store writes the one whole entity
/// every leaf names and otherwise may-writes each named entity (a compound one
/// reads each leaf); a store to a marked pointer reseats it to the store's
/// source. A written member or base initializer writes its members; a lifecycle
/// call kills or writes the storage bound to its parameters
/// (appendLifecycleCallEvents); a this-capturing lambda reads members at its
/// creation; a non-benign DeclRefExpr of a tracked aggregate local is an escape
/// (the read leniency for locals, ProfilesFrameworkInternals.rst,
/// "Known Gaps (std::init)"); a tracked local's DeclStmt resets its storage
/// (Reset), after which a tracked copy's copies each member's state. Every
/// pointer, reference, or handle binding the body performs -- a variable,
/// member, or base initializer, an aggregate element, a call or constructor
/// argument (declared, variadic, or the implicit object argument), a placement
/// argument, an Objective-C message argument, a return, a throw, a
/// new-expression's initializer, a pointer assignment, a lambda capture --
/// becomes a Binding site when a leaf of its source names tracked storage
/// (SemaProfiles::isFlowTrackedLeaf: the parse-time funnel left exactly those
/// to this pass), its other leaves classified by form; a binding that hands out
/// a mutable alias of a marked pointer (`T *&` to `p`, `T **` to `&p`, a
/// by-reference capture) gives it an anonymous referent of unknown state, and a
/// delete-expression kills its operand's storage; a [[now_uninit]] call's
/// arguments become Destroy sites (appendLifecycleCallEvents); a load through a
/// marked pointer or reference or of a subobject of an [[uninit]] local, and a
/// store below either, become ReadThrough / SubobjectWrite sites (AccessSite).
/// Current-object events of a constructor (\p Ctor non-null) come from the
/// constructor body and its written initializers only. \p cfg is the engine's
/// own, fully linearized CFG (runStdInitMemberReadChecks), so every statement
/// class has an element. The flow-tracked leaves of every Binding, Destroy,
/// ReadThrough, and SubobjectWrite site go to \p JudgedLeaves.
static void extractStdInitEvents(
    Sema &S, const Decl *D, const CFG &cfg, const TrackedStorage &Storage,
    const CXXConstructorDecl *Ctor, bool StarThisCopyTrusted,
    std::vector<SmallVector<DefAssignEvent, 4>> &Events,
    SmallVectorImpl<BindingSite> &Sites, SmallVectorImpl<DestroySite> &DSites,
    SmallVectorImpl<AccessSite> &ASites,
    llvm::SmallPtrSetImpl<const Expr *> &JudgedLeaves) {
  using K = SemaProfiles::InitBindingKind;
  ASTContext &Ctx = S.Context;
  const auto *DC = cast<DeclContext>(D);
  llvm::SmallPtrSet<const Stmt *, 32> BodyStmts;
  if (Ctor)
    collectCtorBodyStmts(Ctor, cfg, BodyStmts);
  auto ForEachLeaf =
      [&](const Expr *E,
          llvm::function_ref<void(const TrackedStorage::Resolution &)> F,
          QualType Designated = QualType()) {
        SemaProfiles::forEachTargetLeaf(
            E, [&](const SemaProfiles::ShapeLeaf &Leaf) {
              F(Storage.resolve(Leaf, Designated));
            });
      };

  // Pass one: the base DeclRefExprs a recognized member read or write
  // consumes benignly do not escape the object. A DeclRefExpr element
  // precedes its consuming cast/operator element in a block, so the set must
  // be complete before elements are classified. The source ref of a tracked
  // copy is consumed by the copy's implicit constructor and modeled by the
  // Copy event -- an escape there would wrongly mark the *source* assigned.
  llvm::SmallPtrSet<const DeclRefExpr *, 16> Benign;
  auto NoteBenign = [&](const Expr *G) {
    ForEachLeaf(G, [&](const TrackedStorage::Resolution &R) {
      if (R.LocalBase && R.Benign)
        Benign.insert(R.LocalBase);
    });
  };
  for (const CFGBlock *B : cfg) {
    for (const CFGElement &Elem : *B) {
      auto CS = Elem.getAs<CFGStmt>();
      if (!CS)
        continue;
      const Stmt *St = CS->getStmt();
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(St)) {
        if (ICE->getCastKind() == CK_LValueToRValue)
          NoteBenign(ICE->getSubExpr());
      } else if (const auto *BBC = dyn_cast<BuiltinBitCastExpr>(St)) {
        NoteBenign(BBC->getSubExpr());
      } else if (const auto *BO = dyn_cast<BinaryOperator>(St)) {
        if (BO->isAssignmentOp())
          NoteBenign(BO->getLHS());
      } else if (const auto *UO = dyn_cast<UnaryOperator>(St)) {
        if (UO->isIncrementDecrementOp())
          NoteBenign(UO->getSubExpr());
      } else if (const auto *DS = dyn_cast<DeclStmt>(St)) {
        for (const Decl *Dcl : DS->decls())
          if (const auto *V = dyn_cast<VarDecl>(Dcl))
            if (Storage.CopySource.count(V)) {
              BasePath Unused;
              if (const DeclRefExpr *Base = getLocalCopySourceRef(V, Unused))
                Benign.insert(Base);
            }
      } else if (const auto *LE = dyn_cast<LambdaExpr>(St)) {
        // A by-copy capture of a tracked aggregate copies it
        // (appendByCopyCaptureReadEvents).
        for (const Expr *Init : LE->capture_inits())
          if (const auto *CCE = dyn_cast_or_null<CXXConstructExpr>(
                  Init ? Init->IgnoreImplicit() : nullptr);
              CCE && CCE->getNumArgs() >= 1 &&
              CCE->getConstructor()->isCopyOrMoveConstructor()) {
            BasePath Unused;
            if (const auto *Src = dyn_cast<DeclRefExpr>(
                    SemaProfiles::peelBaseCasts(CCE->getArg(0), Unused));
                Src &&
                Storage.LocalRange.count(dyn_cast<VarDecl>(Src->getDecl())))
              Benign.insert(Src);
          }
      }
    }
  }

  // Pass two: the events, in CFG element order.
  for (const CFGBlock *B : cfg) {
    auto &BlockEvents = Events[B->getBlockID()];
    // The resolutions of a glvalue's leaves, one per leaf, with the access's
    // \p Designated type (TrackedStorage::resolve).
    SmallVector<TrackedStorage::Resolution, 4> Leaves;
    auto CollectLeaves = [&](const Expr *G, QualType Designated = QualType()) {
      Leaves.clear();
      ForEachLeaf(
          G, [&](const TrackedStorage::Resolution &R) { Leaves.push_back(R); },
          Designated);
    };
    // One Read per distinct resolved, read-tracked leaf: only members are
    // read-tracked, a read of part of a member reads the member, and a
    // pointer-to-data-member read reads each member it may name.
    auto AppendReads = [&](const Expr *At) {
      SmallVector<unsigned, 4> Seen;
      auto Read = [&](unsigned Idx) {
        if (Storage.Entities[Idx].TrackReads && !Storage.Entities[Idx].Whole &&
            !llvm::is_contained(Seen, Idx)) {
          Seen.push_back(Idx);
          BlockEvents.push_back({DefAssignEventKind::Read, Idx, At});
        }
      };
      for (const TrackedStorage::Resolution &R : Leaves) {
        if (R.Entity)
          Read(*R.Entity);
        for (unsigned Idx = R.Opaque.first; Idx != R.Opaque.second; ++Idx)
          Read(Idx);
      }
    };
    // A Reseat of the marked pointer, reference, or handle \p Idx at \p At.
    auto AppendReseat = [&](unsigned Idx, const Expr *At, ReseatTarget RT,
                            unsigned Aux = 0) {
      DefAssignEvent Ev{DefAssignEventKind::Reseat, Idx, At, Aux};
      Ev.RT = RT;
      BlockEvents.push_back(Ev);
    };
    // The destruction of the referent of the marked std::unique_ptr \p Idx of
    // type \p OwnerT by \p Destroyer at \p Loc (P4222R2 §4.6): destroy_uninit
    // applies where the owned object's type has a non-trivial destructor, as
    // for an [[uninit]] local's scope exit; double_destroy always.
    auto AppendOwnerDestroy = [&](unsigned Idx, QualType OwnerT,
                                  SourceLocation Loc,
                                  SemaProfiles::InitDestroyer Destroyer,
                                  const VarDecl *ScopeExitVar, const Expr *At) {
      DestroySite Site;
      Site.Loc = Loc;
      Site.Judged = true;
      Site.Destroyer = Destroyer;
      Site.ScopeExitVar = ScopeExitVar;
      QualType VT = SemaProfiles::handleValueType(OwnerT);
      const auto *RD = VT.isNull()
                           ? nullptr
                           : Ctx.getBaseElementType(VT)->getAsCXXRecordDecl();
      Site.Exempt = !RD || !RD->hasDefinition() || RD->hasTrivialDestructor();
      Site.Owner = Idx;
      BindingLeaf Leaf;
      Leaf.Entity = Idx;
      Site.Leaves.push_back(Leaf);
      Site.Affected.push_back(Idx);
      DSites.push_back(std::move(Site));
      BlockEvents.push_back(
          {DefAssignEventKind::Destroy, 0, At, (unsigned)(DSites.size() - 1)});
    };
    // What a marked pointer, reference, or handle bound to \p Src as \p T
    // refers to, as a ReseatTarget and its Aux, through the binding judgment's
    // operand, leaf walk, and resolver (AddBinding,
    // TrackedStorage::resolveFlowLeaf) so the two agree on the source's shape:
    // the one tracked entity a single leaf names (&u, &a.m, u for a
    // reference), what a single marked pointer or reference leaf refers to
    // (q, &*q, &r), the target every leaf agrees on, nothing identifiable
    // when the leaves disagree and one is tracked, and otherwise an anonymous
    // referent -- also for a decayed [[uninit]] array, which the binding judges
    // by the array's state but which names one element (P4222R2 §5.4).
    auto ReseatTargetFor =
        [&](const Expr *Src, QualType T) -> std::pair<ReseatTarget, unsigned> {
      std::pair<ReseatTarget, unsigned> Fresh{ReseatTarget::Fresh, 0};
      if (!Src || T.isNull() || T->isDependentType() ||
          isa<RecoveryExpr>(Src->IgnoreParens()))
        return Fresh;
      // A handle refers to what the leaves of its walk name: a tracked
      // handle's referent, or storage a transfer leaf names whole; a Partial
      // leaf or a subobject is an anonymous referent.
      if (SemaProfiles::isHandleType(T)) {
        using HandleLeaf = SemaProfiles::HandleLeaf;
        SmallVector<std::pair<ReseatTarget, unsigned>, 2> Targets;
        bool AnyTracked = false;
        // Every leaf is null: a std-table construction with no transfer
        // argument, or a null pointer.
        bool AllNull = true;
        SemaProfiles::HandleWalkMemo Memo;
        auto Visit = [&](auto &Self, const HandleLeaf &HL) -> void {
          const Expr *Peeled = HL.E->IgnoreParenImpCasts();
          AllNull &= HL.M == HandleLeaf::Mode::Handle
                         ? isa<CXXConstructExpr>(Peeled) &&
                               SemaProfiles::isStdOwnerType(Peeled->getType())
                         : HL.M == HandleLeaf::Mode::PointerValue &&
                               Peeled->isNullPointerConstant(
                                   Ctx, Expr::NPC_ValueDependentIsNotNull);
          if (HL.M == HandleLeaf::Mode::Handle) {
            AnyTracked |= S.Profiles().isFlowTrackedHandleLeaf(HL, DC);
            TrackedStorage::Resolution R = Storage.resolveHandleLeaf(HL);
            Targets.push_back(
                R.Entity && !R.Subobject
                    ? std::pair{ReseatTarget::ViaPointer, *R.Entity}
                    : Fresh);
            return;
          }
          bool AsPointerValue = HL.M == HandleLeaf::Mode::PointerValue;
          SemaProfiles::forEachTargetLeaf(
              HL.E, [&](const SemaProfiles::ShapeLeaf &Leaf) {
                AnyTracked |=
                    S.Profiles().isFlowTrackedLeaf(Leaf, AsPointerValue, DC);
                TrackedStorage::Resolution R =
                    Storage.resolveFlowLeaf(Leaf, AsPointerValue);
                if (HL.M == HandleLeaf::Mode::Range && !R.Entity) {
                  SemaProfiles::forEachHandleSourceLeaf(
                      Leaf.E,
                      [&](const HandleLeaf &Inner) {
                        // An offset range offsets what it refers to.
                        Self(Self, HandleLeaf{Inner.E, Inner.M,
                                              Inner.Partial || HL.Partial,
                                              Inner.Arms});
                      },
                      &Memo);
                  return;
                }
                bool DecayedArray =
                    AsPointerValue &&
                    SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms)
                        ->getType()
                        ->isArrayType();
                if (R.Entity && !R.Subobject && !HL.Partial && !DecayedArray)
                  Targets.push_back({Storage.Entities[*R.Entity].isPointee()
                                         ? ReseatTarget::ViaPointer
                                         : ReseatTarget::Entity,
                                     *R.Entity});
                else
                  Targets.push_back(Fresh);
              });
        };
        SemaProfiles::forEachHandleSourceLeaf(
            Src, [&](const HandleLeaf &HL) { Visit(Visit, HL); }, &Memo);
        // A null std::unique_ptr refers to nothing.
        if (AllNull && SemaProfiles::isUniqueOwnerType(T))
          return {ReseatTarget::Null, 0};
        if (Targets.empty())
          return Fresh;
        if (!llvm::all_equal(Targets))
          return AnyTracked ? std::pair{ReseatTarget::Unknown, 0u} : Fresh;
        return Targets.front();
      }
      bool AsPointerValue;
      const Expr *Operand =
          SemaProfiles::bindingSourceOperand(Src, T, AsPointerValue);
      if (!Operand)
        return Fresh;
      // Per leaf: the one tracked entity it names, what a marked pointer or
      // reference leaf refers to, or an anonymous referent.
      SmallVector<std::pair<ReseatTarget, unsigned>, 2> Targets;
      bool AnyTracked = false;
      SemaProfiles::forEachTargetLeaf(
          Operand, [&](const SemaProfiles::ShapeLeaf &Leaf) {
            AnyTracked |=
                S.Profiles().isFlowTrackedLeaf(Leaf, AsPointerValue, DC);
            std::pair<ReseatTarget, unsigned> RT = Fresh;
            TrackedStorage::Resolution R =
                Storage.resolveFlowLeaf(Leaf, AsPointerValue);
            if ((!AsPointerValue ||
                 !SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms)
                      ->getType()
                      ->isArrayType()) &&
                R.Entity && !R.Subobject)
              RT = {Storage.Entities[*R.Entity].isPointee()
                        ? ReseatTarget::ViaPointer
                        : ReseatTarget::Entity,
                    *R.Entity};
            // A tracked aggregate named whole is no one entity: its
            // referent is not identified.
            else if (!R.Entity) {
              std::pair<unsigned, unsigned> Part =
                  Storage.objectPart(Leaf, AsPointerValue);
              if (Part.first != Part.second)
                RT = {ReseatTarget::Unknown, 0};
            }
            Targets.push_back(RT);
          });
      if (Targets.empty())
        return Fresh;
      if (!llvm::all_equal(Targets))
        return AnyTracked ? std::pair{ReseatTarget::Unknown, 0u} : Fresh;
      return Targets.front();
    };
    // A member call on, or an alias of, the object \p Object -- the current
    // object (also when null) or a member of it, by value -- at \p At may
    // reseat the marked pointer members below it or destroy their pointees:
    // their referents become unknown.
    auto EscapeMemberPointers = [&](const Expr *Object, const Expr *At) {
      ArrayRef<MemberStep> Prefix;
      std::optional<SemaProfiles::FlowLeafShape> Shape;
      if (Object && !isCurrentObjectBase(Object)) {
        if (Object->getType()->isPointerType())
          return;
        Shape = SemaProfiles::flowLeafShape(Object);
        if (!Shape || !Shape->IsThis || !Shape->PointerMembers.empty() ||
            Shape->Subscript)
          return;
        Prefix = Shape->Members;
      }
      for (unsigned I = Storage.MemberPointerRange.first;
           I != Storage.MemberPointerRange.second; ++I) {
        ArrayRef<MemberStep> Steps = Storage.Entities[I].Steps;
        if (Steps.size() > Prefix.size() &&
            Steps.take_front(Prefix.size()) == Prefix)
          AppendReseat(I, At, ReseatTarget::Escaped);
      }
    };
    // A store to \p G: one Write (or ReadWrite) when every leaf names the
    // same whole entity; otherwise the chosen arm is unknown, so each named
    // whole entity is may-written and a compound store reads each leaf. A
    // store below a whole entity (a subobject) initializes nothing. Every
    // marked pointer a leaf names as its object is reseated to what
    // \p ReseatSource -- the assigned value; null for an increment or a
    // compound assignment -- refers to; a target naming several pointers, or
    // a marked pointer on only some of its arms, leaves each referent
    // unidentified (no binding is judged for such a target:
    // SemaProfiles::resolveAssignTargetMarking).
    auto AppendStore = [&](const Expr *G, const Expr *At, bool ReadsFirst,
                           const Expr *ReseatSource) {
      // The store designates its target's type.
      QualType Designated = G->getType();
      CollectLeaves(G, Designated);
      std::optional<unsigned> Same;
      bool AllSame = !Leaves.empty();
      for (const TrackedStorage::Resolution &R : Leaves) {
        if (!R.Entity || R.Subobject) {
          AllSame = false;
          break;
        }
        if (!Same)
          Same = R.Entity;
        else if (*Same != *R.Entity)
          AllSame = false;
      }
      auto Push = [&](DefAssignEventKind Kind, unsigned Idx) {
        DefAssignEvent Ev{Kind, Idx, At};
        Ev.Designated = Designated;
        BlockEvents.push_back(Ev);
      };
      if (AllSame) {
        Push(ReadsFirst && Storage.Entities[*Same].TrackReads
                 ? DefAssignEventKind::ReadWrite
                 : DefAssignEventKind::Write,
             *Same);
      } else {
        if (ReadsFirst)
          AppendReads(At);
        for (const TrackedStorage::Resolution &R : Leaves)
          if (R.Entity && !R.Subobject)
            Push(DefAssignEventKind::MayWrite, *R.Entity);
      }
      // A store naming a SubMember whole is that member's initialization.
      for (const TrackedStorage::Resolution &R : Leaves)
        if (R.Entity && R.Subobject && R.Sub.Exact) {
          DefAssignEvent Ev{Leaves.size() == 1 ? DefAssignEventKind::Write
                                               : DefAssignEventKind::MayWrite,
                            *R.Entity, At};
          Ev.Designated = Designated;
          Ev.Sub = R.Sub;
          BlockEvents.push_back(Ev);
        }
      SmallVector<unsigned, 2> Reseated;
      bool AllMarked = true;
      SemaProfiles::forEachTargetLeaf(
          G, [&](const SemaProfiles::ShapeLeaf &Leaf) {
            std::optional<unsigned> Idx = Storage.markedPointerObject(Leaf);
            if (!Idx)
              AllMarked = false;
            else if (!llvm::is_contained(Reseated, *Idx))
              Reseated.push_back(*Idx);
          });
      if (Reseated.empty())
        return;
      std::pair<ReseatTarget, unsigned> RT{ReseatTarget::Unknown, 0};
      if (Reseated.size() == 1 && AllMarked)
        RT = ReseatTargetFor(ReseatSource, G->getType());
      for (unsigned Idx : Reseated)
        AppendReseat(Idx, At, RT.first, RT.second);
    };
    // The mutable alias a binding of \p Src as \p T hands out (P4222R2
    // §4.3): `p` bound to a `T *&`, or `&p` bound to a `T **`, per leaf,
    // gives the pointer an anonymous referent of unknown state. A const
    // pointer cannot be reseated through either.
    auto AppendAliasEscapes = [&](const Expr *Src, QualType T, const Expr *At,
                                  const ParmVarDecl *Parm = nullptr) {
      if (!Src || T.isNull())
        return;
      // A pointer or reference to the current object or to a member
      // subobject reaches the marked pointer members below it, whose pointees
      // even a const alias may destroy or reseat.
      if ((T->isPointerType() || T->isReferenceType()) &&
          Storage.MemberPointerRange.first != Storage.MemberPointerRange.second)
        SemaProfiles::forEachTargetLeaf(
            Src, [&](const SemaProfiles::ShapeLeaf &Leaf) {
              const Expr *G =
                  SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms);
              if (T->isPointerType() && !isa<CXXThisExpr>(G)) {
                G = SemaProfiles::addressOfOperand(G);
                if (!G)
                  return;
              }
              if (isa<CXXThisExpr>(G) || G->getType()->isRecordType())
                EscapeMemberPointers(G, At);
            });
      // A non-const reference or pointer to a marked handle escapes its
      // referent: the alias may reseat it. The source of a copy or a move,
      // a transfer argument, and a reference a standard library function
      // takes keep no alias -- but std::swap, std::exchange, and
      // std::iter_swap exchange what it refers to, std::advance offsets it,
      // and a std::unique_ptr moved from is null.
      bool HandleByRef = T->isReferenceType() &&
                         !T.getNonReferenceType().isConstQualified() &&
                         SemaProfiles::isHandleType(T.getNonReferenceType());
      bool HandleByAddr = T->isPointerType() &&
                          !T->getPointeeType().isConstQualified() &&
                          SemaProfiles::isHandleType(T->getPointeeType());
      if (HandleByRef || HandleByAddr) {
        ReseatTarget RT = ReseatTarget::Escaped;
        if (const auto *FD =
                Parm ? dyn_cast<FunctionDecl>(Parm->getDeclContext())
                     : nullptr) {
          if (SemaProfiles::isStdCallable(FD, "advance")) {
            RT = ReseatTarget::Offset;
          } else if (SemaProfiles::isCopySource(Parm) ||
                     SemaProfiles::getCalleeLifecycleRoles(FD).transfersParam(
                         Parm)) {
            if (!T->isRValueReferenceType())
              return;
            RT = ReseatTarget::Null;
          } else if (SemaProfiles::isStdLibraryFunction(FD) &&
                     !SemaProfiles::isStdCallable(FD, "swap") &&
                     !SemaProfiles::isStdCallable(FD, "exchange") &&
                     !SemaProfiles::isStdCallable(FD, "iter_swap")) {
            return;
          }
        }
        SemaProfiles::forEachTargetLeaf(
            Src, [&](const SemaProfiles::ShapeLeaf &Leaf) {
              const Expr *G =
                  SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms);
              if (HandleByAddr) {
                G = SemaProfiles::addressOfOperand(G);
                if (!G)
                  return;
              }
              if (RT == ReseatTarget::Null &&
                  !SemaProfiles::isUniqueOwnerType(G->getType()))
                return;
              if (std::optional<unsigned> Idx =
                      Storage.markedHandleObject({G, Leaf.Arms}))
                AppendReseat(*Idx, At, RT);
            });
        return;
      }
      bool ByRef = SemaProfiles::pointerAliasKind(T) ==
                   SemaProfiles::PointerAliasKind::Mutable;
      bool ByAddr = T->isPointerType() &&
                    T->getPointeeType()->isPointerType() &&
                    !T->getPointeeType().isConstQualified();
      if (!ByRef && !ByAddr)
        return;
      SemaProfiles::forEachTargetLeaf(
          Src, [&](const SemaProfiles::ShapeLeaf &Leaf) {
            const Expr *G =
                SemaProfiles::ignoreTransparentCasts(Leaf.E, Leaf.Arms);
            if (ByAddr) {
              G = SemaProfiles::addressOfOperand(G);
              if (!G)
                return;
            }
            if (std::optional<unsigned> Idx =
                    Storage.markedPointerObject({G, Leaf.Arms}))
              AppendReseat(*Idx, At, ReseatTarget::Escaped);
          });
    };
    // A pointer, reference, or handle binding of \p Src as \p T, with the
    // parse-time funnel's kind, location, and target marking
    // (SemaProfiles::checkInitProfileBinding): a site judged at \p At against
    // the flow state when a leaf names tracked storage; the funnel judged
    // every other binding at parse time. A leaf no entity tracks is
    // classified by form.
    auto AddBindingAs = [&](K Kind, SourceLocation Loc, bool TargetMarked,
                            QualType T, const Expr *Src,
                            const NamedDecl *Subject, const Expr *At,
                            bool EveryPath, bool AsRange) {
      bool IsHandle = SemaProfiles::isHandleType(T);
      if (!Src || T.isNull() || T->isDependentType() ||
          (!T->isPointerType() && !T->isReferenceType() && !IsHandle) ||
          isa<RecoveryExpr>(Src->IgnoreParens()))
        return;
      BindingSite Site{
          Kind,    Loc, TargetMarked, T->isReferenceType() && !AsRange,
          Subject, {},  EveryPath};
      if (IsHandle || AsRange)
        Site.HandleType = T.getNonReferenceType();
      bool AnyTracked = false;
      // The derived-to-base casts above the leaves, which the leaf walk peels.
      BasePath OuterPath;
      // One leaf of the source, used as a pointer value or a glvalue; a leaf
      // no entity tracks takes \p Form's state.
      SemaProfiles::FlowLeafUse Use =
          TargetMarked ? SemaProfiles::FlowLeafUse::MarkedTarget
          : EveryPath  ? SemaProfiles::FlowLeafUse::ValueCopy
                       : SemaProfiles::FlowLeafUse::Plain;
      auto AddLeaf = [&](const SemaProfiles::ShapeLeaf &Leaf,
                         bool AsPointerValue,
                         llvm::function_ref<LeafState()> Form) {
        BindingLeaf L;
        if (S.Profiles().isFlowTrackedLeaf(Leaf, AsPointerValue, DC, Use)) {
          AnyTracked = true;
          JudgedLeaves.insert(Leaf.E);
          TrackedStorage::Resolution R =
              Storage.resolveFlowLeaf(Leaf, AsPointerValue);
          L.Entity = R.Entity;
          L.Sub = R.Sub;
          L.WholeLeaf = !R.Subobject;
          // A value copy through a pointer to data member reads what the
          // step may name.
          if (!L.Entity && Use == SemaProfiles::FlowLeafUse::ValueCopy &&
              R.Opaque.first != R.Opaque.second)
            L.Range = R.Opaque;
          else if (!L.Entity && Use != SemaProfiles::FlowLeafUse::Plain)
            L.Range = Storage.objectPart(Leaf, AsPointerValue, OuterPath, Use);
        }
        if (!L.Entity)
          L.State = Form();
        Site.Leaves.push_back(L);
      };
      if (IsHandle || AsRange) {
        // A handle's state comes from the storage its walk reaches
        // (SemaProfiles::forEachHandleSourceLeaf); a range's from its own
        // storage, else from the storage its walk reaches.
        using HandleLeaf = SemaProfiles::HandleLeaf;
        SemaProfiles::HandleWalkMemo Memo;
        auto Visit = [&](auto &Self, const HandleLeaf &HL) -> void {
          auto FormOf = [&](const SemaProfiles::ShapeLeaf &Leaf,
                            HandleLeaf::Mode M) {
            return formState(S.Profiles().classifyInitHandleLeaf(
                {Leaf.E, M, HL.Partial, Leaf.Arms}, D));
          };
          if (HL.M == HandleLeaf::Mode::Handle) {
            BindingLeaf L;
            if (S.Profiles().isFlowTrackedHandleLeaf(HL, DC)) {
              AnyTracked = true;
              JudgedLeaves.insert(HL.E);
              TrackedStorage::Resolution R = Storage.resolveHandleLeaf(HL);
              L.Entity = R.Entity;
              L.WholeLeaf = !R.Subobject;
            }
            if (!L.Entity)
              L.State = FormOf(HL.E, HL.M);
            Site.Leaves.push_back(L);
            return;
          }
          SemaProfiles::forEachTargetLeaf(
              HL.E, [&](const SemaProfiles::ShapeLeaf &Leaf) {
                if (HL.M != HandleLeaf::Mode::Range ||
                    S.Profiles().isFlowTrackedLeaf(
                        Leaf, /*AsPointerValue=*/false, DC,
                        TargetMarked ? SemaProfiles::FlowLeafUse::MarkedTarget
                                     : SemaProfiles::FlowLeafUse::Plain)) {
                  AddLeaf(Leaf, HL.M == HandleLeaf::Mode::PointerValue,
                          [&] { return FormOf(Leaf, HL.M); });
                  return;
                }
                if (Leaf.E->isGLValue()) {
                  LeafState Storage = FormOf(Leaf, HandleLeaf::Mode::Storage);
                  if (Storage != LeafState::Initialized) {
                    BindingLeaf L;
                    L.State = Storage;
                    Site.Leaves.push_back(L);
                    return;
                  }
                }
                SemaProfiles::forEachHandleSourceLeaf(
                    Leaf.E,
                    [&](const HandleLeaf &Inner) {
                      // An offset range offsets what it refers to.
                      Self(Self,
                           HandleLeaf{Inner.E, Inner.M,
                                      Inner.Partial || HL.Partial, Inner.Arms});
                    },
                    &Memo);
              });
        };
        if (AsRange)
          Visit(Visit, HandleLeaf{Src, HandleLeaf::Mode::Range});
        else
          SemaProfiles::forEachHandleSourceLeaf(
              Src, [&](const HandleLeaf &HL) { Visit(Visit, HL); }, &Memo);
      } else {
        bool AsPointerValue;
        const Expr *Operand =
            SemaProfiles::bindingSourceOperand(Src, T, AsPointerValue);
        if (!Operand)
          return;
        SemaProfiles::peelBaseCasts(Operand, OuterPath);
        SemaProfiles::forEachTargetLeaf(
            Operand, [&](const SemaProfiles::ShapeLeaf &Leaf) {
              AddLeaf(Leaf, AsPointerValue, [&] {
                return formState(S.Profiles().classifyInitBindingLeaf(
                    Leaf, T, D, /*ValueCopy=*/EveryPath && !TargetMarked));
              });
            });
      }
      if (!AnyTracked)
        return;
      Sites.push_back(std::move(Site));
      BlockEvents.push_back(
          {DefAssignEventKind::Binding, 0, At, (unsigned)(Sites.size() - 1)});
    };
    // AddBindingAs, and for an unmarked reference to a handle bound to
    // \p Target, the handle's binding too
    // (SemaProfiles::bindsHandleByReference).
    auto AddBinding = [&](K Kind, SourceLocation Loc, bool TargetMarked,
                          QualType T, const Expr *Src, const NamedDecl *Subject,
                          const Expr *At, bool EveryPath = false,
                          bool AsRange = false,
                          const ValueDecl *Target = nullptr) {
      AddBindingAs(Kind, Loc, TargetMarked, T, Src, Subject, At, EveryPath,
                   AsRange);
      if (!TargetMarked && !AsRange && !T.isNull() && !T->isDependentType() &&
          SemaProfiles::bindsHandleByReference(Kind, T, Target))
        AddBindingAs(Kind, Loc, /*TargetMarked=*/false,
                     T.getNonReferenceType().getUnqualifiedType(), Src, Subject,
                     At, /*EveryPath=*/false, /*AsRange=*/false);
    };
    // A read through (\p IsWrite false) or a store below tracked storage,
    // with the parse-time check's location (the glvalue for a read, the
    // operator for a store): a site when a leaf is flow-tracked (the
    // parse-time checks judged every other access). A std::byte access is
    // exempt (P4222R2 §4.6). A \p WholeRead (__builtin_bit_cast) reads a
    // whole [[uninit]] local as it reads a subobject of one.
    auto AppendAccess = [&](bool IsWrite, const Expr *G, QualType ValueTy,
                            SourceLocation Loc, const Expr *At,
                            bool WholeRead = false) {
      if (!G || Ctx.getBaseElementType(ValueTy)->isStdByteType() ||
          isa<RecoveryExpr>(G->IgnoreParens()))
        return;
      AccessSite Site;
      Site.Loc = Loc;
      bool AnyTracked = false, AnyPointee = false, AnyUninitObject = false,
           AnyCurrentObject = false;
      // The leaves classified by form, which the parse-time check judged
      // unless a leaf is flow-tracked.
      SmallVector<unsigned, 2> FormLeaves;
      // The access designates the glvalue's type (TrackedStorage::resolve).
      QualType Designated = G->getType();
      SemaProfiles::forEachTargetLeaf(
          G, [&](const SemaProfiles::ShapeLeaf &Leaf) {
            // An access below the current object is also one of its
            // lifetime, when that is tracked, judged on every path.
            if (Storage.CurrentObjectRoot) {
              std::optional<SemaProfiles::FlowLeafShape> Shape =
                  SemaProfiles::flowLeafShape(Leaf.E, Leaf.Arms);
              if (Shape && Shape->IsThis &&
                  (!Shape->Members.empty() || Shape->Subscript)) {
                BindingLeaf Root;
                Root.Entity = Storage.CurrentObjectRoot;
                Site.Leaves.push_back(Root);
                AnyCurrentObject = true;
              }
            }
            BindingLeaf L;
            if (S.Profiles().isFlowTrackedLeaf(Leaf,
                                               /*AsPointerValue=*/false, DC)) {
              AnyTracked = true;
              JudgedLeaves.insert(Leaf.E);
              TrackedStorage::Resolution R = Storage.resolveFlowLeaf(
                  Leaf, /*AsPointerValue=*/false, Designated);
              if (R.Entity) {
                const TrackedEntity &Ent = Storage.Entities[*R.Entity];
                bool Pointee = Ent.isPointee();
                // A subobject of an [[uninit]] local or of a read-tracked
                // member tracked whole; a store to part of any read-tracked
                // member is one of an [[uninit]] object too (a read of a
                // scalar one is the member's Read event).
                bool UninitObject =
                    (R.Subobject || WholeRead) &&
                    (Ent.K == TrackedEntity::Kind::WholeLocal ||
                     (!Pointee && Ent.TrackReads && (IsWrite || Ent.Whole)));
                if (UninitObject || (Pointee && (!IsWrite || R.Subobject))) {
                  L.Entity = R.Entity;
                  L.Sub = R.Sub;
                  L.WholeLeaf = !R.Subobject;
                  AnyPointee |= Pointee;
                  AnyUninitObject |= UninitObject;
                } else if (Pointee) {
                  // A whole store through a marked pointer or reference: a
                  // subobject store where the storage it refers to is not
                  // covered by the designation.
                  L.Entity = R.Entity;
                  L.WholeIf = Designated;
                } else {
                  // A whole [[uninit]] local's read, a tracked member, or a
                  // whole-entity store: not this rule's.
                  L.State = LeafState::Initialized;
                }
                Site.Leaves.push_back(L);
                return;
              }
            }
            L.State = formState(S.Profiles().classifyInitAccessLeaf(
                Leaf,
                IsWrite ? SemaProfiles::InitAccessKind::Write
                        : SemaProfiles::InitAccessKind::Read,
                D));
            FormLeaves.push_back(Site.Leaves.size());
            Site.Leaves.push_back(L);
          });
      if (!AnyTracked && !AnyCurrentObject)
        return;
      if (!AnyTracked)
        for (unsigned I : FormLeaves)
          Site.Leaves[I].State = LeafState::Initialized;
      bool UninitObjectOnly = AnyUninitObject && !AnyPointee;
      if (AnyCurrentObject && !AnyUninitObject && !AnyPointee)
        Site.Select = 2;
      else
        Site.Select = IsWrite ? (UninitObjectOnly ? 0u : 1u)
                              : (UninitObjectOnly ? 1u : 0u);
      const Expr *Target = G->IgnoreParenImpCasts();
      Site.Flag = IsWrite && !isa<MemberExpr>(Target) &&
                  !SemaProfiles::asDataMemberPointerAccess(Target);
      ASites.push_back(std::move(Site));
      BlockEvents.push_back({IsWrite ? DefAssignEventKind::SubobjectWrite
                                     : DefAssignEventKind::ReadThrough,
                             0, At, (unsigned)(ASites.size() - 1)});
    };
    // The argument bindings of the call-like \p CallLike
    // (forEachArgumentBinding) of a callee with \p Roles: a VariadicArgument
    // binding per variadic argument, a Parameter binding per declared
    // parameter other than an initializing one, a range's end iterator, a
    // transfer parameter, or a handle an assignment takes by value --
    // none for a [[now_uninit]] or storage-release callee, whose parameters
    // run the destroy rules instead -- and the mutable aliases each argument
    // hands out.
    auto AppendArgumentBindings =
        [&](const Expr *CallLike,
            const SemaProfiles::CalleeLifecycleRoles &Roles) {
          forEachArgumentBinding(
              Ctx, CallLike,
              [&](const ParmVarDecl *Parm, QualType T, const Expr *Arg,
                  bool Variadic) {
                SourceLocation Loc = Arg->IgnoreImplicit()->getExprLoc();
                // A role parameter taking a class-type range by reference
                // judges the range, and hands out no alias of it.
                bool AsRange =
                    Parm && T->isReferenceType() &&
                    SemaProfiles::isHandleType(T.getNonReferenceType()) &&
                    Roles.roleOf(Parm) != SemaProfiles::KnownParamRole::None;
                if (Variadic)
                  AddBinding(K::VariadicArgument, Loc, /*TargetMarked=*/false,
                             T, Arg, nullptr, CallLike);
                else if (!Parm ||
                         !(Roles.destroysParam(Parm) ||
                           Roles.releasesParam(Parm, /*TrustedOnly=*/false) ||
                           Roles.isInitializingParam(Parm) ||
                           Roles.isEndParam(Parm) ||
                           Roles.transfersParam(Parm) ||
                           (SemaProfiles::isHandleType(T) &&
                            SemaProfiles::isAssignmentSource(Parm))))
                  AddBinding(K::Parameter, Loc,
                             Parm && Roles.isMarkedParam(Parm), T, Arg, nullptr,
                             CallLike, SemaProfiles::isValueCopySource(Parm),
                             AsRange, Parm);
                if (!AsRange)
                  AppendAliasEscapes(Arg, T, CallLike, Parm);
              });
        };
    // A call of \p Callee (null when there is no declared callee) through
    // the call-like \p CallLike: its argument bindings and its lifecycle
    // effects, \p DestroyedObject's destruction included
    // (appendLifecycleCallEvents).
    auto AppendCallEvents = [&](const Expr *CallLike,
                                const FunctionDecl *Callee,
                                const Expr *DestroyedObject = nullptr) {
      SemaProfiles::CalleeLifecycleRoles Roles =
          Callee ? SemaProfiles::getCalleeLifecycleRoles(Callee)
                 : SemaProfiles::CalleeLifecycleRoles();
      AppendArgumentBindings(CallLike, Roles);
      appendLifecycleCallEvents(S, D, CallLike, Roles, Storage, BlockEvents,
                                DSites, JudgedLeaves, DestroyedObject);
    };
    // The element bindings of an aggregate initializer of type \p T: each
    // base and field of a record (unnamed bit-fields consume no initializer)
    // is a member binding, each element of an array, vector, or complex a
    // bare element binding. A
    // field's default member initializer and a value-initialized filler
    // have no written source.
    auto AppendAggregateBindings = [&](QualType T, ArrayRef<Expr *> Inits,
                                       const FieldDecl *UnionField) {
      auto Judge = [&](K Kind, const FieldDecl *F, QualType ElemT,
                       const Expr *Init) {
        if (!Init || isa<ImplicitValueInitExpr, CXXDefaultInitExpr>(Init))
          return;
        AddBinding(Kind, Init->IgnoreImplicit()->getExprLoc(),
                   F && F->hasAttr<RefToUninitAttr>(), ElemT, Init, nullptr,
                   Init, /*EveryPath=*/false, /*AsRange=*/false, F);
        AppendAliasEscapes(Init, ElemT, Init);
      };
      if (const RecordDecl *RD = T->getAsRecordDecl()) {
        RD = RD->getDefinition();
        if (!RD)
          return;
        if (RD->isUnion()) {
          if (UnionField && !Inits.empty())
            Judge(K::DataMember, UnionField, UnionField->getType(), Inits[0]);
          return;
        }
        // A base subobject, which cannot carry the marker, consumes each
        // leading initializer.
        unsigned I = 0;
        if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD))
          for (const CXXBaseSpecifier &Base : CRD->bases())
            if (I < Inits.size())
              Judge(K::DataMember, nullptr, Base.getType(), Inits[I++]);
        for (const FieldDecl *F : RD->fields()) {
          if (F->isUnnamedBitField())
            continue;
          if (I >= Inits.size())
            break;
          Judge(K::DataMember, F, F->getType(), Inits[I++]);
        }
        return;
      }
      QualType ElemT;
      if (const ArrayType *AT = Ctx.getAsArrayType(T))
        ElemT = AT->getElementType();
      else if (const auto *VT = T->getAs<VectorType>())
        ElemT = VT->getElementType();
      else if (const auto *CT = T->getAs<ComplexType>())
        ElemT = CT->getElementType();
      else
        return;
      for (const Expr *Init : Inits)
        Judge(K::AggregateElement, nullptr, ElemT, Init);
    };

    // A marked by-value std::unique_ptr parameter is destroyed when the
    // function returns, and with it its referent (a coroutine's parameters
    // live in its frame).
    if (const auto *FD = dyn_cast<FunctionDecl>(D);
        FD && B == &cfg.getExit() && FD->getBody() &&
        !isa<CoroutineBodyStmt>(FD->getBody()))
      for (const ParmVarDecl *P : FD->parameters())
        if (std::optional<unsigned> Idx = Storage.markedHandleObject(P);
            Idx && SemaProfiles::isUniqueOwnerType(P->getType()))
          AppendOwnerDestroy(*Idx, P->getType(), FD->getBody()->getEndLoc(),
                             SemaProfiles::InitDestroyer::ScopeExit, P,
                             nullptr);
    for (const CFGElement &Elem : *B) {
      if (auto OptInit = Elem.getAs<CFGInitializer>()) {
        // A written member initializer binds its member at this point in
        // execution order, after its init expression's events above it, and
        // in a constructor whose members are tracked (\p Ctor) then assigns
        // it; a written base initializer (e.g. `: Base{1}`) gives the tracked
        // members of that constructor-less base subtree their values.
        const CXXCtorInitializer *CI = OptInit->getInitializer();
        // A marked pointer member's default member initializer reseats it.
        if (!CI->isWritten()) {
          if (const FieldDecl *F =
                  CI->isAnyMemberInitializer() ? CI->getAnyMember() : nullptr)
            if (std::optional<unsigned> Root =
                    Storage.memberPointerRoot(MemberStep{BasePath(), F})) {
              const Expr *Init = CI->getInit();
              if (const auto *DIE = dyn_cast<CXXDefaultInitExpr>(Init))
                Init = DIE->getExpr();
              std::pair<ReseatTarget, unsigned> RT =
                  ReseatTargetFor(Init, F->getType());
              AppendReseat(*Root, CI->getInit(), RT.first, RT.second);
            }
          continue;
        }
        // The marked pointer members below a member or base its written
        // initializer constructs refer to what that construction set.
        for (unsigned Idx = Storage.MemberPointerRange.first;
             Idx != Storage.MemberPointerRange.second; ++Idx) {
          ArrayRef<MemberStep> Steps = Storage.Entities[Idx].Steps;
          const MemberStep &First = Steps.front();
          bool Below =
              CI->isAnyMemberInitializer()
                  ? Steps.size() > 1 && First.Bases.empty() &&
                        First.Field->getCanonicalDecl() ==
                            CI->getAnyMember()->getCanonicalDecl()
                  : CI->isBaseInitializer() && !First.Bases.empty() &&
                        CI->getBaseClass()->getAsCXXRecordDecl() &&
                        First.Bases.front() == CI->getBaseClass()
                                                   ->getAsCXXRecordDecl()
                                                   ->getCanonicalDecl();
          if (!Below)
            continue;
          std::pair<ReseatTarget, unsigned> RT{ReseatTarget::Unknown, 0};
          if (const Expr *Elem = CI->isAnyMemberInitializer()
                                     ? aggregateInitializerFor(
                                           CI->getInit(), Steps.drop_front())
                                     : nullptr)
            RT = ReseatTargetFor(Elem, Steps.back().Field->getType());
          AppendReseat(Idx, CI->getInit(), RT.first, RT.second);
        }
        // A base subobject cannot carry the marker.
        if (CI->isBaseInitializer())
          AddBinding(K::DataMember, CI->getSourceLocation(),
                     /*TargetMarked=*/false, QualType(CI->getBaseClass(), 0),
                     CI->getInit(), nullptr, CI->getInit());
        if (CI->isAnyMemberInitializer()) {
          const FieldDecl *F = CI->getAnyMember();
          if (!F)
            continue;
          AddBinding(K::DataMember, CI->getMemberLocation(),
                     F->hasAttr<RefToUninitAttr>(), F->getType(), CI->getInit(),
                     nullptr, CI->getInit(), /*EveryPath=*/false,
                     /*AsRange=*/false, F);
          AppendAliasEscapes(CI->getInit(), F->getType(), CI->getInit());
          // A marked pointer member refers to what its initializer names.
          MemberStep Step{BasePath(), F};
          if (std::optional<unsigned> Root =
                  Storage.memberPointerRoot(ArrayRef(Step))) {
            std::pair<ReseatTarget, unsigned> RT =
                ReseatTargetFor(CI->getInit(), F->getType());
            AppendReseat(*Root, CI->getInit(), RT.first, RT.second);
          }
          if (!Ctor)
            continue;
          // Every entity whose path starts at that member.
          for (unsigned Idx = Storage.CurrentObjectRange.first;
               Idx != Storage.CurrentObjectRange.second; ++Idx) {
            const MemberStep &First = Storage.Entities[Idx].Steps.front();
            if (First.Bases.empty() && First.Field &&
                First.Field->getCanonicalDecl() == F->getCanonicalDecl())
              BlockEvents.push_back(
                  {DefAssignEventKind::Write, Idx, CI->getInit()});
          }
        } else if (Ctor && CI->isBaseInitializer()) {
          // Every entity below that base: its path starts with the base.
          const auto *BRD = CI->getBaseClass()->getAsCXXRecordDecl();
          if (!BRD)
            continue;
          for (unsigned Idx = Storage.CurrentObjectRange.first;
               Idx != Storage.CurrentObjectRange.second; ++Idx) {
            const BasePath &Bases = Storage.Entities[Idx].Steps.front().Bases;
            if (!Bases.empty() && Bases.front() == BRD->getCanonicalDecl())
              BlockEvents.push_back(
                  {DefAssignEventKind::Write, Idx, CI->getInit()});
          }
        }
        continue;
      }
      if (auto Alloc = Elem.getAs<CFGNewAllocator>()) {
        // The allocation function runs after the placement arguments and
        // before the initializer: the placement arguments are a call of it.
        const CXXNewExpr *NE = Alloc->getAllocatorExpr();
        if (!Ctor || BodyStmts.count(NE))
          AppendCallEvents(NE, NE->getOperatorNew());
        continue;
      }
      if (auto Dtor = Elem.getAs<CFGAutomaticObjDtor>()) {
        // The scope-exit destructor of an [[uninit]] local of a type with a
        // non-trivial destructor destroys it (P4222R2 §4.6), reported where
        // the scope ends. Member and base destructors are not judged.
        const VarDecl *V = Dtor->getVarDecl();
        // A marked std::unique_ptr's destructor destroys its referent.
        if (std::optional<unsigned> Idx = Storage.markedHandleObject(V);
            Idx && SemaProfiles::isUniqueOwnerType(V->getType())) {
          const Stmt *Trigger = Dtor->getTriggerStmt();
          AppendOwnerDestroy(*Idx, V->getType(),
                             Trigger ? Trigger->getEndLoc() : V->getLocation(),
                             SemaProfiles::InitDestroyer::ScopeExit, V,
                             nullptr);
          continue;
        }
        // The scope exit of a read-tracked aggregate local destroys its
        // tracked whole members with non-trivial destructors.
        if (auto RIt = Storage.LocalRange.find(V);
            RIt != Storage.LocalRange.end()) {
          for (unsigned I = RIt->second.first; I != RIt->second.second; ++I) {
            const TrackedEntity &Member = Storage.Entities[I];
            const auto *MRD =
                Ctx.getBaseElementType(Member.Type)->getAsCXXRecordDecl();
            if (!Member.Whole || !Member.TrackReads || !MRD ||
                !MRD->hasDefinition() || MRD->hasTrivialDestructor())
              continue;
            DestroySite Site;
            const Stmt *Trigger = Dtor->getTriggerStmt();
            Site.Loc = Trigger ? Trigger->getEndLoc() : V->getLocation();
            Site.Judged = true;
            Site.Destroyer = SemaProfiles::InitDestroyer::ScopeExit;
            Site.ScopeExitVar = V;
            BindingLeaf Leaf;
            Leaf.Entity = I;
            Site.Leaves.push_back(Leaf);
            Site.Affected.push_back(I);
            DSites.push_back(std::move(Site));
            BlockEvents.push_back({DefAssignEventKind::Destroy, 0, nullptr,
                                   (unsigned)(DSites.size() - 1)});
          }
          continue;
        }
        auto It = Storage.LocalEntity.find(V);
        if (It == Storage.LocalEntity.end() ||
            Storage.Entities[It->second].K != TrackedEntity::Kind::WholeLocal)
          continue;
        const auto *RD =
            Ctx.getBaseElementType(V->getType())->getAsCXXRecordDecl();
        if (!RD || !RD->hasDefinition() || RD->hasTrivialDestructor())
          continue;
        // A declaration whose marker a marker rule rejects -- one with an
        // initializer or a default-initialization that runs code --
        // constructed the local.
        if (!S.Profiles().isConsistentUninitMarker(V))
          continue;
        DestroySite Site;
        const Stmt *Trigger = Dtor->getTriggerStmt();
        Site.Loc = Trigger ? Trigger->getEndLoc() : V->getLocation();
        Site.Judged = true;
        Site.Destroyer = SemaProfiles::InitDestroyer::ScopeExit;
        Site.ScopeExitVar = V;
        BindingLeaf Leaf;
        Leaf.Entity = It->second;
        Site.Leaves.push_back(Leaf);
        Site.Affected.push_back(It->second);
        DSites.push_back(std::move(Site));
        BlockEvents.push_back({DefAssignEventKind::Destroy, 0, nullptr,
                               (unsigned)(DSites.size() - 1)});
        continue;
      }
      auto CS = Elem.getAs<CFGStmt>();
      if (!CS)
        continue;
      const Stmt *St = CS->getStmt();
      // A constructor's statement outside its body and written initializers
      // (an NSDMI subtree) contributes nothing.
      if (Ctor && !BodyStmts.count(St))
        continue;
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(St)) {
        if (ICE->getCastKind() != CK_LValueToRValue)
          continue;
        CollectLeaves(ICE->getSubExpr());
        AppendReads(ICE);
        AppendAccess(/*IsWrite=*/false, ICE->getSubExpr(), ICE->getType(),
                     ICE->getSubExpr()->getExprLoc(), ICE);
      } else if (const auto *BO = dyn_cast<BinaryOperator>(St)) {
        if (!BO->isAssignmentOp())
          continue;
        // A compound assignment reads its old value with no
        // lvalue-to-rvalue node; every assignment then stores.
        if (BO->isCompoundAssignmentOp())
          AppendAccess(/*IsWrite=*/false, BO->getLHS(), BO->getLHS()->getType(),
                       BO->getLHS()->getExprLoc(), BO);
        AppendAccess(/*IsWrite=*/true, BO->getLHS(), BO->getLHS()->getType(),
                     BO->getOperatorLoc(), BO);
        // A pointer assignment is judged by the assigned-to pointer's
        // marking; a target of unknown marking checks neither direction
        // (SemaProfiles::resolveAssignTargetMarking).
        if (BO->getOpcode() == BO_Assign &&
            BO->getLHS()->getType()->isPointerType()) {
          if (std::optional<bool> Marked =
                  SemaProfiles::resolveAssignTargetMarking(BO->getLHS()))
            AddBinding(K::PointerAssignment, BO->getOperatorLoc(), *Marked,
                       BO->getLHS()->getType(), BO->getRHS(), nullptr, BO);
          AppendAliasEscapes(BO->getRHS(), BO->getLHS()->getType(), BO);
        }
        AppendStore(BO->getLHS(), BO,
                    /*ReadsFirst=*/BO->isCompoundAssignmentOp(),
                    BO->getOpcode() == BO_Assign ? BO->getRHS() : nullptr);
      } else if (const auto *UO = dyn_cast<UnaryOperator>(St)) {
        // A built-in ++m / m++ / --m / m-- reads the old value and then
        // writes, but unlike -m / !m it carries no lvalue-to-rvalue cast, so
        // the Read arm never sees it: a ReadWrite.
        if (!UO->isIncrementDecrementOp())
          continue;
        AppendAccess(/*IsWrite=*/false, UO->getSubExpr(),
                     UO->getSubExpr()->getType(),
                     UO->getSubExpr()->getExprLoc(), UO);
        AppendAccess(/*IsWrite=*/true, UO->getSubExpr(),
                     UO->getSubExpr()->getType(), UO->getOperatorLoc(), UO);
        AppendStore(UO->getSubExpr(), UO, /*ReadsFirst=*/true,
                    /*ReseatSource=*/nullptr);
      } else if (const auto *CE = dyn_cast<CallExpr>(St)) {
        const FunctionDecl *Callee = CE->getDirectCallee();
        // A member operator called through CXXOperatorCallExpr receives the
        // object as argument 0 -- for a C++23 static operator too, whose
        // object argument is evaluated but binds no parameter.
        const CXXMethodDecl *ObjectMethod = nullptr;
        const Expr *Object = nullptr;
        if (isa<CXXOperatorCallExpr>(CE)) {
          if (const auto *MD = dyn_cast_or_null<CXXMethodDecl>(Callee);
              MD && !MD->isExplicitObjectMemberFunction() && !MD->isStatic() &&
              CE->getNumArgs() > 0) {
            ObjectMethod = MD;
            Object = CE->getArg(0);
          }
        } else if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE)) {
          if (const CXXMethodDecl *MD = MCE->getMethodDecl();
              MD && !MD->isStatic() && !MD->isExplicitObjectMemberFunction()) {
            ObjectMethod = MD;
            Object = MCE->getImplicitObjectArgument();
          }
        }
        // The implicit object parameter: a pointer for an arrow call, a
        // reference to the object otherwise. An explicit destructor call, and
        // a pseudo-destructor call, destroys its object instead
        // (SemaProfiles::checkInitProfileDestructorCall).
        const Expr *DestroyedObject = nullptr;
        if (ObjectMethod && Object && isa<CXXDestructorDecl>(ObjectMethod)) {
          DestroyedObject = Object;
        } else if (ObjectMethod && Object &&
                   !SemaProfiles::isStdArrayAddressMember(ObjectMethod)) {
          QualType OT = Object->getType();
          QualType T =
              OT->isPointerType()
                  ? OT
                  : Ctx.getLValueReferenceType(OT.getNonReferenceType());
          // A defaulted comparison reads its object whole, and a std::array's
          // element access reads an element, as a built-in array's does: on
          // every path.
          AddBinding(K::ObjectArgument, Object->IgnoreImplicit()->getExprLoc(),
                     /*TargetMarked=*/false, T, Object, ObjectMethod, CE,
                     SemaProfiles::isDefaultedComparison(ObjectMethod) ||
                         SemaProfiles::isStdArrayElementMember(ObjectMethod));
          EscapeMemberPointers(Object, CE);
        }
        if (const auto *PDE = dyn_cast<CXXPseudoDestructorExpr>(
                CE->getCallee()->IgnoreParens()))
          DestroyedObject = PDE->getBase();
        // A destructor call on a marked handle destroys the handle object,
        // whose storage no entity tracks -- a std::unique_ptr's destroys its
        // referent too.
        if (std::optional<unsigned> Idx =
                DestroyedObject
                    ? Storage.markedHandleObject({DestroyedObject, {}})
                    : std::nullopt) {
          QualType OwnerT = DestroyedObject->IgnoreParenImpCasts()->getType();
          if (SemaProfiles::isUniqueOwnerType(OwnerT))
            AppendOwnerDestroy(*Idx, OwnerT, CE->getExprLoc(),
                               SemaProfiles::InitDestroyer::DestructorCall,
                               nullptr, CE);
          DestroyedObject = nullptr;
        }
        // A handle reseat is judged against the handle's marking
        // (SemaProfiles::checkInitProfileHandleReseat).
        const Expr *ReseatObj = nullptr, *ReseatSrc = nullptr;
        SourceLocation ReseatLoc;
        bool IsReseat =
            SemaProfiles::getHandleReseat(CE, ReseatObj, ReseatSrc, ReseatLoc);
        if (IsReseat)
          if (std::optional<bool> Marked =
                  SemaProfiles::resolveHandleReseatMarking(ReseatObj))
            AddBinding(K::PointerAssignment, ReseatLoc, *Marked,
                       SemaProfiles::handleReseatType(ReseatObj), ReseatSrc,
                       nullptr, CE);
        AppendCallEvents(CE, Callee, DestroyedObject);
        // A member call on a marked local handle changes what it refers to:
        // a reseat names its source's referent, a reset or an assignment of
        // anything but a class an anonymous one, an offset (++, --, +=, -=)
        // a part of it, and any other non-const call one of unknown state --
        // but a call deriving a pointer or reference from it and a release.
        // An object naming several handles (a conditional) reseats each to
        // an unidentified referent.
        if (ObjectMethod && Object && !Object->getType()->isPointerType() &&
            !isa<CXXDestructorDecl>(ObjectMethod)) {
          SmallVector<unsigned, 2> Handles;
          unsigned NumLeaves = 0;
          SemaProfiles::forEachTargetLeaf(
              Object, [&](const SemaProfiles::ShapeLeaf &Leaf) {
                ++NumLeaves;
                if (std::optional<unsigned> Idx =
                        Storage.markedHandleObject(Leaf))
                  Handles.push_back(*Idx);
              });
          bool Several = NumLeaves > 1;
          for (unsigned Idx : Handles) {
            OverloadedOperatorKind OO = ObjectMethod->getOverloadedOperator();
            bool Written = llvm::any_of(CE->arguments(), [&](const Expr *A) {
              return A != Object && !isa<CXXDefaultArgExpr>(A);
            });
            bool IsReset = ObjectMethod->getIdentifier() &&
                           ObjectMethod->getName() == "reset";
            bool IsRelease = ObjectMethod->getIdentifier() &&
                             ObjectMethod->getName() == "release" &&
                             SemaProfiles::isUniqueOwnerType(
                                 Object->IgnoreParenImpCasts()->getType());
            const Expr *DerivingObj;
            bool Element;
            bool IsOwner = SemaProfiles::isUniqueOwnerType(
                Object->IgnoreParenImpCasts()->getType());
            // A std::unique_ptr's reset or reassignment destroys its referent
            // first.
            if ((IsReseat || OO == OO_Equal || IsReset) && !Several && IsOwner)
              AppendOwnerDestroy(Idx, Object->IgnoreParenImpCasts()->getType(),
                                 CE->getExprLoc(),
                                 SemaProfiles::InitDestroyer::OwnerReset,
                                 nullptr, CE);
            if (IsReseat || OO == OO_Equal || (IsReset && !Written)) {
              // An owner reset or assigned null refers to nothing.
              std::pair<ReseatTarget, unsigned> RT{
                  IsOwner ? ReseatTarget::Null : ReseatTarget::Fresh, 0};
              if (IsReseat)
                RT = ReseatTargetFor(
                    ReseatSrc, ReseatObj->getType().getNonReferenceType());
              if (Several)
                RT = {ReseatTarget::Unknown, 0};
              AppendReseat(Idx, CE, RT.first, RT.second);
            } else if (OO == OO_PlusPlus || OO == OO_MinusMinus ||
                       OO == OO_PlusEqual || OO == OO_MinusEqual) {
              AppendReseat(Idx, CE, ReseatTarget::Offset);
            } else if (IsRelease) {
              // release() hands the referent to its result.
              AppendReseat(Idx, CE,
                           Several ? ReseatTarget::Unknown
                                   : ReseatTarget::Null);
            } else if (!ObjectMethod->isConst() &&
                       !SemaProfiles::isHandleDerefCall(CE, DerivingObj,
                                                        Element)) {
              AppendReseat(Idx, CE, ReseatTarget::Escaped);
            }
          }
        }
      } else if (const auto *CCE = dyn_cast<CXXConstructExpr>(St)) {
        // A constructor call's parameter bindings, the copy of a class object
        // out of tracked storage included; a std-table construction's transfer
        // arguments are judged where the handle lands.
        AppendArgumentBindings(
            CCE, SemaProfiles::getCalleeLifecycleRoles(CCE->getConstructor()));
      } else if (const auto *BBC = dyn_cast<BuiltinBitCastExpr>(St)) {
        // __builtin_bit_cast reads its operand whole, with no
        // lvalue-to-rvalue node.
        const Expr *Op = BBC->getSubExpr();
        CollectLeaves(Op);
        AppendReads(BBC);
        AppendAccess(/*IsWrite=*/false, Op, Op->getType(), Op->getExprLoc(),
                     BBC, /*WholeRead=*/true);
      } else if (const auto *ME = dyn_cast<ObjCMessageExpr>(St)) {
        // A message's parameter bindings; a method carries no lifecycle
        // attribute.
        AppendArgumentBindings(ME, SemaProfiles::CalleeLifecycleRoles());
      } else if (const auto *VAE = dyn_cast<VAArgExpr>(St)) {
        // A class-type va_list is bound to a reference parameter in C++
        // (Sema::BuildVAArgExpr).
        const Expr *List = VAE->getSubExpr();
        if (!VAE->isMicrosoftABI() && Ctx.getLangOpts().CPlusPlus &&
            List->getType()->isRecordType())
          AddBinding(K::Parameter, List->getExprLoc(), /*TargetMarked=*/false,
                     Ctx.getLValueReferenceType(List->getType()), List, nullptr,
                     VAE);
      } else if (const auto *LE = dyn_cast<LambdaExpr>(St)) {
        if (Ctor)
          appendThisCaptureLambdaReadEvents(LE, Storage, StarThisCopyTrusted,
                                            BlockEvents);
        appendByCopyCaptureReadEvents(LE, Storage, BlockEvents);
        // An init-capture is a variable binding; a by-reference capture
        // binds an unmarked reference to the variable and hands out a
        // mutable alias of a marked pointer; a by-copy capture copies a
        // pointer into a closure field, which cannot carry the marker.
        auto InitIt = LE->capture_init_begin();
        for (const LambdaCapture &C : LE->captures()) {
          const Expr *Init =
              InitIt != LE->capture_init_end() ? *InitIt++ : nullptr;
          // A this capture reaches the marked pointer members.
          if (C.capturesThis())
            EscapeMemberPointers(/*Object=*/nullptr, LE);
          if (!C.capturesVariable())
            continue;
          const auto *V = dyn_cast<VarDecl>(C.getCapturedVar());
          if (!V)
            continue;
          if (V->isInitCapture()) {
            AddBinding(K::Variable, C.getLocation(),
                       V->hasAttr<RefToUninitAttr>(), V->getType(),
                       V->getInit(), nullptr, LE, /*EveryPath=*/false,
                       /*AsRange=*/false, V);
            AppendAliasEscapes(V->getInit(), V->getType(), LE);
            continue;
          }
          QualType VT = V->getType().getNonReferenceType();
          if (C.getCaptureKind() == LCK_ByRef) {
            AddBinding(K::ByRefCapture, C.getLocation(),
                       /*TargetMarked=*/false, Ctx.getLValueReferenceType(VT),
                       Init, V, LE);
            if (!VT.isConstQualified()) {
              std::optional<unsigned> Idx = Storage.markedPointerObject(V);
              if (!Idx)
                Idx = Storage.markedHandleObject(V);
              if (Idx)
                AppendReseat(*Idx, LE, ReseatTarget::Escaped);
            }
          } else if (C.getCaptureKind() == LCK_ByCopy) {
            AddBinding(K::ByCopyCapture, C.getLocation(),
                       /*TargetMarked=*/false, VT, Init, nullptr, LE);
          }
        }
      } else if (const auto *DRE = dyn_cast<DeclRefExpr>(St)) {
        if (Benign.count(DRE))
          continue;
        std::pair<unsigned, unsigned> Range = Storage.wholeObject(DRE);
        if (Range.first != Range.second)
          BlockEvents.push_back({DefAssignEventKind::AggregateEscape,
                                 Range.first, DRE, Range.second});
      } else if (const auto *DS = dyn_cast<DeclStmt>(St)) {
        for (const Decl *Dcl : DS->decls()) {
          const auto *V = dyn_cast<VarDecl>(Dcl);
          if (!V || V->isInvalidDecl())
            continue;
          // A tracked object's storage is a new object, unassigned, each time
          // its declaration runs.
          if (auto It = Storage.LocalEntity.find(V);
              It != Storage.LocalEntity.end() &&
              Storage.Entities[It->second].K == TrackedEntity::Kind::WholeLocal)
            BlockEvents.push_back(
                {DefAssignEventKind::Reset, It->second, nullptr});
          if (auto It = Storage.LocalRange.find(V);
              It != Storage.LocalRange.end())
            BlockEvents.push_back({DefAssignEventKind::Reset, It->second.first,
                                   nullptr, It->second.second});
          // The variable's own binding (a decomposition's hidden variable
          // included); the DeclStmt element follows its initializer's
          // subexpression elements, so the judgment sees their effects. An
          // implicit handle local (range-for's __begin) stands for its
          // initializer, which the handle walk sees through, and no implicit
          // local (range-for's __range) hands out an alias of its own.
          if (const Expr *Init = V->getInit()) {
            if (!V->isImplicit() || !SemaProfiles::isHandleType(V->getType()))
              AddBinding(K::Variable, V->getLocation(),
                         V->hasAttr<RefToUninitAttr>(), V->getType(), Init,
                         nullptr, Init, /*EveryPath=*/false, /*AsRange=*/false,
                         V);
            if (!V->isImplicit())
              AppendAliasEscapes(Init, V->getType(), Init);
            // A declared marked pointer or reference refers to what its
            // initializer names; a __block one can be reseated by a block
            // body the analysis does not see.
            if (auto It = Storage.LocalEntity.find(V);
                It != Storage.LocalEntity.end() &&
                Storage.Entities[It->second].K ==
                    TrackedEntity::Kind::Pointee) {
              std::pair<ReseatTarget, unsigned> RT{ReseatTarget::Fresh, 0};
              if (!V->hasAttr<BlocksAttr>())
                RT = ReseatTargetFor(Init, V->getType());
              AppendReseat(It->second, Init, RT.first, RT.second);
            }
          }
          // A tracked copy: each dest member's state becomes its source
          // member's -- the source entity whose path is the member's own with
          // the copy's derived-to-base path prepended to its first step, so a
          // sliced copy maps onto the right base subobject. A source entity
          // that does not exist cannot occur (the dest's members are a subset
          // of the source's); fall back to Write (assume assigned) if it
          // somehow does.
          auto CopyIt = Storage.CopySource.find(V);
          if (CopyIt == Storage.CopySource.end())
            continue;
          auto Range = Storage.LocalRange.find(V)->second;
          for (unsigned Idx = Range.first; Idx != Range.second; ++Idx) {
            MemberPath SrcSteps(Storage.Entities[Idx].Steps);
            BasePath &First = SrcSteps.front().Bases;
            First.insert(First.begin(), CopyIt->second.Path.begin(),
                         CopyIt->second.Path.end());
            std::optional<unsigned> SrcIdx =
                Storage.find(CopyIt->second.Src, SrcSteps);
            // A member of class type tracked whole is copy-constructed: its
            // source is a value copy's, and the copy is a live object.
            if (SrcIdx && Storage.Entities[Idx].Whole &&
                Ctx.getBaseElementType(Storage.Entities[Idx].Type)
                    ->isRecordType()) {
              BasePath Unused;
              BindingSite Site{K::Variable,
                               getLocalCopySourceRef(V, Unused)->getExprLoc(),
                               /*TargetMarked=*/false,
                               /*IsReference=*/true,
                               nullptr,
                               {},
                               /*EveryPath=*/true};
              BindingLeaf L;
              L.Entity = *SrcIdx;
              Site.Leaves.push_back(L);
              Sites.push_back(std::move(Site));
              BlockEvents.push_back({DefAssignEventKind::Binding, 0,
                                     V->getInit(),
                                     (unsigned)(Sites.size() - 1)});
              BlockEvents.push_back(
                  {DefAssignEventKind::Write, Idx, V->getInit()});
            } else if (SrcIdx)
              BlockEvents.push_back(
                  {DefAssignEventKind::Copy, Idx, V->getInit(), *SrcIdx});
            else
              BlockEvents.push_back(
                  {DefAssignEventKind::Write, Idx, V->getInit()});
          }
        }
      } else if (const auto *RS = dyn_cast<ReturnStmt>(St)) {
        // The returned pointer or reference against the function's return
        // marking (a block's inferred return type carries none).
        const Expr *RV = RS->getRetValue();
        if (!RV)
          continue;
        const ValueDecl *Target = nullptr;
        QualType T;
        if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
          Target = FD;
          T = FD->getReturnType();
        } else if (const auto *BD = dyn_cast<BlockDecl>(D)) {
          const TypeSourceInfo *TSI = BD->getSignatureAsWritten();
          const FunctionType *FT =
              TSI ? TSI->getType()->getAs<FunctionType>() : nullptr;
          T = FT ? FT->getReturnType() : RV->getType();
        } else if (const auto *OMD = dyn_cast<ObjCMethodDecl>(D)) {
          T = OMD->getReturnType();
        }
        AddBinding(K::Return, RV->IgnoreImplicit()->getExprLoc(),
                   Target && Target->hasAttr<RefToUninitAttr>(), T, RV, nullptr,
                   RV, /*EveryPath=*/false, /*AsRange=*/false, Target);
        AppendAliasEscapes(RV, T, RV);
      } else if (const auto *TE = dyn_cast<CXXThrowExpr>(St)) {
        // The thrown pointer copy-initializes the exception object, which
        // cannot carry the marker.
        const Expr *Sub = TE->getSubExpr();
        if (!Sub)
          continue;
        AddBinding(K::Throw, Sub->IgnoreImplicit()->getExprLoc(),
                   /*TargetMarked=*/false, Sub->getType(), Sub, nullptr, TE);
        AppendAliasEscapes(Sub, Sub->getType(), TE);
      } else if (const auto *NE = dyn_cast<CXXNewExpr>(St)) {
        // The written initializer of a scalar allocation binds the allocated
        // pointer; an array's elements are the aggregate arm's.
        if (NE->isArray() || !NE->hasInitializer())
          continue;
        const Expr *Init = NE->getInitializer();
        if (const auto *ILE = dyn_cast<InitListExpr>(Init)) {
          if (ILE->getNumInits() != 1)
            continue;
          Init = ILE->getInit(0);
        }
        AddBinding(K::NewInitializer, Init->IgnoreImplicit()->getExprLoc(),
                   /*TargetMarked=*/false, NE->getAllocatedType(), Init,
                   nullptr, NE);
        AppendAliasEscapes(Init, NE->getAllocatedType(), NE);
      } else if (const auto *DE = dyn_cast<CXXDeleteExpr>(St)) {
        // A delete-expression runs the destructor of its operand's referent
        // when that is non-trivial, then releases the operand's storage.
        if (const auto *RD = Ctx.getBaseElementType(DE->getDestroyedType())
                                 ->getAsCXXRecordDecl();
            RD && RD->hasDefinition() && !RD->hasTrivialDestructor())
          appendLifecycleCallEvents(
              S, D, DE, SemaProfiles::CalleeLifecycleRoles(), Storage,
              BlockEvents, DSites, JudgedLeaves, DE->getArgument());
        SmallVector<TrackedStorage::Resolution, 2> Operands;
        SemaProfiles::forEachTargetLeaf(
            DE->getArgument(), [&](const SemaProfiles::ShapeLeaf &Leaf) {
              Operands.push_back(
                  Storage.resolveFlowLeaf(Leaf, /*AsPointerValue=*/true));
            });
        // A release kills the one whole entity every leaf names.
        if (llvm::all_of(Operands,
                         [&](const TrackedStorage::Resolution &R) {
                           return R.Entity && !R.Subobject &&
                                  R.Entity == Operands.front().Entity;
                         }) &&
            !Operands.empty())
          BlockEvents.push_back(
              {DefAssignEventKind::Kill, *Operands.front().Entity, DE});
      } else if (const auto *ILE = dyn_cast<InitListExpr>(St)) {
        if (!ILE->isTransparent())
          AppendAggregateBindings(ILE->getType(), ILE->inits(),
                                  ILE->getInitializedFieldInUnion());
      } else if (const auto *PLIE = dyn_cast<CXXParenListInitExpr>(St)) {
        AppendAggregateBindings(PLIE->getType(), PLIE->getInitExprs(),
                                PLIE->getInitializedFieldInUnion());
      }
    }
  }
}

// If V is a local whose [[uninit]] members this pass may soundly flow-track
// from an all-unassigned start, return its class definition; null otherwise.
// Sound means nothing can have assigned the members before V's declaration:
// the class shape qualifies (above) and the declaration ran nothing but the
// implicit no-op default-construction.
static const CXXRecordDecl *getTrackedLocalAggregate(const VarDecl *V) {
  const CXXRecordDecl *RD = SemaProfiles::getTrackableLocalClass(V);
  if (!RD)
    return nullptr;
  // Declared without a real initializer: for a record local that is the
  // synthesized call to the implicit default constructor (`Agg a;`), the
  // shared plain-default-init shape. A value-initializing form -- `Agg a{}`
  // / `= {}` (an InitListExpr), `Agg a = Agg()` (a CXXTemporaryObjectExpr),
  // any zero-initializing construction -- gives every member a value and
  // leaves nothing to track. A *copy* does not: it copies indeterminate
  // bits, and a copy does not inherit initialization (paper §5.2). A copy
  // from a *tracked* local inherits the source's per-member state through
  // the copy harvest in harvestTrackedLocals; for an arbitrary
  // untracked source that state is unknowable, so those copies stay
  // untracked -- a missed diagnostic, never a false positive.
  if (!SemaProfiles::isDefaultInitShape(V->getInit()))
    return nullptr;
  return RD;
}

// If V is copy- or move-constructed from a directly named variable, return
// that source's DeclRefExpr and, in \p CastPath, the derived-to-base path a
// slicing copy converts it through; null otherwise. The peel
// (SemaProfiles::peelBaseCasts) resolves the move forms
// (`static_cast<Agg &&>(a)`, `std::move(a)`) to the named operand.
static const DeclRefExpr *getLocalCopySourceRef(const VarDecl *V,
                                                BasePath &CastPath) {
  const Expr *Init = V->getInit();
  if (!Init)
    return nullptr;
  const auto *CCE = dyn_cast<CXXConstructExpr>(Init->IgnoreImplicit());
  if (!CCE || CCE->getNumArgs() < 1 ||
      !CCE->getConstructor()->isCopyOrMoveConstructor())
    return nullptr;
  return dyn_cast<DeclRefExpr>(
      SemaProfiles::peelBaseCasts(CCE->getArg(0), CastPath));
}

/// True if \p E is the current object or its address through transparent
/// casts (\c this, \c *this, \c &*this).
static bool isCurrentObjectOrAddress(const Expr *E) {
  E = SemaProfiles::ignoreTransparentCasts(E);
  if (const Expr *Op = SemaProfiles::addressOfOperand(E))
    E = Op;
  return isCurrentObjectBase(E);
}

/// True if \p St destroys the current object as a whole -- an explicit
/// destructor call on it, delete this -- or binds it whole to a marked,
/// initializing, destroying, or releasing parameter of a call: the uses for
/// which the current object's lifetime is tracked
/// (SemaProfiles::FlowLeafUse).
static bool isCurrentObjectLifecycleUse(Sema &S, const Stmt *St) {
  auto IsCurrentObject = isCurrentObjectOrAddress;
  if (const auto *DE = dyn_cast<CXXDeleteExpr>(St))
    return IsCurrentObject(DE->getArgument());
  const auto *CE = dyn_cast<CallExpr>(St);
  if (!CE)
    return false;
  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE);
      MCE && isa_and_nonnull<CXXDestructorDecl>(MCE->getMethodDecl()))
    return IsCurrentObject(MCE->getImplicitObjectArgument());
  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee)
    return false;
  SemaProfiles::CalleeLifecycleRoles Roles =
      SemaProfiles::getCalleeLifecycleRoles(Callee);
  bool Found = false;
  forEachArgumentBinding(
      S.Context, CE,
      [&](const ParmVarDecl *P, QualType, const Expr *Arg, bool Variadic) {
        Found |= P && !Variadic && !SemaProfiles::isHandleType(P->getType()) &&
                 (Roles.isMarkedParam(P) || Roles.initializesParam(P) ||
                  Roles.destroysParam(P) ||
                  Roles.releasesParam(P, /*TrustedOnly=*/false)) &&
                 IsCurrentObject(Arg);
      });
  return Found;
}

/// Harvest the tracked storage of the analyzed body \p D into \p Storage.
/// Read-tracked (TrackedEntity::TrackReads) are the members of by-value
/// parameters of a trackable class without a default argument, of
/// default-initialized constructor-less aggregate locals (the DeclStmts of
/// forEachAnalyzedStmt, in source order), and of locals copy- or
/// move-constructed from a read-tracked local (recorded in
/// TrackedStorage::CopySource; iterated to a fixpoint so a copy of a copy
/// resolves). Tracked for bindings only are the members of every other local,
/// handler variable, or parameter whose class has marked scalar members
/// (SemaProfiles::flowTrackedAggregateClass), every [[uninit]] local or
/// parameter as a whole, and the referent of every [[ref_to_uninit]] local
/// pointer, reference, or handle; a variable of an enclosing function the body
/// reaches by capture is tracked from an unknown entry state. The read-tracking
/// premises -- a copy does not inherit initialization, a by-value parameter is
/// a copy of the caller's argument, a defaulted parameter is not -- are argued
/// in ProfilesFrameworkInternals.rst, "Flow-Tracked Storage (std::init)" and
/// "Known Gaps (std::init)".
static void harvestTrackedLocals(Sema &S, const Decl *D,
                                 TrackedStorage &Storage) {
  // A marked local or parameter is tracked as a whole ([[uninit]]) or by
  // its referent ([[ref_to_uninit]] pointer, reference, or handle).
  auto HarvestMarked = [&](const VarDecl *V, bool UnknownEntry) {
    if (V->isInvalidDecl() || !V->hasLocalStorage())
      return;
    if (V->hasAttr<UninitAttr>())
      Storage.addLocalEntity(TrackedEntity::Kind::WholeLocal, V, UnknownEntry);
    else if (V->hasAttr<RefToUninitAttr>() &&
             (V->getType()->isPointerType() ||
              V->getType()->isReferenceType() ||
              SemaProfiles::isHandleType(V->getType())))
      Storage.addLocalEntity(TrackedEntity::Kind::Pointee, V, UnknownEntry);
  };
  llvm::SmallPtrSet<const VarDecl *, 16> Owned;
  // A by-value parameter's members are read-tracked from the all-unassigned
  // entry state unless the parameter declares a default argument (a
  // defaulted call initializes the parameter object directly, and the mere
  // presence gates so that getDefaultArg() is never called on an unparsed or
  // uninstantiated default).
  ArrayRef<ParmVarDecl *> Params;
  if (const auto *FD = dyn_cast<FunctionDecl>(D))
    Params = FD->parameters();
  else if (const auto *BD = dyn_cast<BlockDecl>(D))
    Params = BD->parameters();
  else if (const auto *OMD = dyn_cast<ObjCMethodDecl>(D))
    Params = OMD->parameters();
  for (const ParmVarDecl *P : Params) {
    Owned.insert(P);
    HarvestMarked(P, /*UnknownEntry=*/false);
    if (const CXXRecordDecl *RD = SemaProfiles::flowTrackedAggregateClass(P))
      Storage.addObject(P, RD,
                        /*TrackReads=*/!P->hasDefaultArg() &&
                            SemaProfiles::getTrackableSlotClass(P->getType()),
                        /*UnknownEntry=*/false);
  }
  // The current object's lifetime, outside its constructors, when the body
  // has a lifecycle use of it.
  const CXXMethodDecl *Method =
      SemaProfiles::enclosingInstanceMethod(dyn_cast<DeclContext>(D));
  if (isa_and_nonnull<CXXConstructorDecl>(Method))
    Method = nullptr;
  bool CurrentObjectUsed = false;
  // The body's own variables (DeclStmts and handler declarations, in source
  // order) and the local-storage variables of enclosing functions it reaches
  // by capture.
  SmallVector<const VarDecl *, 8> Locals;
  SmallVector<const DeclStmt *, 8> DeclStmts;
  SmallVector<const VarDecl *, 4> Referenced;
  forEachAnalyzedStmt(D, [&](const Stmt *Cur) {
    if (Method && !CurrentObjectUsed)
      CurrentObjectUsed = isCurrentObjectLifecycleUse(S, Cur);
    if (const auto *DS = dyn_cast<DeclStmt>(Cur)) {
      DeclStmts.push_back(DS);
      for (const Decl *Dcl : DS->decls())
        if (const auto *V = dyn_cast<VarDecl>(Dcl); V && Owned.insert(V).second)
          Locals.push_back(V);
    } else if (const auto *CS = dyn_cast<CXXCatchStmt>(Cur)) {
      if (const VarDecl *V = CS->getExceptionDecl();
          V && Owned.insert(V).second)
        Locals.push_back(V);
    } else if (const auto *DRE = dyn_cast<DeclRefExpr>(Cur)) {
      if (const auto *V = dyn_cast<VarDecl>(DRE->getDecl());
          V && V->hasLocalStorage())
        Referenced.push_back(V);
    }
  });
  if (CurrentObjectUsed)
    Storage.addCurrentObjectRoot(Method->getParent());
  for (const VarDecl *V : Locals) {
    HarvestMarked(V, /*UnknownEntry=*/false);
    if (const CXXRecordDecl *RD = getTrackedLocalAggregate(V))
      Storage.addObject(V, RD, /*TrackReads=*/true, /*UnknownEntry=*/false);
  }
  // The copy harvest chains from a read-tracked local only, so it runs
  // before the binding-only objects are added.
  for (bool Added = true; Added;) {
    Added = false;
    for (const DeclStmt *DS : DeclStmts)
      for (const Decl *Dcl : DS->decls()) {
        const auto *V = dyn_cast<VarDecl>(Dcl);
        if (!V || Storage.LocalRange.count(V))
          continue;
        const CXXRecordDecl *RD = SemaProfiles::getTrackableLocalClass(V);
        if (!RD)
          continue;
        BasePath CastPath;
        const DeclRefExpr *SrcRef = getLocalCopySourceRef(V, CastPath);
        const auto *Src =
            SrcRef ? dyn_cast<VarDecl>(SrcRef->getDecl()) : nullptr;
        auto SrcIt =
            Src ? Storage.LocalRange.find(Src) : Storage.LocalRange.end();
        if (SrcIt == Storage.LocalRange.end() ||
            !Storage.Entities[SrcIt->second.first].TrackReads)
          continue;
        std::pair<unsigned, unsigned> Range =
            Storage.addObject(V, RD, /*TrackReads=*/true,
                              /*UnknownEntry=*/false);
        if (Range.first == Range.second)
          continue;
        Storage.CopySource[V] = {Src, std::move(CastPath)};
        Added = true;
      }
  }
  // Every other local with marked scalar members is tracked for bindings.
  for (const VarDecl *V : Locals)
    if (!Storage.LocalRange.count(V))
      if (const CXXRecordDecl *RD = SemaProfiles::flowTrackedAggregateClass(V))
        Storage.addObject(V, RD, /*TrackReads=*/false,
                          /*UnknownEntry=*/false);
  // Variables of enclosing functions, from an unknown entry state.
  llvm::SmallPtrSet<const VarDecl *, 4> Seen;
  for (const VarDecl *V : Referenced) {
    if (Owned.count(V) || !Seen.insert(V).second)
      continue;
    HarvestMarked(V, /*UnknownEntry=*/true);
    if (!Storage.LocalRange.count(V) && !Storage.LocalEntity.count(V))
      if (const CXXRecordDecl *RD = SemaProfiles::flowTrackedAggregateClass(V))
        Storage.addObject(V, RD, /*TrackReads=*/false,
                          /*UnknownEntry=*/true);
  }
}

/// Add to \p Out every expression below a construct of \p D's body that the CFG
/// contains and the parse-time funnels check but the std::init flow engine does
/// not enumerate: an argument of a builtin with custom type checking (other
/// than __builtin_operator_new and __builtin_operator_delete, which
/// forEachArgumentBinding zips), an atomic builtin's operand, an Objective-C
/// boxed expression's operand, and a captured statement's capture initializer,
/// through which the region's statements use an enclosing variable in place. A
/// deferred check below one is a known gap (ProfilesFrameworkInternals.rst,
/// "Known Gaps (std::init)").
static void collectUnenumeratedExprs(const ASTContext &Ctx, const Decl *D,
                                     llvm::SmallPtrSetImpl<const Expr *> &Out) {
  llvm::SmallPtrSet<const Stmt *, 16> Visited;
  auto AddSubtree = [&](const Stmt *Root) {
    SmallVector<const Stmt *, 16> Stack(1, Root);
    while (!Stack.empty()) {
      const Stmt *Cur = Stack.pop_back_val();
      if (!Cur || !Visited.insert(Cur).second)
        continue;
      if (const auto *E = dyn_cast<Expr>(Cur))
        Out.insert(E);
      appendAnalyzedChildren(Cur, Stack);
    }
  };
  forEachAnalyzedStmt(D, [&](const Stmt *Cur) {
    if (const auto *CE = dyn_cast<CallExpr>(Cur)) {
      unsigned ID = CE->getBuiltinCallee();
      if (ID && Ctx.BuiltinInfo.hasCustomTypechecking(ID) &&
          ID != Builtin::BI__builtin_operator_new &&
          ID != Builtin::BI__builtin_operator_delete)
        for (const Expr *Arg : CE->arguments())
          AddSubtree(Arg);
    } else if (isa<AtomicExpr, ObjCBoxedExpr>(Cur)) {
      for (const Stmt *Child : Cur->children())
        AddSubtree(Child);
    } else if (const auto *CS = dyn_cast<CapturedStmt>(Cur)) {
      for (const Expr *Init : CS->capture_inits())
        AddSubtree(Init);
    }
  });
}

/// Account for the checks the parse-time funnels deferred to the flow pass
/// over \p D (SemaProfiles::DeferredInitSite): a site none of whose tracked
/// leaves is in \p Judged is a leftover. A leftover none of whose leaves is
/// an element of \p cfg -- null when the CFG could not be built -- lies in
/// code the CFG does not contain (an unevaluated builtin argument, a
/// coroutine's parameter copies, an unselected _Generic association) and is
/// dropped, as is one below a construct the engine does not enumerate
/// (collectUnenumeratedExprs). Any other leftover is an enumeration bug: an
/// error under -fprofiles-verify-deferred-checks, and judged by form in every
/// build.
static void verifyDeferredInitSites(
    Sema &S, const Decl *D, ArrayRef<SemaProfiles::DeferredInitSite> Deferred,
    const CFG *cfg, const llvm::SmallPtrSetImpl<const Expr *> &Judged) {
  if (!cfg)
    return;
  SmallVector<const SemaProfiles::DeferredInitSite *, 4> Leftover;
  for (const SemaProfiles::DeferredInitSite &Site : Deferred)
    if (llvm::none_of(Site.TrackedLeaves,
                      [&](const Expr *Leaf) { return Judged.contains(Leaf); }))
      Leftover.push_back(&Site);
  if (Leftover.empty())
    return;
  llvm::SmallPtrSet<const Stmt *, 32> Elements;
  for (const CFGBlock *B : *cfg)
    for (const CFGElement &Elem : *B)
      if (auto CS = Elem.getAs<CFGStmt>())
        Elements.insert(CS->getStmt());
  llvm::SmallPtrSet<const Expr *, 32> Unenumerated;
  collectUnenumeratedExprs(S.Context, D, Unenumerated);
  llvm::erase_if(Leftover, [&](const SemaProfiles::DeferredInitSite *Site) {
    return llvm::none_of(
               Site->TrackedLeaves,
               [&](const Expr *Leaf) { return Elements.contains(Leaf); }) ||
           llvm::any_of(Site->TrackedLeaves, [&](const Expr *Leaf) {
             return Unenumerated.contains(Leaf);
           });
  });
  for (const SemaProfiles::DeferredInitSite *Site : Leftover) {
    // The diagnostic's %select follows DeferredInitSite::Kind.
    if (S.getLangOpts().ProfilesVerifyDeferredChecks)
      S.Diag(Site->Loc, diag::err_init_deferred_check_unjudged)
          << static_cast<unsigned>(Site->K);
    S.Profiles().judgeDeferredInitSiteByForm(*Site, D);
  }
}

/// The std::init flow engine over one body: the member read-before-init rule
/// (P4222R2 §4.2, §5.1-§5.4) and the binding, destroy, read-through, and
/// subobject-write judgments of flow-tracked storage
/// (ProfilesFrameworkInternals.rst, "Flow-Tracked Storage") -- one entity
/// table, one event extraction, one definite-assignment run, one report,
/// over a CFG of the engine's own. The current object's members are tracked
/// in every instance member function body (a lambda's or block's body inside
/// one included), from the unassigned entry state; their reads are tracked
/// in a constructor body only. A delegating constructor's target initializes
/// the members first and a union's members are mutually exclusive (§5.6), so
/// neither constructor's reads are tracked, as ctor_uninit_member exempts
/// both. The crediting policy -- whole-entity stores and lifecycle calls,
/// plus any escape for a local aggregate's reads -- is stated in
/// ProfilesFramework.rst, "Reads of Uninitialized Objects", and
/// ProfilesFrameworkInternals.rst, "Known Gaps (std::init)".
static void runStdInitMemberReadChecks(Sema &S, const Decl *D,
                                       AnalysisDeclContext &,
                                       const CFGProfileEntry &Entry) {
  TrackedStorage Storage(S);
  const auto *Ctor = dyn_cast<CXXConstructorDecl>(D);
  if (Ctor && (Ctor->isDelegatingConstructor() || Ctor->getParent()->isUnion()))
    Ctor = nullptr;
  bool StarThisCopyTrusted = false;
  if (const CXXMethodDecl *MD =
          SemaProfiles::enclosingInstanceMethod(dyn_cast<DeclContext>(D))) {
    if (const CXXRecordDecl *RD = MD->getParent()->getDefinition()) {
      Storage.addObject(/*Base=*/nullptr, RD, /*TrackReads=*/Ctor != nullptr,
                        /*UnknownEntry=*/false);
      Storage.addMemberPointerRoots(RD);
    }
    // A [*this] capture copy-constructs the whole object; its whole-object
    // reads are appended only without a user-provided copy constructor (see
    // ProfilesFrameworkInternals.rst, "Known Gaps (std::init)").
    if (Ctor)
      StarThisCopyTrusted = llvm::any_of(
          Ctor->getParent()->ctors(), [](const CXXConstructorDecl *C) {
            return C->isCopyConstructor() && C->isUserProvided();
          });
  }
  harvestTrackedLocals(S, D, Storage);
  SmallVector<SemaProfiles::DeferredInitSite, 4> Deferred =
      S.Profiles().takeDeferredInitSites(D);
  // A marked binding of the current object, deferred by the parse-time
  // funnel, is a lifecycle use of it too.
  if (const CXXMethodDecl *Method =
          SemaProfiles::enclosingInstanceMethod(dyn_cast<DeclContext>(D));
      Method && !isa<CXXConstructorDecl>(Method) &&
      !Storage.CurrentObjectRoot &&
      llvm::any_of(Deferred, [](const SemaProfiles::DeferredInitSite &Site) {
        return llvm::any_of(Site.TrackedLeaves, isCurrentObjectOrAddress);
      }))
    Storage.addCurrentObjectRoot(Method->getParent());
  if (Storage.Entities.empty() && Deferred.empty())
    return;
  // The engine's own CFG, built only for a body with tracked storage or
  // deferred checks: exception edges, so a call inside a try block reaches its
  // handler before the assignment that follows it (P4222R2 §1.3: every path
  // is considered), every expression as an element, so the extraction arms
  // match any statement class, and a new-expression's allocation call as an
  // element of its own. The shared CFG the other analyses see is untouched
  // (see ProfilesFrameworkInternals.rst, "CFG Riders").
  AnalysisDeclContext InitAC(/*Mgr=*/nullptr, D);
  CFG::BuildOptions &Options = InitAC.getCFGBuildOptions();
  Options.PruneTriviallyFalseEdges = true;
  Options.AddEHEdges = true;
  Options.AddInitializers = true;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  Options.AddCXXDefaultInitExprInCtors = true;
  Options.AddCXXNewAllocator = true;
  Options.setAllAlwaysAdd();
  CFG *cfg = InitAC.getCFG();
  // A flow-tracked leaf names a harvested entity, so with none every deferred
  // check is a leftover.
  llvm::SmallPtrSet<const Expr *, 16> JudgedLeaves;
  if (!cfg || Storage.Entities.empty()) {
    verifyDeferredInitSites(S, D, Deferred, cfg, JudgedLeaves);
    return;
  }
  std::vector<SmallVector<DefAssignEvent, 4>> Events(cfg->getNumBlockIDs());
  SmallVector<BindingSite, 8> Sites;
  SmallVector<DestroySite, 4> DSites;
  SmallVector<AccessSite, 8> ASites;
  extractStdInitEvents(S, D, *cfg, Storage, Ctor, StarThisCopyTrusted, Events,
                       Sites, DSites, ASites, JudgedLeaves);
  verifyDeferredInitSites(S, D, Deferred, cfg, JudgedLeaves);
  for (const SmallVector<DefAssignEvent, 4> &BlockEvents : Events) {
    NumFlowEvents += BlockEvents.size();
    for (const DefAssignEvent &Ev : BlockEvents)
      if (Ev.Kind == DefAssignEventKind::Reseat &&
          Ev.RT == ReseatTarget::ViaPointer)
        Storage.AliasClasses.unionSets(Ev.Idx, Ev.Aux);
  }
  // Nothing is assigned at function entry: written initializers write at
  // their CFGInitializer elements, a tracked local cannot be referenced
  // before its DeclStmt, a by-value parameter starts unassigned by design,
  // and a marked local's marker asserts it.
  SmallVector<PendingViolation, 8> Violations;
  std::vector<SmallVector<const Expr *, 2>> Offending = runDefiniteAssignment(
      *cfg, InitAC, Storage, Events, Sites, DSites, ASites, Violations);
  reportMemberReadsBeforeInit(S, Offending, Storage, Entry.Name);
  reportFlowViolations(S, Violations, Entry.Name);
}

class UninitValsDiagReporter : public UninitVariablesHandler {
  Sema &S;
  AnalysisDeclContext &AC;
  /// When set, only the CFGProfiles diagnostics are emitted; the default
  /// -Wuninitialized reports are skipped. The post-error profile pass sets
  /// this so it cannot resurrect ordinary warnings that the first TU error
  /// is meant to suppress.
  bool ProfileOnly;
  typedef SmallVector<UninitUse, 2> UsesVec;
  typedef llvm::PointerIntPair<UsesVec *, 1, bool> MappedType;
  // Prefer using MapVector to DenseMap, so that iteration order will be
  // the same as insertion order. This is needed to obtain a deterministic
  // order of diagnostics when calling flushDiagnostics().
  typedef llvm::MapVector<const VarDecl *, MappedType> UsesMap;
  UsesMap uses;

public:
  UninitValsDiagReporter(Sema &S, AnalysisDeclContext &AC,
                         bool ProfileOnly = false)
      : S(S), AC(AC), ProfileOnly(ProfileOnly) {}
  ~UninitValsDiagReporter() override { flushDiagnostics(); }

  MappedType &getUses(const VarDecl *vd) {
    MappedType &V = uses[vd];
    if (!V.getPointer())
      V.setPointer(new UsesVec());
    return V;
  }

  void handleUseOfUninitVariable(const VarDecl *vd,
                                 const UninitUse &use) override {
    getUses(vd).getPointer()->push_back(use);
  }

  void handleSelfInit(const VarDecl *vd) override { getUses(vd).setInt(true); }

  void flushDiagnostics() {
    for (const auto &P : uses) {
      const VarDecl *vd = P.first;
      const MappedType &V = P.second;

      UsesVec *vec = V.getPointer();
      bool hasSelfInit = V.getInt();

      diagnoseUnitializedVar(vd, hasSelfInit, vec);

      // Release the uses vector.
      delete vec;
    }

    uses.clear();
  }

private:
  static bool hasAlwaysUninitializedUse(const UsesVec* vec) {
    return llvm::any_of(*vec, [](const UninitUse &U) {
      return U.getKind() == UninitUse::Always ||
             U.getKind() == UninitUse::AfterCall ||
             U.getKind() == UninitUse::AfterDecl;
    });
  }

  // Print the diagnostic for the variable.  We try to warn only on the first
  // point at which a variable is used uninitialized.  After the first
  // diagnostic is printed, further diagnostics for this variable are skipped.
  void diagnoseUnitializedVar(const VarDecl *vd, bool hasSelfInit,
                              UsesVec *vec) {
    if (tryDiagnoseProfileUninitRead(S, AC, vd, hasSelfInit, *vec))
      return;
    // The post-error pass runs purely to keep CFG-uninit profiles diagnosing;
    // it must never fall through to the default -Wuninitialized reports.
    if (ProfileOnly)
      return;

    // Specially handle the case where we have uses of an uninitialized
    // variable, but the root cause is an idiomatic self-init.  We want
    // to report the diagnostic at the self-init since that is the root cause.
    if (hasSelfInit && hasAlwaysUninitializedUse(vec)) {
      if (DiagnoseUninitializedUse(S, vd,
                                   UninitUse(vd->getInit()->IgnoreParenCasts(),
                                             /*isAlwaysUninit=*/true),
                                   /*alwaysReportSelfInit=*/true))
        return;
    }

    // Sort the uses by their SourceLocations.  While not strictly
    // guaranteed to produce them in line/column order, this will provide
    // a stable ordering.
    llvm::sort(*vec, [](const UninitUse &a, const UninitUse &b) {
      // Prefer the direct use of an uninitialized variable over its use via
      // constant reference or pointer.
      if (a.isConstRefOrPtrUse() != b.isConstRefOrPtrUse())
        return b.isConstRefOrPtrUse();
      // Prefer a more confident report over a less confident one.
      if (a.getKind() != b.getKind())
        return a.getKind() > b.getKind();
      return a.getUser()->getBeginLoc() < b.getUser()->getBeginLoc();
    });

    for (const auto &U : *vec) {
      if (U.isConstRefUse()) {
        if (DiagnoseUninitializedConstRefUse(S, vd, U))
          return;
      } else if (U.isConstPtrUse()) {
        if (DiagnoseUninitializedConstPtrUse(S, vd, U))
          return;
      } else {
        // If we have self-init, downgrade all uses to 'may be uninitialized'.
        UninitUse Use = hasSelfInit ? UninitUse(U.getUser(), false) : U;
        if (DiagnoseUninitializedUse(S, vd, Use))
          return;
      }
    }
  }
};

/// Inter-procedural data for the called-once checker.
class CalledOnceInterProceduralData {
public:
  // Add the delayed warning for the given block.
  void addDelayedWarning(const BlockDecl *Block,
                         PartialDiagnosticAt &&Warning) {
    DelayedBlockWarnings[Block].emplace_back(std::move(Warning));
  }
  // Report all of the warnings we've gathered for the given block.
  void flushWarnings(const BlockDecl *Block, Sema &S) {
    for (const PartialDiagnosticAt &Delayed : DelayedBlockWarnings[Block])
      S.Diag(Delayed.first, Delayed.second);

    discardWarnings(Block);
  }
  // Discard all of the warnings we've gathered for the given block.
  void discardWarnings(const BlockDecl *Block) {
    DelayedBlockWarnings.erase(Block);
  }

private:
  using DelayedDiagnostics = SmallVector<PartialDiagnosticAt, 2>;
  llvm::DenseMap<const BlockDecl *, DelayedDiagnostics> DelayedBlockWarnings;
};

class CalledOnceCheckReporter : public CalledOnceCheckHandler {
public:
  CalledOnceCheckReporter(Sema &S, CalledOnceInterProceduralData &Data)
      : S(S), Data(Data) {}
  void handleDoubleCall(const ParmVarDecl *Parameter, const Expr *Call,
                        const Expr *PrevCall, bool IsCompletionHandler,
                        bool Poised) override {
    auto DiagToReport = IsCompletionHandler
                            ? diag::warn_completion_handler_called_twice
                            : diag::warn_called_once_gets_called_twice;
    S.Diag(Call->getBeginLoc(), DiagToReport) << Parameter;
    S.Diag(PrevCall->getBeginLoc(), diag::note_called_once_gets_called_twice)
        << Poised;
  }

  void handleNeverCalled(const ParmVarDecl *Parameter,
                         bool IsCompletionHandler) override {
    auto DiagToReport = IsCompletionHandler
                            ? diag::warn_completion_handler_never_called
                            : diag::warn_called_once_never_called;
    S.Diag(Parameter->getBeginLoc(), DiagToReport)
        << Parameter << /* Captured */ false;
  }

  void handleNeverCalled(const ParmVarDecl *Parameter, const Decl *Function,
                         const Stmt *Where, NeverCalledReason Reason,
                         bool IsCalledDirectly,
                         bool IsCompletionHandler) override {
    auto DiagToReport = IsCompletionHandler
                            ? diag::warn_completion_handler_never_called_when
                            : diag::warn_called_once_never_called_when;
    PartialDiagnosticAt Warning(Where->getBeginLoc(), S.PDiag(DiagToReport)
                                                          << Parameter
                                                          << IsCalledDirectly
                                                          << (unsigned)Reason);

    if (const auto *Block = dyn_cast<BlockDecl>(Function)) {
      // We shouldn't report these warnings on blocks immediately
      Data.addDelayedWarning(Block, std::move(Warning));
    } else {
      S.Diag(Warning.first, Warning.second);
    }
  }

  void handleCapturedNeverCalled(const ParmVarDecl *Parameter,
                                 const Decl *Where,
                                 bool IsCompletionHandler) override {
    auto DiagToReport = IsCompletionHandler
                            ? diag::warn_completion_handler_never_called
                            : diag::warn_called_once_never_called;
    S.Diag(Where->getBeginLoc(), DiagToReport)
        << Parameter << /* Captured */ true;
  }

  void
  handleBlockThatIsGuaranteedToBeCalledOnce(const BlockDecl *Block) override {
    Data.flushWarnings(Block, S);
  }

  void handleBlockWithNoGuarantees(const BlockDecl *Block) override {
    Data.discardWarnings(Block);
  }

private:
  Sema &S;
  CalledOnceInterProceduralData &Data;
};

constexpr unsigned CalledOnceWarnings[] = {
    diag::warn_called_once_never_called,
    diag::warn_called_once_never_called_when,
    diag::warn_called_once_gets_called_twice};

constexpr unsigned CompletionHandlerWarnings[]{
    diag::warn_completion_handler_never_called,
    diag::warn_completion_handler_never_called_when,
    diag::warn_completion_handler_called_twice};

bool shouldAnalyzeCalledOnceImpl(llvm::ArrayRef<unsigned> DiagIDs,
                                 const DiagnosticsEngine &Diags,
                                 SourceLocation At) {
  return llvm::any_of(DiagIDs, [&Diags, At](unsigned DiagID) {
    return !Diags.isIgnored(DiagID, At);
  });
}

bool shouldAnalyzeCalledOnceConventions(const DiagnosticsEngine &Diags,
                                        SourceLocation At) {
  return shouldAnalyzeCalledOnceImpl(CompletionHandlerWarnings, Diags, At);
}

bool shouldAnalyzeCalledOnceParameters(const DiagnosticsEngine &Diags,
                                       SourceLocation At) {
  return shouldAnalyzeCalledOnceImpl(CalledOnceWarnings, Diags, At) ||
         shouldAnalyzeCalledOnceConventions(Diags, At);
}
} // anonymous namespace

//===----------------------------------------------------------------------===//
// -Wthread-safety
//===----------------------------------------------------------------------===//
namespace clang {
namespace threadSafety {
namespace {
class ThreadSafetyReporter : public clang::threadSafety::ThreadSafetyHandler {
  Sema &S;
  DiagList Warnings;
  SourceLocation FunLocation, FunEndLocation;

  const FunctionDecl *CurrentFunction;
  bool Verbose;

  OptionalNotes getNotes() const {
    if (Verbose && CurrentFunction) {
      PartialDiagnosticAt FNote(CurrentFunction->getBody()->getBeginLoc(),
                                S.PDiag(diag::note_thread_warning_in_fun)
                                    << CurrentFunction);
      return OptionalNotes(1, FNote);
    }
    return OptionalNotes();
  }

  OptionalNotes getNotes(const PartialDiagnosticAt &Note) const {
    OptionalNotes ONS(1, Note);
    if (Verbose && CurrentFunction) {
      PartialDiagnosticAt FNote(CurrentFunction->getBody()->getBeginLoc(),
                                S.PDiag(diag::note_thread_warning_in_fun)
                                    << CurrentFunction);
      ONS.push_back(std::move(FNote));
    }
    return ONS;
  }

  OptionalNotes getNotes(const PartialDiagnosticAt &Note1,
                         const PartialDiagnosticAt &Note2) const {
    OptionalNotes ONS;
    ONS.push_back(Note1);
    ONS.push_back(Note2);
    if (Verbose && CurrentFunction) {
      PartialDiagnosticAt FNote(CurrentFunction->getBody()->getBeginLoc(),
                                S.PDiag(diag::note_thread_warning_in_fun)
                                    << CurrentFunction);
      ONS.push_back(std::move(FNote));
    }
    return ONS;
  }

  OptionalNotes makeLockedHereNote(SourceLocation LocLocked, StringRef Kind) {
    return LocLocked.isValid()
               ? getNotes(PartialDiagnosticAt(
                     LocLocked, S.PDiag(diag::note_locked_here) << Kind))
               : getNotes();
  }

  OptionalNotes makeUnlockedHereNote(SourceLocation LocUnlocked,
                                     StringRef Kind) {
    return LocUnlocked.isValid()
               ? getNotes(PartialDiagnosticAt(
                     LocUnlocked, S.PDiag(diag::note_unlocked_here) << Kind))
               : getNotes();
  }

  OptionalNotes makeManagedMismatchNoteForParam(SourceLocation DeclLoc) {
    return DeclLoc.isValid()
               ? getNotes(PartialDiagnosticAt(
                     DeclLoc,
                     S.PDiag(diag::note_managed_mismatch_here_for_param)))
               : getNotes();
  }

 public:
  ThreadSafetyReporter(Sema &S, SourceLocation FL, SourceLocation FEL)
    : S(S), FunLocation(FL), FunEndLocation(FEL),
      CurrentFunction(nullptr), Verbose(false) {}

  void setVerbose(bool b) { Verbose = b; }

  /// Emit all buffered diagnostics in order of sourcelocation.
  /// We need to output diagnostics produced while iterating through
  /// the lockset in deterministic order, so this function orders diagnostics
  /// and outputs them.
  void emitDiagnostics() {
    Warnings.sort(SortDiagBySourceLocation(S.getSourceManager()));
    for (const auto &Diag : Warnings) {
      S.Diag(Diag.first.first, Diag.first.second);
      for (const auto &Note : Diag.second)
        S.Diag(Note.first, Note.second);
    }
  }

  void handleUnmatchedUnderlyingMutexes(SourceLocation Loc, SourceLocation DLoc,
                                        Name scopeName, StringRef Kind,
                                        Name expected, Name actual) override {
    PartialDiagnosticAt Warning(Loc,
                                S.PDiag(diag::warn_unmatched_underlying_mutexes)
                                    << Kind << scopeName << expected << actual);
    Warnings.emplace_back(std::move(Warning),
                          makeManagedMismatchNoteForParam(DLoc));
  }

  void handleExpectMoreUnderlyingMutexes(SourceLocation Loc,
                                         SourceLocation DLoc, Name scopeName,
                                         StringRef Kind,
                                         Name expected) override {
    PartialDiagnosticAt Warning(
        Loc, S.PDiag(diag::warn_expect_more_underlying_mutexes)
                 << Kind << scopeName << expected);
    Warnings.emplace_back(std::move(Warning),
                          makeManagedMismatchNoteForParam(DLoc));
  }

  void handleExpectFewerUnderlyingMutexes(SourceLocation Loc,
                                          SourceLocation DLoc, Name scopeName,
                                          StringRef Kind,
                                          Name actual) override {
    PartialDiagnosticAt Warning(
        Loc, S.PDiag(diag::warn_expect_fewer_underlying_mutexes)
                 << Kind << scopeName << actual);
    Warnings.emplace_back(std::move(Warning),
                          makeManagedMismatchNoteForParam(DLoc));
  }

  void handleInvalidLockExp(SourceLocation Loc) override {
    PartialDiagnosticAt Warning(Loc, S.PDiag(diag::warn_cannot_resolve_lock)
                                         << Loc);
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void handleUnmatchedUnlock(StringRef Kind, Name LockName, SourceLocation Loc,
                             SourceLocation LocPreviousUnlock) override {
    if (Loc.isInvalid())
      Loc = FunLocation;
    PartialDiagnosticAt Warning(Loc, S.PDiag(diag::warn_unlock_but_no_lock)
                                         << Kind << LockName);
    Warnings.emplace_back(std::move(Warning),
                          makeUnlockedHereNote(LocPreviousUnlock, Kind));
  }

  void handleIncorrectUnlockKind(StringRef Kind, Name LockName,
                                 LockKind Expected, LockKind Received,
                                 SourceLocation LocLocked,
                                 SourceLocation LocUnlock) override {
    if (LocUnlock.isInvalid())
      LocUnlock = FunLocation;
    PartialDiagnosticAt Warning(
        LocUnlock, S.PDiag(diag::warn_unlock_kind_mismatch)
                       << Kind << LockName << Received << Expected);
    Warnings.emplace_back(std::move(Warning),
                          makeLockedHereNote(LocLocked, Kind));
  }

  void handleDoubleLock(StringRef Kind, Name LockName, SourceLocation LocLocked,
                        SourceLocation LocDoubleLock) override {
    if (LocDoubleLock.isInvalid())
      LocDoubleLock = FunLocation;
    PartialDiagnosticAt Warning(LocDoubleLock, S.PDiag(diag::warn_double_lock)
                                                   << Kind << LockName);
    Warnings.emplace_back(std::move(Warning),
                          makeLockedHereNote(LocLocked, Kind));
  }

  void handleMutexHeldEndOfScope(StringRef Kind, Name LockName,
                                 SourceLocation LocLocked,
                                 SourceLocation LocEndOfScope,
                                 LockErrorKind LEK,
                                 bool ReentrancyMismatch) override {
    unsigned DiagID = 0;
    switch (LEK) {
      case LEK_LockedSomePredecessors:
        DiagID = diag::warn_lock_some_predecessors;
        break;
      case LEK_LockedSomeLoopIterations:
        DiagID = diag::warn_expecting_lock_held_on_loop;
        break;
      case LEK_LockedAtEndOfFunction:
        DiagID = diag::warn_no_unlock;
        break;
      case LEK_NotLockedAtEndOfFunction:
        DiagID = diag::warn_expecting_locked;
        break;
    }
    if (LocEndOfScope.isInvalid())
      LocEndOfScope = FunEndLocation;

    PartialDiagnosticAt Warning(LocEndOfScope, S.PDiag(DiagID)
                                                   << Kind << LockName
                                                   << ReentrancyMismatch);
    Warnings.emplace_back(std::move(Warning),
                          makeLockedHereNote(LocLocked, Kind));
  }

  void handleExclusiveAndShared(StringRef Kind, Name LockName,
                                SourceLocation Loc1,
                                SourceLocation Loc2) override {
    PartialDiagnosticAt Warning(Loc1,
                                S.PDiag(diag::warn_lock_exclusive_and_shared)
                                    << Kind << LockName);
    PartialDiagnosticAt Note(Loc2, S.PDiag(diag::note_lock_exclusive_and_shared)
                                       << Kind << LockName);
    Warnings.emplace_back(std::move(Warning), getNotes(Note));
  }

  void handleNoMutexHeld(const NamedDecl *D, ProtectedOperationKind POK,
                         AccessKind AK, SourceLocation Loc) override {
    unsigned DiagID = 0;
    switch (POK) {
    case POK_VarAccess:
    case POK_PassByRef:
    case POK_ReturnByRef:
    case POK_PassPointer:
    case POK_ReturnPointer:
      DiagID = diag::warn_variable_requires_any_lock;
      break;
    case POK_VarDereference:
    case POK_PtPassByRef:
    case POK_PtReturnByRef:
    case POK_PtPassPointer:
    case POK_PtReturnPointer:
      DiagID = diag::warn_var_deref_requires_any_lock;
      break;
    case POK_FunctionCall:
      llvm_unreachable("Only works for variables");
      break;
    }
    PartialDiagnosticAt Warning(Loc, S.PDiag(DiagID)
      << D << getLockKindFromAccessKind(AK));
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void handleGuardedByAnyReadNotHeld(const NamedDecl *D,
                                     ProtectedOperationKind POK,
                                     ArrayRef<StringRef> LockNames,
                                     SourceLocation Loc) override {
    bool IsDeref;
    switch (POK) {
    case POK_VarAccess:
    case POK_PassByRef:
    case POK_ReturnByRef:
    case POK_PassPointer:
    case POK_ReturnPointer:
      IsDeref = false;
      break;
    case POK_VarDereference:
    case POK_PtPassByRef:
    case POK_PtReturnByRef:
    case POK_PtPassPointer:
    case POK_PtReturnPointer:
      IsDeref = true;
      break;
    case POK_FunctionCall:
      llvm_unreachable("POK_FunctionCall not applicable here");
    }
    std::string Quoted;
    llvm::raw_string_ostream OS(Quoted);
    llvm::ListSeparator LS;
    for (StringRef Name : LockNames)
      OS << LS << "'" << Name << "'";
    PartialDiagnosticAt Warning(Loc, S.PDiag(diag::warn_requires_any_of_locks)
                                         << D << IsDeref << Quoted);
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void handleMutexNotHeld(StringRef Kind, const NamedDecl *D,
                          ProtectedOperationKind POK, Name LockName,
                          LockKind LK, SourceLocation Loc,
                          Name *PossibleMatch) override {
    unsigned DiagID = 0;
    if (PossibleMatch) {
      switch (POK) {
        case POK_VarAccess:
          DiagID = diag::warn_variable_requires_lock_precise;
          break;
        case POK_VarDereference:
          DiagID = diag::warn_var_deref_requires_lock_precise;
          break;
        case POK_FunctionCall:
          DiagID = diag::warn_fun_requires_lock_precise;
          break;
        case POK_PassByRef:
          DiagID = diag::warn_guarded_pass_by_reference;
          break;
        case POK_PtPassByRef:
          DiagID = diag::warn_pt_guarded_pass_by_reference;
          break;
        case POK_ReturnByRef:
          DiagID = diag::warn_guarded_return_by_reference;
          break;
        case POK_PtReturnByRef:
          DiagID = diag::warn_pt_guarded_return_by_reference;
          break;
        case POK_PassPointer:
          DiagID = diag::warn_guarded_pass_pointer;
          break;
        case POK_PtPassPointer:
          DiagID = diag::warn_pt_guarded_pass_pointer;
          break;
        case POK_ReturnPointer:
          DiagID = diag::warn_guarded_return_pointer;
          break;
        case POK_PtReturnPointer:
          DiagID = diag::warn_pt_guarded_return_pointer;
          break;
      }
      PartialDiagnosticAt Warning(Loc, S.PDiag(DiagID) << Kind
                                                       << D
                                                       << LockName << LK);
      PartialDiagnosticAt Note(Loc, S.PDiag(diag::note_found_mutex_near_match)
                                        << *PossibleMatch);
      if (Verbose && POK == POK_VarAccess) {
        PartialDiagnosticAt VNote(D->getLocation(),
                                  S.PDiag(diag::note_guarded_by_declared_here)
                                      << D->getDeclName());
        Warnings.emplace_back(std::move(Warning), getNotes(Note, VNote));
      } else
        Warnings.emplace_back(std::move(Warning), getNotes(Note));
    } else {
      switch (POK) {
        case POK_VarAccess:
          DiagID = diag::warn_variable_requires_lock;
          break;
        case POK_VarDereference:
          DiagID = diag::warn_var_deref_requires_lock;
          break;
        case POK_FunctionCall:
          DiagID = diag::warn_fun_requires_lock;
          break;
        case POK_PassByRef:
          DiagID = diag::warn_guarded_pass_by_reference;
          break;
        case POK_PtPassByRef:
          DiagID = diag::warn_pt_guarded_pass_by_reference;
          break;
        case POK_ReturnByRef:
          DiagID = diag::warn_guarded_return_by_reference;
          break;
        case POK_PtReturnByRef:
          DiagID = diag::warn_pt_guarded_return_by_reference;
          break;
        case POK_PassPointer:
          DiagID = diag::warn_guarded_pass_pointer;
          break;
        case POK_PtPassPointer:
          DiagID = diag::warn_pt_guarded_pass_pointer;
          break;
        case POK_ReturnPointer:
          DiagID = diag::warn_guarded_return_pointer;
          break;
        case POK_PtReturnPointer:
          DiagID = diag::warn_pt_guarded_return_pointer;
          break;
      }
      PartialDiagnosticAt Warning(Loc, S.PDiag(DiagID) << Kind
                                                       << D
                                                       << LockName << LK);
      if (Verbose && POK == POK_VarAccess) {
        PartialDiagnosticAt Note(D->getLocation(),
                                 S.PDiag(diag::note_guarded_by_declared_here));
        Warnings.emplace_back(std::move(Warning), getNotes(Note));
      } else
        Warnings.emplace_back(std::move(Warning), getNotes());
    }
  }

  void handleNegativeNotHeld(StringRef Kind, Name LockName, Name Neg,
                             SourceLocation Loc) override {
    PartialDiagnosticAt Warning(Loc,
        S.PDiag(diag::warn_acquire_requires_negative_cap)
        << Kind << LockName << Neg);
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void handleNegativeNotHeld(const NamedDecl *D, Name LockName,
                             SourceLocation Loc) override {
    PartialDiagnosticAt Warning(
        Loc, S.PDiag(diag::warn_fun_requires_negative_cap) << D << LockName);
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void handleFunExcludesLock(StringRef Kind, Name FunName, Name LockName,
                             SourceLocation Loc) override {
    PartialDiagnosticAt Warning(Loc, S.PDiag(diag::warn_fun_excludes_mutex)
                                         << Kind << FunName << LockName);
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void handleLockAcquiredBefore(StringRef Kind, Name L1Name, Name L2Name,
                                SourceLocation Loc) override {
    PartialDiagnosticAt Warning(Loc,
      S.PDiag(diag::warn_acquired_before) << Kind << L1Name << L2Name);
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void handleBeforeAfterCycle(Name L1Name, SourceLocation Loc) override {
    PartialDiagnosticAt Warning(Loc,
      S.PDiag(diag::warn_acquired_before_after_cycle) << L1Name);
    Warnings.emplace_back(std::move(Warning), getNotes());
  }

  void enterFunction(const FunctionDecl* FD) override {
    CurrentFunction = FD;
  }

  void leaveFunction(const FunctionDecl* FD) override {
    CurrentFunction = nullptr;
  }
};
} // anonymous namespace
} // namespace threadSafety
} // namespace clang

//===----------------------------------------------------------------------===//
// -Wconsumed
//===----------------------------------------------------------------------===//

namespace clang {
namespace consumed {
namespace {
class ConsumedWarningsHandler : public ConsumedWarningsHandlerBase {

  Sema &S;
  DiagList Warnings;

public:

  ConsumedWarningsHandler(Sema &S) : S(S) {}

  void emitDiagnostics() override {
    Warnings.sort(SortDiagBySourceLocation(S.getSourceManager()));
    for (const auto &Diag : Warnings) {
      S.Diag(Diag.first.first, Diag.first.second);
      for (const auto &Note : Diag.second)
        S.Diag(Note.first, Note.second);
    }
  }

  void warnLoopStateMismatch(SourceLocation Loc,
                             StringRef VariableName) override {
    PartialDiagnosticAt Warning(Loc, S.PDiag(diag::warn_loop_state_mismatch) <<
      VariableName);

    Warnings.emplace_back(std::move(Warning), OptionalNotes());
  }

  void warnParamReturnTypestateMismatch(SourceLocation Loc,
                                        StringRef VariableName,
                                        StringRef ExpectedState,
                                        StringRef ObservedState) override {

    PartialDiagnosticAt Warning(Loc, S.PDiag(
      diag::warn_param_return_typestate_mismatch) << VariableName <<
        ExpectedState << ObservedState);

    Warnings.emplace_back(std::move(Warning), OptionalNotes());
  }

  void warnParamTypestateMismatch(SourceLocation Loc, StringRef ExpectedState,
                                  StringRef ObservedState) override {

    PartialDiagnosticAt Warning(Loc, S.PDiag(
      diag::warn_param_typestate_mismatch) << ExpectedState << ObservedState);

    Warnings.emplace_back(std::move(Warning), OptionalNotes());
  }

  void warnReturnTypestateForUnconsumableType(SourceLocation Loc,
                                              StringRef TypeName) override {
    PartialDiagnosticAt Warning(Loc, S.PDiag(
      diag::warn_return_typestate_for_unconsumable_type) << TypeName);

    Warnings.emplace_back(std::move(Warning), OptionalNotes());
  }

  void warnReturnTypestateMismatch(SourceLocation Loc, StringRef ExpectedState,
                                   StringRef ObservedState) override {

    PartialDiagnosticAt Warning(Loc, S.PDiag(
      diag::warn_return_typestate_mismatch) << ExpectedState << ObservedState);

    Warnings.emplace_back(std::move(Warning), OptionalNotes());
  }

  void warnUseOfTempInInvalidState(StringRef MethodName, StringRef State,
                                   SourceLocation Loc) override {

    PartialDiagnosticAt Warning(Loc, S.PDiag(
      diag::warn_use_of_temp_in_invalid_state) << MethodName << State);

    Warnings.emplace_back(std::move(Warning), OptionalNotes());
  }

  void warnUseInInvalidState(StringRef MethodName, StringRef VariableName,
                             StringRef State, SourceLocation Loc) override {

    PartialDiagnosticAt Warning(Loc, S.PDiag(diag::warn_use_in_invalid_state) <<
                                MethodName << VariableName << State);

    Warnings.emplace_back(std::move(Warning), OptionalNotes());
  }
};
} // anonymous namespace
} // namespace consumed
} // namespace clang

//===----------------------------------------------------------------------===//
// Unsafe buffer usage analysis.
//===----------------------------------------------------------------------===//

namespace {
class UnsafeBufferUsageReporter : public UnsafeBufferUsageHandler {
  Sema &S;
  bool SuggestSuggestions;  // Recommend -fsafe-buffer-usage-suggestions?

  // Lists as a string the names of variables in `VarGroupForVD` except for `VD`
  // itself:
  std::string listVariableGroupAsString(
      const VarDecl *VD, const ArrayRef<const VarDecl *> &VarGroupForVD) const {
    if (VarGroupForVD.size() <= 1)
      return "";

    std::vector<StringRef> VarNames;
    auto PutInQuotes = [](StringRef S) -> std::string {
      return "'" + S.str() + "'";
    };

    for (auto *V : VarGroupForVD) {
      if (V == VD)
        continue;
      VarNames.push_back(V->getName());
    }
    if (VarNames.size() == 1) {
      return PutInQuotes(VarNames[0]);
    }
    if (VarNames.size() == 2) {
      return PutInQuotes(VarNames[0]) + " and " + PutInQuotes(VarNames[1]);
    }
    assert(VarGroupForVD.size() > 3);
    const unsigned N = VarNames.size() -
                       2; // need to print the last two names as "..., X, and Y"
    std::string AllVars = "";

    for (unsigned I = 0; I < N; ++I)
      AllVars.append(PutInQuotes(VarNames[I]) + ", ");
    AllVars.append(PutInQuotes(VarNames[N]) + ", and " +
                   PutInQuotes(VarNames[N + 1]));
    return AllVars;
  }

public:
  UnsafeBufferUsageReporter(Sema &S, bool SuggestSuggestions)
    : S(S), SuggestSuggestions(SuggestSuggestions) {}

  void handleUnsafeOperation(const Stmt *Operation, bool IsRelatedToDecl,
                             ASTContext &Ctx) override {
    SourceLocation Loc;
    SourceRange Range;
    unsigned MsgParam = 0;
    NamedDecl *D = nullptr;
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Operation)) {
      Loc = ASE->getBase()->getExprLoc();
      Range = ASE->getBase()->getSourceRange();
      MsgParam = 2;
    } else if (const auto *BO = dyn_cast<BinaryOperator>(Operation)) {
      BinaryOperator::Opcode Op = BO->getOpcode();
      if (Op == BO_Add || Op == BO_AddAssign || Op == BO_Sub ||
          Op == BO_SubAssign) {
        if (BO->getRHS()->getType()->isIntegerType()) {
          Loc = BO->getLHS()->getExprLoc();
          Range = BO->getLHS()->getSourceRange();
        } else {
          Loc = BO->getRHS()->getExprLoc();
          Range = BO->getRHS()->getSourceRange();
        }
        MsgParam = 1;
      }
    } else if (const auto *UO = dyn_cast<UnaryOperator>(Operation)) {
      UnaryOperator::Opcode Op = UO->getOpcode();
      if (Op == UO_PreInc || Op == UO_PreDec || Op == UO_PostInc ||
          Op == UO_PostDec) {
        Loc = UO->getSubExpr()->getExprLoc();
        Range = UO->getSubExpr()->getSourceRange();
        MsgParam = 1;
      }
    } else {
      if (isa<CallExpr>(Operation) || isa<CXXConstructExpr>(Operation)) {
        // note_unsafe_buffer_operation doesn't have this mode yet.
        assert(!IsRelatedToDecl && "Not implemented yet!");
        MsgParam = 3;
      } else if (isa<MemberExpr>(Operation)) {
        // note_unsafe_buffer_operation doesn't have this mode yet.
        assert(!IsRelatedToDecl && "Not implemented yet!");
        auto *ME = cast<MemberExpr>(Operation);
        D = ME->getMemberDecl();
        MsgParam = 5;
      } else if (const auto *ECE = dyn_cast<ExplicitCastExpr>(Operation)) {
        QualType destType = ECE->getType();

        if (!isa<PointerType>(destType))
          return;
        destType = destType.getTypePtr()->getPointeeType();

        // If destination type is incomplete or dependent, it is unsafe to cast
        // to anyway, no need to check its type:
        if (!destType->isIncompleteType() && !destType->isDependentType()) {
          const uint64_t dSize = Ctx.getTypeSize(destType);
          QualType srcType = ECE->getSubExpr()->getType();

          assert(srcType->isPointerType());

          QualType srcPointeeType = srcType.getTypePtr()->getPointeeType();

          // Check if source type is incomplete or dependent as well:
          if (!srcPointeeType->isIncompleteType() &&
              !srcPointeeType->isDependentType()) {
            const uint64_t sSize = Ctx.getTypeSize(srcPointeeType);

            if (sSize >= dSize)
              return;
          }
        }
        if (const auto *CE = dyn_cast<CXXMemberCallExpr>(
                ECE->getSubExpr()->IgnoreParens())) {
          D = CE->getMethodDecl();
        }

        if (!D)
          return;

        MsgParam = 4;
      }
      Loc = Operation->getBeginLoc();
      Range = Operation->getSourceRange();
    }
    if (IsRelatedToDecl) {
      assert(!SuggestSuggestions &&
             "Variables blamed for unsafe buffer usage without suggestions!");
      S.Diag(Loc, diag::note_unsafe_buffer_operation) << MsgParam << Range;
    } else {
      if (D) {
        S.Diag(Loc, diag::warn_unsafe_buffer_operation)
            << MsgParam << D << Range;
      } else {
        S.Diag(Loc, diag::warn_unsafe_buffer_operation) << MsgParam << Range;
      }
      if (SuggestSuggestions) {
        S.Diag(Loc, diag::note_safe_buffer_usage_suggestions_disabled);
      }
    }
  }

  void handleUnsafeLibcCall(const CallExpr *Call, unsigned PrintfInfo,
                            ASTContext &Ctx,
                            const Expr *UnsafeArg = nullptr) override {
    unsigned DiagID = diag::warn_unsafe_buffer_libc_call;
    if (PrintfInfo & 0x8) {
      // The callee is a function with the format attribute. See the
      // documentation of PrintfInfo in UnsafeBufferUsageHandler, and
      // UnsafeLibcFunctionCallGadget::UnsafeKind.
      DiagID = diag::warn_unsafe_buffer_format_attr_call;
      PrintfInfo ^= 0x8;
    }
    S.Diag(Call->getBeginLoc(), DiagID)
        << Call->getDirectCallee() // We've checked there is a direct callee
        << Call->getSourceRange();
    if (PrintfInfo > 0) {
      SourceRange R =
          UnsafeArg ? UnsafeArg->getSourceRange() : Call->getSourceRange();
      S.Diag(R.getBegin(), diag::note_unsafe_buffer_printf_call)
          << PrintfInfo << R;
    }
  }

  void handleUnsafeOperationInContainer(const Stmt *Operation,
                                        bool IsRelatedToDecl,
                                        ASTContext &Ctx) override {
    SourceLocation Loc;
    SourceRange Range;
    unsigned MsgParam = 0;

    // This function only handles SpanTwoParamConstructorGadget so far, which
    // always gives a CXXConstructExpr.
    const auto *CtorExpr = cast<CXXConstructExpr>(Operation);
    Loc = CtorExpr->getLocation();

    S.Diag(Loc, diag::warn_unsafe_buffer_usage_in_container);
    if (IsRelatedToDecl) {
      assert(!SuggestSuggestions &&
             "Variables blamed for unsafe buffer usage without suggestions!");
      S.Diag(Loc, diag::note_unsafe_buffer_operation) << MsgParam << Range;
    }
  }

  void handleUnsafeVariableGroup(const VarDecl *Variable,
                                 const VariableGroupsManager &VarGrpMgr,
                                 FixItList &&Fixes, const Decl *D,
                                 const FixitStrategy &VarTargetTypes) override {
    assert(!SuggestSuggestions &&
           "Unsafe buffer usage fixits displayed without suggestions!");
    S.Diag(Variable->getLocation(), diag::warn_unsafe_buffer_variable)
        << Variable << (Variable->getType()->isPointerType() ? 0 : 1)
        << Variable->getSourceRange();
    if (!Fixes.empty()) {
      assert(isa<NamedDecl>(D) &&
             "Fix-its are generated only for `NamedDecl`s");
      const NamedDecl *ND = cast<NamedDecl>(D);
      bool BriefMsg = false;
      // If the variable group involves parameters, the diagnostic message will
      // NOT explain how the variables are grouped as the reason is non-trivial
      // and irrelavant to users' experience:
      const auto VarGroupForVD = VarGrpMgr.getGroupOfVar(Variable, &BriefMsg);
      unsigned FixItStrategy = 0;
      switch (VarTargetTypes.lookup(Variable)) {
      case clang::FixitStrategy::Kind::Span:
        FixItStrategy = 0;
        break;
      case clang::FixitStrategy::Kind::Array:
        FixItStrategy = 1;
        break;
      default:
        assert(false && "We support only std::span and std::array");
      };

      const auto &FD =
          S.Diag(Variable->getLocation(),
                 BriefMsg ? diag::note_unsafe_buffer_variable_fixit_together
                          : diag::note_unsafe_buffer_variable_fixit_group);

      FD << Variable << FixItStrategy;
      FD << listVariableGroupAsString(Variable, VarGroupForVD)
         << (VarGroupForVD.size() > 1) << ND;
      for (const auto &F : Fixes) {
        FD << F;
      }
    }

#ifndef NDEBUG
    if (areDebugNotesRequested())
      for (const DebugNote &Note: DebugNotesByVar[Variable])
        S.Diag(Note.first, diag::note_safe_buffer_debug_mode) << Note.second;
#endif
  }

  void handleUnsafeUniquePtrArrayAccess(const DynTypedNode &Node,
                                        bool IsRelatedToDecl,
                                        ASTContext &Ctx) override {
    SourceLocation Loc;

    Loc = Node.get<Stmt>()->getBeginLoc();
    S.Diag(Loc, diag::warn_unsafe_buffer_usage_unique_ptr_array_access)
        << Node.getSourceRange();
  }

  bool isSafeBufferOptOut(const SourceLocation &Loc) const override {
    return S.PP.isSafeBufferOptOut(S.getSourceManager(), Loc);
  }

  bool ignoreUnsafeBufferInContainer(const SourceLocation &Loc) const override {
    return S.Diags.isIgnored(diag::warn_unsafe_buffer_usage_in_container, Loc);
  }

  bool ignoreUnsafeBufferInLibcCall(const SourceLocation &Loc) const override {
    return S.Diags.isIgnored(diag::warn_unsafe_buffer_libc_call, Loc);
  }

  bool ignoreUnsafeBufferInStaticSizedArray(
      const SourceLocation &Loc) const override {
    return S.Diags.isIgnored(
        diag::warn_unsafe_buffer_usage_in_static_sized_array, Loc);
  }

  // Returns the text representation of clang::unsafe_buffer_usage attribute.
  // `WSSuffix` holds customized "white-space"s, e.g., newline or whilespace
  // characters.
  std::string
  getUnsafeBufferUsageAttributeTextAt(SourceLocation Loc,
                                      StringRef WSSuffix = "") const override {
    Preprocessor &PP = S.getPreprocessor();
    TokenValue ClangUnsafeBufferUsageTokens[] = {
        tok::l_square,
        tok::l_square,
        PP.getIdentifierInfo("clang"),
        tok::coloncolon,
        PP.getIdentifierInfo("unsafe_buffer_usage"),
        tok::r_square,
        tok::r_square};

    StringRef MacroName;

    // The returned macro (it returns) is guaranteed not to be function-like:
    MacroName = PP.getLastMacroWithSpelling(Loc, ClangUnsafeBufferUsageTokens);
    if (MacroName.empty())
      MacroName = "[[clang::unsafe_buffer_usage]]";
    return MacroName.str() + WSSuffix.str();
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// AnalysisBasedWarnings - Worker object used by Sema to execute analysis-based
//  warnings on a function, method, or block.
//===----------------------------------------------------------------------===//

sema::AnalysisBasedWarnings::Policy::Policy() {
  enableCheckFallThrough = 1;
  enableCheckUnreachable = 0;
  enableThreadSafetyAnalysis = 0;
  enableConsumedAnalysis = 0;
}

/// InterProceduralData aims to be a storage of whatever data should be passed
/// between analyses of different functions.
///
/// At the moment, its primary goal is to make the information gathered during
/// the analysis of the blocks available during the analysis of the enclosing
/// function.  This is important due to the fact that blocks are analyzed before
/// the enclosed function is even parsed fully, so it is not viable to access
/// anything in the outer scope while analyzing the block.  On the other hand,
/// re-building CFG for blocks and re-analyzing them when we do have all the
/// information (i.e. during the analysis of the enclosing function) seems to be
/// ill-designed.
class sema::AnalysisBasedWarnings::InterProceduralData {
public:
  // It is important to analyze blocks within functions because it's a very
  // common pattern to capture completion handler parameters by blocks.
  CalledOnceInterProceduralData CalledOnceData;
};

template <typename... Ts>
static bool areAnyEnabled(DiagnosticsEngine &D, SourceLocation Loc,
                          Ts... Diags) {
  return (!D.isIgnored(Diags, Loc) || ...);
}

sema::AnalysisBasedWarnings::AnalysisBasedWarnings(Sema &s)
    : S(s), IPData(std::make_unique<InterProceduralData>()),
      NumFunctionsAnalyzed(0), NumFunctionsWithBadCFGs(0), NumCFGBlocks(0),
      MaxCFGBlocksPerFunction(0), NumUninitAnalysisFunctions(0),
      NumUninitAnalysisVariables(0), MaxUninitAnalysisVariablesPerFunction(0),
      NumUninitAnalysisBlockVisits(0),
      MaxUninitAnalysisBlockVisitsPerFunction(0) {
}

// We need this here for unique_ptr with forward declared class.
sema::AnalysisBasedWarnings::~AnalysisBasedWarnings() = default;

sema::AnalysisBasedWarnings::Policy
sema::AnalysisBasedWarnings::getPolicyInEffectAt(SourceLocation Loc) {
  using namespace diag;
  DiagnosticsEngine &D = S.getDiagnostics();
  Policy P;

  // Note: The enabled checks should be kept in sync with the switch in
  // SemaPPCallbacks::PragmaDiagnostic().
  P.enableCheckUnreachable =
      PolicyOverrides.enableCheckUnreachable ||
      areAnyEnabled(D, Loc, warn_unreachable, warn_unreachable_break,
                    warn_unreachable_return, warn_unreachable_loop_increment);

  P.enableThreadSafetyAnalysis = PolicyOverrides.enableThreadSafetyAnalysis ||
                                 areAnyEnabled(D, Loc, warn_double_lock);

  P.enableConsumedAnalysis = PolicyOverrides.enableConsumedAnalysis ||
                             areAnyEnabled(D, Loc, warn_use_in_invalid_state);
  return P;
}

void sema::AnalysisBasedWarnings::clearOverrides() {
  PolicyOverrides.enableCheckUnreachable = false;
  PolicyOverrides.enableConsumedAnalysis = false;
  PolicyOverrides.enableThreadSafetyAnalysis = false;
}

bool sema::AnalysisBasedWarnings::hasLiveCFGProfile() const {
  return llvm::any_of(CFGProfiles, [&](const CFGProfileEntry &E) {
    return S.Profiles().isProfileLive(E.Name);
  });
}

static void flushDiagnostics(Sema &S, const sema::FunctionScopeInfo *fscope) {
  for (const auto &D : fscope->PossiblyUnreachableDiags)
    S.Diag(D.Loc, D.PD);
}

template <typename Iterator>
static void emitPossiblyUnreachableDiags(Sema &S, AnalysisDeclContext &AC,
                                         std::pair<Iterator, Iterator> PUDs) {

  if (PUDs.first == PUDs.second)
    return;

  for (auto I = PUDs.first; I != PUDs.second; ++I) {
    for (const Stmt *S : I->Stmts)
      AC.registerForcedBlockExpression(S);
  }

  if (AC.getCFG()) {
    CFGReverseBlockReachabilityAnalysis *Analysis =
        AC.getCFGReachablityAnalysis();

    for (auto I = PUDs.first; I != PUDs.second; ++I) {
      const auto &D = *I;
      if (llvm::all_of(D.Stmts, [&](const Stmt *St) {
            const CFGBlock *Block = AC.getBlockForRegisteredExpression(St);
            // FIXME: We should be able to assert that block is non-null, but
            // the CFG analysis can skip potentially-evaluated expressions in
            // edge cases; see test/Sema/vla-2.c.
            if (Block && Analysis)
              if (!Analysis->isReachable(&AC.getCFG()->getEntry(), Block))
                return false;
            return true;
          })) {
        S.Diag(D.Loc, D.PD);
      }
    }
  } else {
    for (auto I = PUDs.first; I != PUDs.second; ++I)
      S.Diag(I->Loc, I->PD);
  }
}

void sema::AnalysisBasedWarnings::registerVarDeclWarning(
    VarDecl *VD, clang::sema::PossiblyUnreachableDiag PUD) {
  VarDeclPossiblyUnreachableDiags.emplace(VD, PUD);
}

void sema::AnalysisBasedWarnings::issueWarningsForRegisteredVarDecl(
    VarDecl *VD) {
  if (!llvm::is_contained(VarDeclPossiblyUnreachableDiags, VD))
    return;

  AnalysisDeclContext AC(/*Mgr=*/nullptr, VD);

  AC.getCFGBuildOptions().PruneTriviallyFalseEdges = true;
  AC.getCFGBuildOptions().AddEHEdges = false;
  AC.getCFGBuildOptions().AddInitializers = true;
  AC.getCFGBuildOptions().AddImplicitDtors = true;
  AC.getCFGBuildOptions().AddTemporaryDtors = true;
  AC.getCFGBuildOptions().AddCXXNewAllocator = false;
  AC.getCFGBuildOptions().AddCXXDefaultInitExprInCtors = true;

  auto Range = VarDeclPossiblyUnreachableDiags.equal_range(VD);
  auto SecondRange =
      llvm::make_second_range(llvm::make_range(Range.first, Range.second));
  emitPossiblyUnreachableDiags(
      S, AC, std::make_pair(SecondRange.begin(), SecondRange.end()));
}

/// Base CFG build options shared by the main per-function analysis pass
/// (IssueWarnings) and the post-error profile rerun below, so both see the
/// same CFG shape.
static void configureBaseCFGBuildOptions(AnalysisDeclContext &AC) {
  // Don't generate EH edges for CallExprs as we'd like to avoid the n^2
  // explosion for destructors that can result and the compile time hit.
  AC.getCFGBuildOptions().PruneTriviallyFalseEdges = true;
  AC.getCFGBuildOptions().AddEHEdges = false;
  AC.getCFGBuildOptions().AddInitializers = true;
  AC.getCFGBuildOptions().AddImplicitDtors = true;
  AC.getCFGBuildOptions().AddParameterLifetimes = true;
  AC.getCFGBuildOptions().AddTemporaryDtors = true;
  AC.getCFGBuildOptions().AddCXXNewAllocator = false;
  AC.getCFGBuildOptions().AddCXXDefaultInitExprInCtors = true;
}

/// The always-add statement classes of the main pass's non-linearized CFG
/// configuration; shared with the post-error profile rerun.
static void addNonLinearizedAlwaysAddClasses(AnalysisDeclContext &AC) {
  AC.getCFGBuildOptions()
      .setAlwaysAdd(Stmt::BinaryOperatorClass)
      .setAlwaysAdd(Stmt::CompoundAssignOperatorClass)
      .setAlwaysAdd(Stmt::BlockExprClass)
      .setAlwaysAdd(Stmt::CStyleCastExprClass)
      .setAlwaysAdd(Stmt::DeclRefExprClass)
      .setAlwaysAdd(Stmt::ImplicitCastExprClass)
      .setAlwaysAdd(Stmt::UnaryOperatorClass);
}

/// Apply the ConfigureCFG hooks of the live CFGProfiles rows to \p Options;
/// both analysis paths call it so they build the same CFG shape.
static void configureProfileCFGOptions(Sema &S, CFG::BuildOptions &Options) {
  for (const CFGProfileEntry &E : CFGProfiles)
    if (E.ConfigureCFG && S.Profiles().isProfileLive(E.Name))
      E.ConfigureCFG(Options);
}

/// Run the ExtraPass hooks of the live CFGProfiles rows; both analysis paths
/// call it after the uninitialized-variables reporter has flushed.
static void runProfileExtraCFGPasses(Sema &S, const Decl *D,
                                     AnalysisDeclContext &AC) {
  for (const CFGProfileEntry &E : CFGProfiles)
    if (E.ExtraPass && S.Profiles().isProfileLive(E.Name))
      E.ExtraPass(S, D, AC, E);
}

/// Whether an expression under \p D's body, or under a constructor's member
/// initializers and the default member initializers they use, contains
/// errors.
static bool containsErrorExpr(const Decl *D) {
  SmallVector<const Stmt *, 32> Worklist;
  if (const Stmt *Body = D->getBody())
    Worklist.push_back(Body);
  if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(D))
    for (const CXXCtorInitializer *Init : Ctor->inits())
      if (const Expr *E = Init->getInit())
        Worklist.push_back(E);
  while (!Worklist.empty()) {
    const Stmt *S = Worklist.pop_back_val();
    if (const auto *E = dyn_cast<Expr>(S)) {
      if (E->containsErrors())
        return true;
      if (const auto *DIE = dyn_cast<CXXDefaultInitExpr>(E))
        Worklist.push_back(DIE->getExpr());
    }
    for (const Stmt *Child : S->children())
      if (Child)
        Worklist.push_back(Child);
  }
  return false;
}

/// Whether the profile-only pass may build a CFG of \p D, whose function
/// scope is \p FSI: the CFG builder and the riders assume a well-formed AST,
/// so a body whose own scope saw an unrecoverable error, or that reaches an
/// expression containing errors, is skipped (see
/// ProfilesFrameworkInternals.rst, "CFG Riders").
static bool isProfileCFGAnalyzableBody(Sema &S,
                                       const sema::FunctionScopeInfo &FSI,
                                       const Decl *D) {
  if (D->isInvalidDecl() || FSI.hasUnrecoverableErrorOccurred() ||
      S.getDiagnostics().hasFatalErrorOccurred())
    return false;
  // An expression contains errors only after an error was emitted.
  return !S.hasUncompilableErrorOccurred() || !containsErrorExpr(D);
}

/// The CFGProfiles rows ride the uninitialized-variables analysis, but
/// profile rules emit errors, so they must run even
/// where IssueWarnings skips the warning pipeline: once the TU has an
/// uncompilable error, and when warnings are disabled for the declaration
/// (-w, or a system-header decl under -fno-profiles-exempt-system-headers).
/// Run just the profile-relevant analyses for a single function on those
/// paths, if isProfileCFGAnalyzableBody allows. Diagnostics are restricted to
/// the profiles via the ProfileOnly reporter and the hooks' own gates.
static void runProfileOnlyCFGAnalysis(Sema &S,
                                      const sema::FunctionScopeInfo &FSI,
                                      const Decl *D) {
  if (!isProfileCFGAnalyzableBody(S, FSI, D))
    return;
  AnalysisDeclContext AC(/*Mgr=*/nullptr, D);

  configureBaseCFGBuildOptions(AC);
  addNonLinearizedAlwaysAddClasses(AC);
  configureProfileCFGOptions(S, AC.getCFGBuildOptions());

  if (CFG *cfg = AC.getCFG()) {
    UninitValsDiagReporter reporter(S, AC, /*ProfileOnly=*/true);
    UninitVariablesAnalysisStats stats = {};
    runUninitializedVariablesAnalysis(*cast<DeclContext>(D), *cfg, AC, reporter,
                                      stats);
  }
  runProfileExtraCFGPasses(S, D, AC);
}

// An AST Visitor that calls a callback function on each callable DEFINITION
// that is NOT in a dependent context:
class CallableVisitor : public DynamicRecursiveASTVisitor {
private:
  llvm::function_ref<void(const Decl *)> Callback;
  const Module *const TUModule;

public:
  CallableVisitor(llvm::function_ref<void(const Decl *)> Callback,
                  const Module *const TUModule)
      : Callback(Callback), TUModule(TUModule) {
    ShouldVisitTemplateInstantiations = true;
    ShouldVisitImplicitCode = false;
  }

  bool TraverseDecl(Decl *Node) override {
    // For performance reasons, only validate the current translation unit's
    // module, and not modules it depends on.
    // See https://issues.chromium.org/issues/351909443 for details.
    if (Node && Node->getOwningModule() == TUModule)
      return DynamicRecursiveASTVisitor::TraverseDecl(Node);
    return true;
  }

  bool VisitFunctionDecl(FunctionDecl *Node) override {
    if (cast<DeclContext>(Node)->isDependentContext())
      return true; // Not to analyze dependent decl
    // `FunctionDecl->hasBody()` returns true if the function has a body
    // somewhere defined.  But we want to know if this `Node` has a body
    // child.  So we use `doesThisDeclarationHaveABody`:
    if (Node->doesThisDeclarationHaveABody())
      Callback(Node);
    return true;
  }

  bool VisitBlockDecl(BlockDecl *Node) override {
    if (cast<DeclContext>(Node)->isDependentContext())
      return true; // Not to analyze dependent decl
    Callback(Node);
    return true;
  }

  bool VisitObjCMethodDecl(ObjCMethodDecl *Node) override {
    if (cast<DeclContext>(Node)->isDependentContext())
      return true; // Not to analyze dependent decl
    if (Node->hasBody())
      Callback(Node);
    return true;
  }

  bool VisitLambdaExpr(LambdaExpr *Node) override {
    return VisitFunctionDecl(Node->getCallOperator());
  }
};

// CFG build options for running the lifetime safety analysis on a function.
static void setLifetimeSafetyCFGBuildOptions(AnalysisDeclContext &AC) {
  AC.getCFGBuildOptions().PruneTriviallyFalseEdges = true;
  AC.getCFGBuildOptions().AddLifetime = true;
  AC.getCFGBuildOptions().AddParameterLifetimes = true;
  AC.getCFGBuildOptions().AddInitializers = true;
  AC.getCFGBuildOptions().AddCXXDefaultInitExprInCtors = true;
  AC.getCFGBuildOptions().setAllAlwaysAdd();
}

// Returns true when analysis-based warnings should be skipped for D: warnings
// are ignored, D is in a suppressed system header, or D is in a dependent
// context (which is handled later at instantiation time).
static bool shouldSkipAnalysisForDecl(Sema &S, const Decl *D) {
  DiagnosticsEngine &Diags = S.getDiagnostics();
  if (Diags.getIgnoreAllWarnings() ||
      (Diags.getSuppressSystemWarnings() &&
       S.SourceMgr.isInSystemHeader(D->getLocation())))
    return true;
  return cast<DeclContext>(D)->isDependentContext();
}

static void
LifetimeSafetyTUAnalysis(Sema &S, TranslationUnitDecl *TU,
                         clang::lifetimes::LifetimeSafetyStats &LSStats) {
  llvm::TimeTraceScope TimeProfile("LifetimeSafetyTUAnalysis");
  CallGraph CG;
  CG.addToCallGraph(TU);
  lifetimes::LifetimeSafetySemaHelperImpl SemaHelper(S);
  for (auto *Node : llvm::post_order(&CG)) {
    const clang::FunctionDecl *CanonicalFD =
        dyn_cast_or_null<clang::FunctionDecl>(Node->getDecl());
    if (!CanonicalFD)
      continue;
    const FunctionDecl *FD = CanonicalFD->getDefinition();
    if (!FD)
      continue;
    AnalysisDeclContext AC(nullptr, FD);
    setLifetimeSafetyCFGBuildOptions(AC);
    if (AC.getCFG())
      runLifetimeSafetyAnalysis(AC, &SemaHelper,
                                lifetimes::GetLifetimeSafetyOpts(S, FD),
                                LSStats, S.CollectStats);
  }
}

static bool shouldRunUnsafeBufferUsageAnalysis(const Sema &S,
                                               SourceLocation Loc) {
  const DiagnosticsEngine &Diags = S.getDiagnostics();
  return !Diags.isIgnored(diag::warn_unsafe_buffer_operation, Loc) ||
         !Diags.isIgnored(diag::warn_unsafe_buffer_variable, Loc) ||
         !Diags.isIgnored(diag::warn_unsafe_buffer_usage_in_container, Loc) ||
         (!Diags.isIgnored(diag::warn_unsafe_buffer_libc_call, Loc) &&
          S.getLangOpts().CPlusPlus);
}

/// \return true iff fix-its should be emitted along with -Wunsafe-buffer-usage
/// warnings
static bool shouldEmitUnsafeBufferUsageSuggestions(const Sema &S) {
  return S.getLangOpts().CPlusPlus20 && S.getDiagnostics()
                                            .getDiagnosticOptions()
                                            .ShowSafeBufferUsageSuggestions;
}

/// \return true iff an extra note that encourages users to turn on fix-its
/// should be emitted along with -Wunsafe-buffer-usage warnings
static bool shouldSuggestUnsafeBufferUsageSuggestions(const Sema &S) {
  return S.getLangOpts().CPlusPlus20 && !S.getDiagnostics()
                                             .getDiagnosticOptions()
                                             .ShowSafeBufferUsageSuggestions;
}

void clang::sema::AnalysisBasedWarnings::IssueWarnings(
     TranslationUnitDecl *TU) {
  if (!TU)
    return; // This is unexpected, give up quietly.

  DiagnosticsEngine &Diags = S.getDiagnostics();

  if (S.hasUncompilableErrorOccurred() || Diags.getIgnoreAllWarnings())
    // exit if having uncompilable errors or ignoring all warnings:
    return;

  // When the '-fsafe-buffer-usage-suggestions' option is enabled,
  // the '-Wunsafe-buffer-usage' analysis is performed at the end of the
  // translation unit. Otherwise, the analysis is more efficiently performed at
  // the end of each Decl during parsing.
  if (shouldEmitUnsafeBufferUsageSuggestions(S)) {
    UnsafeBufferUsageReporter R(S, /*SuggestSuggestions=*/false);

    // The Callback function that performs analyses:
    auto CallAnalyzers = [&](const Decl *Node) -> void {
      if (Node->hasAttr<UnsafeBufferUsageAttr>())
        return;

      // Perform unsafe buffer usage analysis:
      if (shouldRunUnsafeBufferUsageAnalysis(S, Node->getBeginLoc())) {
        clang::checkUnsafeBufferUsage(Node, R,
                                      /*EmitSuggestion =*/true);
      }

      // More analysis ...
    };
    // Emit per-function analysis-based warnings that require the whole-TU
    // reasoning. Check if any of them is enabled at all before scanning the
    // AST:
    if (shouldRunUnsafeBufferUsageAnalysis(S, SourceLocation())) {
      CallableVisitor(CallAnalyzers, TU->getOwningModule())
          .TraverseTranslationUnitDecl(TU);
    }
  }

  if (lifetimes::IsLifetimeSafetyEnabled(S, TU))
    LifetimeSafetyTUAnalysis(S, TU, LSStats);
}

void clang::sema::AnalysisBasedWarnings::IssueWarningsForImplicitFunction(
    const Decl *D) {
  // Currently this runs only lifetime safety: a default member initializer
  // applied by a synthesized constructor can bind a view/pointer member to a
  // temporary that dies at the end of construction -- a dangling field that
  // would otherwise be missed.
  if (!D || !D->getBody())
    return;
  // In TU-end mode IsLifetimeSafetyEnabled returns false for non-TU decls, so
  // such definitions are reached only via the call-graph walk, not here.
  if (!lifetimes::IsLifetimeSafetyEnabled(S, D))
    return;
  if (shouldSkipAnalysisForDecl(S, D) || S.hasUncompilableErrorOccurred())
    return;

  // A synthesized constructor can only dangle a field through an NSDMI, so skip
  // classes with no in-class field initializer. We do not narrow by member
  // type: an NSDMI can bind a borrow nested inside an aggregate member too.
  if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(D)) {
    bool HasInClassInit = false;
    for (const FieldDecl *FD : Ctor->getParent()->fields())
      if (FD->hasInClassInitializer()) {
        HasInClassInit = true;
        break;
      }
    if (!HasInClassInit)
      return;
  }

  AnalysisDeclContext AC(/*AnalysisDeclContextManager=*/nullptr, D);
  setLifetimeSafetyCFGBuildOptions(AC);

  lifetimes::LifetimeSafetySemaHelperImpl SemaHelper(S);
  if (AC.getCFG())
    lifetimes::runLifetimeSafetyAnalysis(AC, &SemaHelper,
                                         lifetimes::GetLifetimeSafetyOpts(S, D),
                                         LSStats, S.CollectStats);
}

void clang::sema::AnalysisBasedWarnings::IssueWarnings(
    sema::AnalysisBasedWarnings::Policy P, sema::FunctionScopeInfo *fscope,
    const Decl *D, QualType BlockType) {

  // We avoid doing analysis-based warnings when there are errors for
  // two reasons:
  // (1) The CFGs often can't be constructed (if the body is invalid), so
  //     don't bother trying.
  // (2) The code already has problems; running the analysis just takes more
  //     time.
  DiagnosticsEngine &Diags = S.getDiagnostics();

  if (shouldSkipAnalysisForDecl(S, D)) {
    // The dependent-context skip is unconditional (handled at instantiation
    // time); the -w and system-header skips must not silence profile rules,
    // which are errors (see runProfileOnlyCFGAnalysis).
    if (!cast<DeclContext>(D)->isDependentContext() && hasLiveCFGProfile() &&
        !S.Context.isProfileExemptSystemHeaderLoc(D->getLocation()))
      runProfileOnlyCFGAnalysis(S, *fscope, D);
    return;
  }

  // Cached for the uses below (liveness cannot change within one
  // invocation); the skip path above queries hasLiveCFGProfile directly.
  const bool CFGProfileLive = hasLiveCFGProfile();

  if (S.hasUncompilableErrorOccurred()) {
    // Flush out any possibly unreachable diagnostics.
    flushDiagnostics(S, fscope);
    // Profile rules are errors: keep the CFG profile pass alive after a TU
    // error (see runProfileOnlyCFGAnalysis). Other analyses keep the
    // early-out.
    if (CFGProfileLive)
      runProfileOnlyCFGAnalysis(S, *fscope, D);
    return;
  }

  const Stmt *Body = D->getBody();
  assert(Body);

  // Construct the analysis context with the specified CFG build options.
  AnalysisDeclContext AC(/* AnalysisDeclContextManager */ nullptr, D);

  configureBaseCFGBuildOptions(AC);

  bool EnableLifetimeSafetyAnalysis = lifetimes::IsLifetimeSafetyEnabled(S, D);

  // Force that certain expressions appear as CFGElements in the CFG.  This
  // is used to speed up various analyses.
  // FIXME: This isn't the right factoring.  This is here for initial
  // prototyping, but we need a way for analyses to say what expressions they
  // expect to always be CFGElements and then fill in the BuildOptions
  // appropriately.  This is essentially a layering violation.
  if (P.enableCheckUnreachable || P.enableThreadSafetyAnalysis ||
      P.enableConsumedAnalysis || EnableLifetimeSafetyAnalysis) {
    // Unreachable code analysis and thread safety require a linearized CFG.
    AC.getCFGBuildOptions().setAllAlwaysAdd();
  } else {
    addNonLinearizedAlwaysAddClasses(AC);
    if (CFGProfileLive)
      configureProfileCFGOptions(S, AC.getCFGBuildOptions());
  }
  if (EnableLifetimeSafetyAnalysis)
    AC.getCFGBuildOptions().AddLifetime = true;

  // Install the logical handler.
  std::optional<LogicalErrorHandler> LEH;
  if (LogicalErrorHandler::hasActiveDiagnostics(Diags, D->getBeginLoc())) {
    LEH.emplace(S);
    AC.getCFGBuildOptions().Observer = &*LEH;
  }

  // Emit delayed diagnostics.
  auto &PUDs = fscope->PossiblyUnreachableDiags;
  emitPossiblyUnreachableDiags(S, AC, std::make_pair(PUDs.begin(), PUDs.end()));

  // Warning: check missing 'return'
  if (P.enableCheckFallThrough) {
    const CheckFallThroughDiagnostics &CD =
        (isa<BlockDecl>(D) ? CheckFallThroughDiagnostics::MakeForBlock()
         : (isa<CXXMethodDecl>(D) &&
            cast<CXXMethodDecl>(D)->getOverloadedOperator() == OO_Call &&
            cast<CXXMethodDecl>(D)->getParent()->isLambda())
             ? CheckFallThroughDiagnostics::MakeForLambda()
             : (fscope->isCoroutine()
                    ? CheckFallThroughDiagnostics::MakeForCoroutine(D)
                    : CheckFallThroughDiagnostics::MakeForFunction(S, D)));
    CheckFallThroughForBody(S, D, Body, BlockType, CD, AC);
  }

  // Warning: check for unreachable code
  if (P.enableCheckUnreachable) {
    // Only check for unreachable code on non-template instantiations.
    // Different template instantiations can effectively change the control-flow
    // and it is very difficult to prove that a snippet of code in a template
    // is unreachable for all instantiations.
    bool isTemplateInstantiation = false;
    if (const FunctionDecl *Function = dyn_cast<FunctionDecl>(D))
      isTemplateInstantiation = Function->isTemplateInstantiation();
    if (!isTemplateInstantiation)
      CheckUnreachable(S, AC);
  }

  // Check for thread safety violations
  if (P.enableThreadSafetyAnalysis) {
    SourceLocation FL = AC.getDecl()->getLocation();
    SourceLocation FEL = AC.getDecl()->getEndLoc();
    threadSafety::ThreadSafetyReporter Reporter(S, FL, FEL);
    if (!Diags.isIgnored(diag::warn_thread_safety_beta, D->getBeginLoc()))
      Reporter.setIssueBetaWarnings(true);
    if (!Diags.isIgnored(diag::warn_thread_safety_verbose, D->getBeginLoc()))
      Reporter.setVerbose(true);

    threadSafety::runThreadSafetyAnalysis(AC, Reporter,
                                          &S.ThreadSafetyDeclCache);
    Reporter.emitDiagnostics();
  }

  // Check for violations of consumed properties.
  if (P.enableConsumedAnalysis) {
    consumed::ConsumedWarningsHandler WarningHandler(S);
    consumed::ConsumedAnalyzer Analyzer(WarningHandler);
    Analyzer.run(AC);
  }

  if (CFGProfileLive ||
      !Diags.isIgnored(diag::warn_uninit_var, D->getBeginLoc()) ||
      !Diags.isIgnored(diag::warn_sometimes_uninit_var, D->getBeginLoc()) ||
      !Diags.isIgnored(diag::warn_maybe_uninit_var, D->getBeginLoc()) ||
      !Diags.isIgnored(diag::warn_uninit_const_reference, D->getBeginLoc()) ||
      !Diags.isIgnored(diag::warn_uninit_const_pointer, D->getBeginLoc())) {
    if (CFG *cfg = AC.getCFG()) {
      UninitValsDiagReporter reporter(S, AC);
      UninitVariablesAnalysisStats stats = {};
      runUninitializedVariablesAnalysis(*cast<DeclContext>(D), *cfg, AC,
                                        reporter, stats);

      if (S.CollectStats && stats.NumVariablesAnalyzed > 0) {
        ++NumUninitAnalysisFunctions;
        NumUninitAnalysisVariables += stats.NumVariablesAnalyzed;
        NumUninitAnalysisBlockVisits += stats.NumBlockVisits;
        MaxUninitAnalysisVariablesPerFunction =
            std::max(MaxUninitAnalysisVariablesPerFunction,
                     stats.NumVariablesAnalyzed);
        MaxUninitAnalysisBlockVisitsPerFunction =
            std::max(MaxUninitAnalysisBlockVisitsPerFunction,
                     stats.NumBlockVisits);
      }
    }
  }

  if (CFGProfileLive)
    runProfileExtraCFGPasses(S, D, AC);

  if (EnableLifetimeSafetyAnalysis) {
    if (AC.getCFG()) {
      lifetimes::LifetimeSafetySemaHelperImpl LifetimeSafetySemaHelper(S);
      lifetimes::runLifetimeSafetyAnalysis(
          AC, &LifetimeSafetySemaHelper, lifetimes::GetLifetimeSafetyOpts(S, D),
          LSStats, S.CollectStats);
    }
  }
  // Check for violations of "called once" parameter properties.
  if (S.getLangOpts().ObjC && !S.getLangOpts().CPlusPlus &&
      shouldAnalyzeCalledOnceParameters(Diags, D->getBeginLoc())) {
    if (AC.getCFG()) {
      CalledOnceCheckReporter Reporter(S, IPData->CalledOnceData);
      checkCalledOnceParameters(
          AC, Reporter,
          shouldAnalyzeCalledOnceConventions(Diags, D->getBeginLoc()));
    }
  }

  bool FallThroughDiagFull =
      !Diags.isIgnored(diag::warn_unannotated_fallthrough, D->getBeginLoc());
  bool FallThroughDiagPerFunction = !Diags.isIgnored(
      diag::warn_unannotated_fallthrough_per_function, D->getBeginLoc());
  if (FallThroughDiagFull || FallThroughDiagPerFunction ||
      fscope->HasFallthroughStmt) {
    DiagnoseSwitchLabelsFallthrough(S, AC, !FallThroughDiagFull);
  }

  if (S.getLangOpts().ObjCWeak &&
      !Diags.isIgnored(diag::warn_arc_repeated_use_of_weak, D->getBeginLoc()))
    diagnoseRepeatedUseOfWeak(S, fscope, D, AC.getParentMap());


  // Check for infinite self-recursion in functions
  if (!Diags.isIgnored(diag::warn_infinite_recursive_function,
                       D->getBeginLoc())) {
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
      checkRecursiveFunction(S, FD, Body, AC);
    }
  }

  // Check for throw out of non-throwing function.
  if (!Diags.isIgnored(diag::warn_throw_in_noexcept_func, D->getBeginLoc()))
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D))
      if (S.getLangOpts().CPlusPlus && !fscope->isCoroutine() && isNoexcept(FD))
        checkThrowInNonThrowingFunc(S, FD, AC);

  // If none of the previous checks caused a CFG build, trigger one here
  // for the logical error handler.
  if (LogicalErrorHandler::hasActiveDiagnostics(Diags, D->getBeginLoc())) {
    AC.getCFG();
  }

  // Clear any of our policy overrides.
  clearOverrides();

  // Collect statistics about the CFG if it was built.
  if (S.CollectStats && AC.isCFGBuilt()) {
    ++NumFunctionsAnalyzed;
    if (CFG *cfg = AC.getCFG()) {
      // If we successfully built a CFG for this context, record some more
      // detail information about it.
      NumCFGBlocks += cfg->getNumBlockIDs();
      MaxCFGBlocksPerFunction = std::max(MaxCFGBlocksPerFunction,
                                         cfg->getNumBlockIDs());
    } else {
      ++NumFunctionsWithBadCFGs;
    }
  }

  // If the '-fsafe-buffer-usage-suggestions' option is not specified or C++20
  // is not available, '-Wunsafe-buffer-usage' warnings are analyzed at the end
  // of each Decl. This is because only '-fsafe-buffer-usage-suggestions'
  // requires visibility of the whole translation unit, hence the cumbersome
  // post-TU analysis, which deserializes and scans pre-compiled ASTs.
  if (!shouldEmitUnsafeBufferUsageSuggestions(S) &&
      !D->hasAttr<UnsafeBufferUsageAttr>()) {
    UnsafeBufferUsageReporter R(S,
                                shouldSuggestUnsafeBufferUsageSuggestions(S));

    // Perform unsafe buffer usage analysis:
    if (shouldRunUnsafeBufferUsageAnalysis(S, D->getBeginLoc())) {
      clang::checkUnsafeBufferUsage(
          D, R, /*UnsafeBufferUsageShouldEmitSuggestions=*/false);
    }
  }
}

void clang::sema::AnalysisBasedWarnings::PrintStats() const {
  llvm::errs() << "\n*** Analysis Based Warnings Stats:\n";

  unsigned NumCFGsBuilt = NumFunctionsAnalyzed - NumFunctionsWithBadCFGs;
  unsigned AvgCFGBlocksPerFunction =
      !NumCFGsBuilt ? 0 : NumCFGBlocks/NumCFGsBuilt;
  llvm::errs() << NumFunctionsAnalyzed << " functions analyzed ("
               << NumFunctionsWithBadCFGs << " w/o CFGs).\n"
               << "  " << NumCFGBlocks << " CFG blocks built.\n"
               << "  " << AvgCFGBlocksPerFunction
               << " average CFG blocks per function.\n"
               << "  " << MaxCFGBlocksPerFunction
               << " max CFG blocks per function.\n";

  unsigned AvgUninitVariablesPerFunction = !NumUninitAnalysisFunctions ? 0
      : NumUninitAnalysisVariables/NumUninitAnalysisFunctions;
  unsigned AvgUninitBlockVisitsPerFunction = !NumUninitAnalysisFunctions ? 0
      : NumUninitAnalysisBlockVisits/NumUninitAnalysisFunctions;
  llvm::errs() << NumUninitAnalysisFunctions
               << " functions analyzed for uninitialiazed variables\n"
               << "  " << NumUninitAnalysisVariables << " variables analyzed.\n"
               << "  " << AvgUninitVariablesPerFunction
               << " average variables per function.\n"
               << "  " << MaxUninitAnalysisVariablesPerFunction
               << " max variables per function.\n"
               << "  " << NumUninitAnalysisBlockVisits << " block visits.\n"
               << "  " << AvgUninitBlockVisitsPerFunction
               << " average block visits per function.\n"
               << "  " << MaxUninitAnalysisBlockVisitsPerFunction
               << " max block visits per function.\n";
  clang::lifetimes::printStats(LSStats);
}
