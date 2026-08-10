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
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
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
/// are optional (see ProfilesFrameworkInternals.rst, "Pattern 2").
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
/// ProfilesFrameworkInternals.rst, "Pattern 2").
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
                  diag::err_profile_cfg_hooks_test, LE->getBeginLoc(),
                  /*D=*/nullptr, /*PostParse=*/true))
            S.Diag(LE->getBeginLoc(), diag::err_profile_cfg_hooks_test)
                << Entry.Name;
}

/// VarExempt hook of the std::init row: std::byte may be read while
/// uninitialized (P4222R2 §4).
static bool stdInitVarExempt(Sema &S, const VarDecl *VD) {
  return S.Context.getBaseElementType(VD->getType())->isStdByteType();
}

/// ConfigureCFG hook of the std::init row: the ctor-body pass scans
/// this-capturing lambda bodies from the LambdaExpr's element; without
/// always-add, a lambda in a non-statement position never gets its own CFG
/// element.
static void stdInitConfigureCFG(CFG::BuildOptions &Options) {
  Options.setAlwaysAdd(Stmt::LambdaExprClass);
}

static void checkInitProfileCtorBody(Sema &S, const CXXConstructorDecl *Ctor,
                                     AnalysisDeclContext &AC,
                                     const CFGProfileEntry &Entry);
static void checkInitProfileLocalMembers(Sema &S, AnalysisDeclContext &AC,
                                         const CFGProfileEntry &Entry);

/// ExtraPass of the std::init row: the member read-before-init passes -- the
/// constructor-body check and the local-aggregate member check (the two
/// track disjoint storage, this-members versus locals; the latter runs for
/// every definition, constructors included). The passes must recover the
/// same event stream from either CFG shape (see
/// ProfilesFrameworkInternals.rst, "Pattern 2"), so their extraction loops
/// match only always-add classes (lvalue-to-rvalue ImplicitCastExpr,
/// (compound-)assignment BinaryOperator, ++/-- UnaryOperator, DeclRefExpr,
/// LambdaExpr via stdInitConfigureCFG) or unconditional CFG elements
/// (CFGInitializer, CallExpr, DeclStmt); a new extraction arm means
/// always-adding its class in stdInitConfigureCFG.
static void runStdInitMemberReadChecks(Sema &S, const Decl *D,
                                       AnalysisDeclContext &AC,
                                       const CFGProfileEntry &Entry) {
  if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(D))
    checkInitProfileCtorBody(S, Ctor, AC, Entry);
  checkInitProfileLocalMembers(S, AC, Entry);
}

constexpr CFGProfileEntry CFGProfiles[] = {
    {"test::uninit_read", diag::err_profile_uninit_read},
    {"test::cfg_hooks", diag::err_profile_cfg_hooks_uninit_read,
     &isTestCFGHooksExemptVar, &configureTestCFGHooksCFG, &runTestCFGHooksPass},
    {"std::init", diag::err_init_uninit_read, &stdInitVarExempt,
     &stdInitConfigureCFG, &runStdInitMemberReadChecks},
};

/// Diagnose an uninitialized read of \p vd under the CFGProfiles rows,
/// reporting a self-init at its root cause and otherwise the first recorded
/// real use; returns true if a diagnostic was emitted. A row whose VarExempt
/// hook exempts \p vd takes no part in either arm.
static bool
tryDiagnoseProfileUninitRead(Sema &S, AnalysisDeclContext &AC,
                             const VarDecl *vd, bool hasSelfInit,
                             const SmallVectorImpl<UninitUse> &vec) {
  SmallVector<const CFGProfileEntry *, 4> Rows;
  for (const CFGProfileEntry &E : CFGProfiles)
    if (E.DiagID != 0 && (!E.VarExempt || !E.VarExempt(S, vd)))
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
              E->DiagID, Init->getBeginLoc(), /*D=*/nullptr,
              /*PostParse=*/true))
        continue;
      S.Diag(Init->getBeginLoc(), E->DiagID) << E->Name << vd->getDeclName();
      S.Diag(vd->getLocation(), diag::note_var_declared_here)
          << vd->getDeclName();
      return true;
    }
  }

  // If a CFG-uninit-analysis-requesting profile is enforced at any real use
  // site and is not suppressed there, emit the profile diagnostic and skip
  // the default warning path entirely.
  for (const UninitUse &U : vec) {
    // A const-reference binding or address-taking use is not a read; it is
    // pointer/reference-binding territory, checked at the binding site.
    if (U.isConstRefOrPtrUse())
      continue;
    for (const CFGProfileEntry *E : Rows) {
      if (!S.Profiles().shouldEmitProfileViolation(
              E->DiagID, U.getUser()->getBeginLoc(), /*D=*/nullptr,
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

// True if E denotes the current object: `this` (the implicit/explicit pointer
// of an arrow access) or `*this` (the object lvalue of a dot access), seen
// through the transparent casts the parse-order credit sees through
// (`(Base &)*this`, `(Base *)this`).
static bool isCurrentObjectBase(const Expr *E) {
  E = SemaProfiles::ignoreTransparentCasts(E);
  if (isa<CXXThisExpr>(E))
    return true;
  const auto *UO = dyn_cast<UnaryOperator>(E);
  return UO && UO->getOpcode() == UO_Deref &&
         isa<CXXThisExpr>(
             SemaProfiles::ignoreTransparentCasts(UO->getSubExpr()));
}

// If E names a non-static data member of the current object (`this->m`, the
// implicit `m`, or the equivalent `(*this).m`), return that field; otherwise
// null. Access through any other object (e.g. `other.m`) is not the current
// object's member. Transparent casts are peeled at both the member and base
// positions -- `(int &)m` denotes the same storage as `m` (paper §4.3), and
// the parse-order credit already sees through them -- so a store through a
// reference cast credits and a read through one is detected.
static const FieldDecl *getCurrentObjectMember(const Expr *E) {
  const auto *ME =
      dyn_cast<MemberExpr>(SemaProfiles::ignoreTransparentCasts(E));
  if (!ME || !isCurrentObjectBase(ME->getBase()))
    return nullptr;
  return dyn_cast<FieldDecl>(ME->getMemberDecl());
}

// A class with a user-provided constructor is trusted (paper §5.1): its
// constructor body may have assigned a member, which local analysis cannot
// see, so its members are not flow-tracked.
static bool hasUserProvidedCtor(const CXXRecordDecl *RD) {
  return llvm::any_of(RD->ctors(), [](const CXXConstructorDecl *C) {
    return C->isUserProvided();
  });
}

// Visit the candidate fields of RD and of its non-virtual, constructor-less
// base classes, recursively.
template <typename Fn>
static void forEachCandidateUninitField(const CXXRecordDecl *RD, Fn Visit) {
  SmallVector<const CXXRecordDecl *, 4> RecordStack(1, RD);
  while (!RecordStack.empty()) {
    const CXXRecordDecl *Cur = RecordStack.pop_back_val();
    for (const FieldDecl *F : Cur->fields())
      Visit(F);
    for (const CXXBaseSpecifier &BS : Cur->bases()) {
      if (BS.isVirtual())
        continue;
      const CXXRecordDecl *BRD = BS.getType()->getAsCXXRecordDecl();
      if (BRD && BRD->hasDefinition() &&
          !hasUserProvidedCtor(BRD->getDefinition()))
        RecordStack.push_back(BRD->getDefinition());
    }
  }
}

// Collect the flow-trackable members of RD into Members/Index: [[uninit]]
// built-in scalar (arithmetic or enum) members whose assignment counts as
// initialization (§4.5), including those inherited from non-virtual,
// constructor-less bases -- nothing can have assigned them before the
// containing object's user code runs, so tracking them is sound. std::byte
// is exempt (§4.5), matching R1/R2. Class-type and array members (which
// would need construct_at flow modeling) and pointers (banned with
// [[uninit]] by R8) are out of scope.
static void collectTrackedUninitMembers(
    Sema &S, const CXXRecordDecl *RD,
    SmallVectorImpl<const FieldDecl *> &Members,
    llvm::DenseMap<const FieldDecl *, unsigned> &Index) {
  forEachCandidateUninitField(RD, [&](const FieldDecl *F) {
    if (!F->hasAttr<UninitAttr>() || !F->getDeclName() ||
        F->hasInClassInitializer())
      return;
    QualType T = F->getType();
    if (!T->isIntegralOrEnumerationType() && !T->isFloatingType())
      return;
    if (S.Context.getBaseElementType(T)->isStdByteType())
      return;
    if (Index.count(F))
      return;
    Index[F] = Members.size();
    Members.push_back(F);
  });
}

// Per-block ordered events recovered from the linearized CFG by the
// definite-assignment passes: a value load of tracked entity Idx is a Read;
// an assignment is a Write that marks it assigned after the RHS is
// evaluated; a compound assignment or built-in ++/-- both reads and then
// writes it; a Copy makes entity Idx's state the source entity SrcIdx's at
// that point (the local pass's tracked-copy transfer); a Kill clears the
// assigned bit -- destruction makes the storage uninitialized again
// ("Lifetimes", p4222r2.md:922-927) -- and never reports by itself.
enum class DefAssignEventKind { Read, Write, ReadWrite, Copy, Kill };
struct DefAssignEvent {
  DefAssignEventKind Kind;
  unsigned Idx;
  const Expr *E;
  unsigned SrcIdx = 0; // Copy only: the source entity.
};

// Replay a block's events over State: the definite-assignment engine's one
// block-level transfer function, shared by the fixpoint and the reporting
// replay so the two can never disagree. When Offending is non-null, a read
// of an entity not definitely assigned at its program point is collected.
static void
applyDefAssignEvents(ArrayRef<DefAssignEvent> BlockEvents,
                     llvm::BitVector &State,
                     std::vector<SmallVector<const Expr *, 2>> *Offending) {
  for (const DefAssignEvent &Ev : BlockEvents) {
    switch (Ev.Kind) {
    case DefAssignEventKind::Read:
      if (Offending && !State.test(Ev.Idx))
        (*Offending)[Ev.Idx].push_back(Ev.E);
      break;
    case DefAssignEventKind::Write:
      State.set(Ev.Idx);
      break;
    case DefAssignEventKind::ReadWrite:
      if (Offending && !State.test(Ev.Idx))
        (*Offending)[Ev.Idx].push_back(Ev.E);
      State.set(Ev.Idx);
      break;
    case DefAssignEventKind::Copy:
      if (State.test(Ev.SrcIdx))
        State.set(Ev.Idx);
      else
        State.reset(Ev.Idx);
      break;
    case DefAssignEventKind::Kill:
      State.reset(Ev.Idx);
      break;
    }
  }
}

// The forward "definitely assigned" dataflow under both member passes:
// nothing is assigned at function entry; a block's entry is the
// intersection over its predecessors' exits (an entity is definitely
// assigned at a point only if assigned on every incoming path, the paper's
// all-branches rule, §1.2/§1.3) -- so a Kill on any incoming path clears
// the bit at the join: destruction is may-kill under the same rule.
// A block's transfer is the event replay
// above -- the same replay the reporting pass uses, and a caller-supplied
// transfer is deliberately not offered: a new event kind is added here,
// once, instead of re-opening the fixpoint/replay divergence class.
// Unprocessed (unreachable) predecessors keep the all-assigned top, so
// unreachable code is never flagged. The transfer is monotone *as a
// function* (a larger input state yields a larger output state:
// Read/Write/ReadWrite only set bits, Copy projects the source bit, and
// Kill is a constant function of its bit -- constants are monotone),
// not set-only -- Copy and Kill can clear a bit -- so termination follows
// from every
// block's exit only ever descending from the all-assigned top in the finite
// lattice. Returns, per tracked entity, the reads at program points where
// the entity is not definitely assigned.
static std::vector<SmallVector<const Expr *, 2>>
runDefiniteAssignment(CFG &cfg, AnalysisDeclContext &AC, unsigned NumTracked,
                      ArrayRef<SmallVector<DefAssignEvent, 4>> Events) {
  const unsigned NumBlocks = cfg.getNumBlockIDs();
  std::vector<llvm::BitVector> EntryState(NumBlocks,
                                          llvm::BitVector(NumTracked, true));
  std::vector<llvm::BitVector> ExitState(NumBlocks,
                                         llvm::BitVector(NumTracked, true));
  const CFGBlock &CFGEntry = cfg.getEntry();
  ForwardDataflowWorklist Worklist(cfg, AC);
  Worklist.enqueueBlock(&CFGEntry);
  llvm::BitVector Visited(NumBlocks, false);
  while (const CFGBlock *B = Worklist.dequeue()) {
    llvm::BitVector In(NumTracked, true);
    if (B == &CFGEntry) {
      In = llvm::BitVector(NumTracked, false);
    } else {
      bool First = true;
      for (const CFGBlock *Pred : B->preds()) {
        if (!Pred)
          continue;
        if (First) {
          In = ExitState[Pred->getBlockID()];
          First = false;
        } else {
          In &= ExitState[Pred->getBlockID()];
        }
      }
    }
    EntryState[B->getBlockID()] = In;
    applyDefAssignEvents(Events[B->getBlockID()], In, /*Offending=*/nullptr);
    // Enqueue-skip subtlety: ExitState starts at the all-assigned top, and a
    // block whose computed exit *equals* its stored exit enqueues no
    // successors. Before Kill existed every transfer mapped top to top
    // (Read/Write/ReadWrite set bits; Copy projects a bit that is set at
    // top), so a block reached only through all-top exits genuinely had a
    // top exit and skipping it forever was sound. Kill breaks that
    // property -- a kill block entered at top exits *below* top -- so a
    // reachable block's first visit must propagate even when its computed
    // exit equals the stored top (the Visited disjunct below) -- which also
    // makes Visited mean exactly "reachable from the entry". Unreachable
    // blocks are still never enqueued (only successors of dequeued blocks
    // are), so they keep the all-assigned top: an unreachable destroy
    // spoils no reachable join. The reporting replay below walks only the
    // visited blocks: walking the unreachable ones from top used to be a
    // harmless no-op (no transfer could drop below top), but a Kill
    // followed by a read inside one would now flag unreachable code.
    // Lowering the initial ExitState, seeding EntryState differently,
    // or replaying unvisited blocks would each break the others'
    // assumption; change them together or not at all.
    bool FirstVisit = !Visited[B->getBlockID()];
    Visited[B->getBlockID()] = true;
    if (FirstVisit || In != ExitState[B->getBlockID()]) {
      ExitState[B->getBlockID()] = In;
      Worklist.enqueueSuccessors(B);
    }
  }

  // Replay each visited (reachable) block from its fixpoint entry state and
  // collect reads of an entity that is not yet definitely assigned at that
  // point.
  std::vector<SmallVector<const Expr *, 2>> Offending(NumTracked);
  for (const CFGBlock *B : cfg) {
    if (!Visited[B->getBlockID()])
      continue;
    llvm::BitVector Assigned = EntryState[B->getBlockID()];
    applyDefAssignEvents(Events[B->getBlockID()], Assigned, &Offending);
  }
  return Offending;
}

// Report at the first offending read (in source order) that is not
// suppressed, once per tracked entity, mirroring the local-variable
// reporter. The profile name and rule arrive from the std::init CFGProfiles
// row.
static void reportMemberReadsBeforeInit(
    Sema &S, AnalysisDeclContext &AC,
    MutableArrayRef<SmallVector<const Expr *, 2>> Offending,
    ArrayRef<const FieldDecl *> TrackedFields, StringRef Name) {
  for (unsigned I = 0, N = Offending.size(); I != N; ++I) {
    if (Offending[I].empty())
      continue;
    llvm::sort(Offending[I], [&](const Expr *A, const Expr *B) {
      return S.SourceMgr.isBeforeInTranslationUnit(A->getBeginLoc(),
                                                   B->getBeginLoc());
    });
    for (const Expr *R : Offending[I]) {
      if (!S.Profiles().shouldEmitProfileViolation(
              diag::err_init_member_read_before_init, R->getBeginLoc(),
              /*D=*/nullptr, /*PostParse=*/true))
        continue;
      S.Diag(R->getBeginLoc(), diag::err_init_member_read_before_init)
          << Name << TrackedFields[I]->getDeclName();
      S.Diag(TrackedFields[I]->getLocation(),
             diag::note_init_uninit_member_here)
          << TrackedFields[I]->getDeclName();
      break;
    }
  }
}

// Peel a lifecycle call's argument down to the expression that names the
// storage: the transparent casts the parse-time recognizers see through
// (§4.3: a cast marked pointer is itself marked), and, through \p Glvalue,
// a top-level `&`. Returns the peeled argument (`&m`, `m`, `this`); Glvalue
// is the same expression with the `&` stripped (`m`), the shape
// getCurrentObjectMember and the local pass's lookups take.
static const Expr *peelLifecycleArgument(const Expr *Arg,
                                         const Expr *&Glvalue) {
  Arg = SemaProfiles::ignoreTransparentCasts(Arg);
  Glvalue = Arg;
  if (const auto *AddrOf = dyn_cast<UnaryOperator>(Arg);
      AddrOf && AddrOf->getOpcode() == UO_AddrOf)
    Glvalue = AddrOf->getSubExpr();
  return Arg;
}

// A call to a lifecycle-annotated function changes the assignment state of
// the current-object storage bound to its parameters, as real dataflow
// facts, not parse-order credit -- §1.2's all-branches rule governs both
// directions. A [[now_uninit]] callee destroys the storage bound to each
// pointer/reference parameter (no marker key: the attribute's contract
// covers every such argument), so a member passed as `&m` / `m` -- or
// every tracked member when `this` / `*this` itself is passed -- has its
// assigned bit killed: destruction makes the storage uninitialized again
// ("Lifetimes", p4222r2.md:922-927), and a destroy under a branch already
// spoils a read at the join (may-kill). A [[now_init]] callee initializes
// the storage bound to each of its [[ref_to_uninit]] parameters (P4222R2
// §6.2) -- the paper's sanctioned exception to the ctor-body pass's strict
// assignment-only crediting. A current-object member passed as `&m` / `m`
// becomes assigned at the call element (its argument-subexpression events,
// e.g. a read of another member, precede it in the block); passing `this`
// / `*this` itself to a marked parameter hands the callee the whole object
// to initialize, so every tracked member is assigned. Kill events are
// appended before Write events -- append order is replay order -- so a
// dual-attributed reinitializer nets to assigned. Storage-release callees
// (free) are deliberately not kills: a release ends no object's lifetime,
// and the trusted/by-name allocator split stays out of this file. A plain
// callee earns nothing.
static void appendLifecycleCallEvents(
    const CallExpr *CE, unsigned NumMembers,
    const llvm::DenseMap<const FieldDecl *, unsigned> &Index,
    SmallVectorImpl<DefAssignEvent> &BlockEvents) {
  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee ||
      (!Callee->hasAttr<NowInitAttr>() && !Callee->hasAttr<NowUninitAttr>()))
    return;
  // Zip declared parameters with arguments. A member operator called
  // through CXXOperatorCallExpr receives the object as argument 0
  // ahead of its declared parameters -- for a C++23 static operator
  // too, whose object argument is still evaluated -- so skip it. An
  // explicit-object member function instead declares its object as
  // parameter 0, so its mapping is already direct.
  unsigned ArgOffset = 0;
  if (isa<CXXOperatorCallExpr>(CE))
    if (const auto *MD = dyn_cast<CXXMethodDecl>(Callee);
        MD && !MD->isExplicitObjectMemberFunction())
      ArgOffset = 1;
  if (Callee->hasAttr<NowUninitAttr>()) {
    for (unsigned PI = 0, NP = Callee->getNumParams(); PI != NP; ++PI) {
      if (PI + ArgOffset >= CE->getNumArgs())
        break;
      QualType PT = Callee->getParamDecl(PI)->getType();
      if (!PT->isPointerType() && !PT->isReferenceType())
        continue;
      const Expr *G = nullptr;
      const Expr *Arg = peelLifecycleArgument(CE->getArg(PI + ArgOffset), G);
      if (const FieldDecl *F = getCurrentObjectMember(G)) {
        auto It = Index.find(F);
        if (It == Index.end())
          continue;
        BlockEvents.push_back({DefAssignEventKind::Kill, It->second, CE});
      } else if (isCurrentObjectBase(Arg)) {
        for (unsigned Idx = 0; Idx != NumMembers; ++Idx)
          BlockEvents.push_back({DefAssignEventKind::Kill, Idx, CE});
      }
    }
  }
  if (!Callee->hasAttr<NowInitAttr>())
    return;
  for (unsigned PI = 0, NP = Callee->getNumParams(); PI != NP; ++PI) {
    if (PI + ArgOffset >= CE->getNumArgs())
      break;
    if (!Callee->getParamDecl(PI)->hasAttr<RefToUninitAttr>())
      continue;
    const Expr *G = nullptr;
    const Expr *Arg = peelLifecycleArgument(CE->getArg(PI + ArgOffset), G);
    if (const FieldDecl *F = getCurrentObjectMember(G)) {
      auto It = Index.find(F);
      if (It == Index.end())
        continue;
      BlockEvents.push_back({DefAssignEventKind::Write, It->second, CE});
    } else if (isCurrentObjectBase(Arg)) {
      for (unsigned Idx = 0; Idx != NumMembers; ++Idx)
        BlockEvents.push_back({DefAssignEventKind::Write, Idx, CE});
    }
  }
}

// The lambda body is a separate function and never appears in the enclosing
// constructor's CFG, but a this-capturing lambda can read members the moment
// it is created (it may be invoked immediately). A `*this` capture
// copy-constructs the whole object at creation, reading every member right
// there: unless the class has a user-provided copy constructor (opaque and
// trusted per the paper's §5.1 trust-the-constructor principle,
// \p StarThisCopyTrusted -- consistent with this pass's other trust gates),
// append one Read per tracked member at the LambdaExpr, and never
// body-scan -- body accesses go to the *copy*, so attributing them to the
// original would be wrong either way. A plain `this` capture instead reads
// nothing at creation: treat every member read in its body -- and in nested
// lambda bodies, reached through children() -- as a Read at the LambdaExpr's
// program point, except that a nested lambda capturing `*this` gets the
// whole-object reads at its own LambdaExpr (its creation runs when the outer
// body does, which may be immediately) and is not descended into. Writes in
// a body earn no assignment credit (the lambda may never run), consistent
// with the intersection semantics; a lambda stored now and called only after
// the member is assigned is still flagged (accepted imprecision). Capture
// initializers are ordinary CFG elements, already handled by the caller's
// other arms.
static void appendThisCaptureLambdaReadEvents(
    const LambdaExpr *LE,
    const llvm::DenseMap<const FieldDecl *, unsigned> &Index,
    bool StarThisCopyTrusted, SmallVectorImpl<DefAssignEvent> &BlockEvents) {
  auto GetThisCaptureKind =
      [](const LambdaExpr *L) -> std::optional<LambdaCaptureKind> {
    for (const LambdaCapture &C : L->captures())
      if (C.capturesThis())
        return C.getCaptureKind();
    return std::nullopt;
  };
  auto AppendWholeObjectReads = [&](const LambdaExpr *At) {
    // Index's values are exactly 0..size()-1 (collectTrackedUninitMembers).
    for (unsigned I = 0, N = Index.size(); I != N; ++I)
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
  SmallVector<const Stmt *, 16> Stack(1, LE->getBody());
  while (!Stack.empty()) {
    const Stmt *Cur = Stack.pop_back_val();
    if (!Cur)
      continue;
    if (const auto *NestedLE = dyn_cast<LambdaExpr>(Cur)) {
      if (GetThisCaptureKind(NestedLE) == LCK_StarThis) {
        if (!StarThisCopyTrusted)
          AppendWholeObjectReads(NestedLE);
        continue;
      }
    }
    const FieldDecl *F = nullptr;
    if (const auto *BodyICE = dyn_cast<ImplicitCastExpr>(Cur);
        BodyICE && BodyICE->getCastKind() == CK_LValueToRValue)
      F = getCurrentObjectMember(BodyICE->getSubExpr());
    else if (const auto *BodyBO = dyn_cast<BinaryOperator>(Cur);
             BodyBO && BodyBO->isCompoundAssignmentOp())
      F = getCurrentObjectMember(BodyBO->getLHS());
    else if (const auto *BodyUO = dyn_cast<UnaryOperator>(Cur);
             BodyUO && BodyUO->isIncrementDecrementOp())
      F = getCurrentObjectMember(BodyUO->getSubExpr());
    if (F) {
      auto It = Index.find(F);
      if (It != Index.end())
        BlockEvents.push_back(
            {DefAssignEventKind::Read, It->second, cast<Expr>(Cur)});
    }
    for (const Stmt *Child : Cur->children())
      Stack.push_back(Child);
  }
}

// std::init constructor-body check (paper §7.1 "initialized ... before use").
//
// A [[uninit]] scalar data member is deliberately *not* required to be
// initialized by the constructor (paper §5.1 excepts members with an
// uninitialized indicator; §5.3 leaves them for users). What is required is
// that such a member is not *read* before it is given a value (§4.5: reading an
// uninitialized object, except std::byte, is erroneous). This is the member
// analog of the R1 rule for [[uninit]] locals.
//
// A forward definite-assignment dataflow over the constructor body: a scalar
// member is "assigned" by a plain `m = e` (for a built-in type a write is its
// initialization, §4.5) and a member is definitely assigned at a point only if
// assigned on every path reaching it (§1.3: all branches are considered
// executed). A value read of a member that is not definitely assigned there is
// the violation. There is no constructor-exit requirement: a member that is
// simply never read is left as-is, exactly as the structural ctor_uninit_member
// check (R5) excuses a marked member.
//
// Crediting is strict for plain escapes: nothing but a whole-member store
// (or a written member/base initializer) marks a member assigned. Taking the
// member's address, binding a reference to it, calling a member function,
// letting `this` escape, or passing &m to a [[ref_to_uninit]] parameter of
// an ordinary function earns no credit -- the paper rejects complex
// constructor code (§5.1) and reserves callee-initialization for now_init
// (§6.2), so such code is deliberately rejected here (the remedy is
// [[profiles::suppress]]). The one sanctioned exception is exactly §6.2's: a
// call to a [[now_init]] function credits the current-object storage bound
// to its [[ref_to_uninit]] parameters (see the CallExpr arm below), as a
// real Gen bit -- so §1.2's all-branches rule still governs a call under a
// branch. This is the deliberate counterpart of
// checkInitProfileLocalMembers' "soundness over completeness" escape
// crediting below: locals conservatively credit any escape (missed
// diagnostics only, subsuming [[now_init]] callees), while constructor
// bodies get the paper's strictness.
static void checkInitProfileCtorBody(Sema &S, const CXXConstructorDecl *Ctor,
                                     AnalysisDeclContext &AC,
                                     const CFGProfileEntry &Entry) {
  // A delegating constructor leaves member initialization to its target (paper
  // §5.1 trusts the constructor that runs first), so by the time the delegating
  // body runs the members are already initialized; analyzing its body would
  // falsely flag a read. This mirrors how ctor_uninit_member (R5) skips them.
  if (Ctor->isDelegatingConstructor())
    return;

  // A union's members are mutually exclusive: writing one gives the union
  // its value, so per-member assigned bits mismodel variant exclusivity
  // (paper §5.6 -- delayed union-member initialization is banned, and
  // whether the active member is set is deferred), exactly as the
  // ctor_uninit_member finalization callback exempts a union's own
  // constructor. Reachable only when the union_marker rejections are
  // suppressed (the [[uninit]] markers stay in the AST either way).
  if (Ctor->getParent()->isUnion())
    return;

  CFG *cfg = AC.getCFG();
  if (!cfg)
    return;

  // Target members: the shared flow-trackable filter (a base with a
  // user-provided constructor is trusted per paper §5.1; nothing can have
  // assigned a constructor-less base's members before this body runs).
  SmallVector<const FieldDecl *, 4> Members;
  llvm::DenseMap<const FieldDecl *, unsigned> Index;
  collectTrackedUninitMembers(S, Ctor->getParent(), Members, Index);
  if (Members.empty())
    return;
  const unsigned N = Members.size();

  // A `[*this]` capture copy-constructs the whole object; a user-provided
  // copy constructor is opaque and trusted (paper §5.1's
  // trust-the-constructor principle, like this pass's other trust gates),
  // so the capture's whole-object reads are appended only without one.
  const bool StarThisCopyTrusted =
      llvm::any_of(Ctor->getParent()->ctors(), [](const CXXConstructorDecl *C) {
        return C->isCopyConstructor() && C->isUserProvided();
      });

  // Statements the pass may see: the constructor body plus each *written*
  // member/base initializer expression (the CFG is built with
  // AddInitializers=true, so those run as CFG elements in execution order --
  // member-initializer reads such as `X() : o(m) {}` are checked exactly like
  // body reads). A written initializer's own member becomes assigned at its
  // CFGInitializer element below, so declaration order decides what an
  // initializer may read. Not covered: an NSDMI's subexpressions. They *are*
  // in the CFG (AddCXXDefaultInitExprInCtors expands the CXXDefaultInitExpr
  // an unwritten initializer runs), but this filter deliberately keeps them
  // out, so a read of a tracked member inside another member's default
  // initializer stays undetected; lifting that gap means whitelisting the
  // CXXDefaultInitExpr subtrees here, not changing CFG build options.
  llvm::SmallPtrSet<const Stmt *, 32> BodyStmts;
  {
    SmallVector<const Stmt *, 32> Stack;
    if (const Stmt *Body = Ctor->getBody())
      Stack.push_back(Body);
    for (const CXXCtorInitializer *Init : Ctor->inits())
      if (Init->isWritten())
        Stack.push_back(Init->getInit());
    while (!Stack.empty()) {
      const Stmt *Cur = Stack.pop_back_val();
      if (!Cur || !BodyStmts.insert(Cur).second)
        continue;
      for (const Stmt *Child : Cur->children())
        Stack.push_back(Child);
    }
  }

  // Event extraction. Every statement class matched below must be CFG-shape
  // stable: see the linearized-CFG invariant at
  // addNonLinearizedAlwaysAddClasses.
  const unsigned NumBlocks = cfg->getNumBlockIDs();
  std::vector<SmallVector<DefAssignEvent, 4>> Events(NumBlocks);
  for (const CFGBlock *B : *cfg) {
    auto &BlockEvents = Events[B->getBlockID()];
    for (const CFGElement &Elem : *B) {
      // A written member initializer assigns its member at this point in
      // execution order, after its init expression's events above it. (The
      // former entry-state seeding could not order the initializers' own
      // reads against these writes.)
      if (auto OptInit = Elem.getAs<CFGInitializer>()) {
        const CXXCtorInitializer *CI = OptInit->getInitializer();
        if (!CI->isWritten())
          continue;
        if (CI->isAnyMemberInitializer()) {
          const FieldDecl *F = CI->getAnyMember();
          if (!F)
            continue;
          auto It = Index.find(F);
          if (It == Index.end())
            continue;
          BlockEvents.push_back(
              {DefAssignEventKind::Write, It->second, CI->getInit()});
        } else if (CI->isBaseInitializer()) {
          // A written base initializer (e.g. `: Base{1}`) gives the tracked
          // members of that constructor-less base subtree their values.
          const auto *BRD = CI->getBaseClass()->getAsCXXRecordDecl();
          if (!BRD || !BRD->hasDefinition())
            continue;
          forEachCandidateUninitField(
              BRD->getDefinition(), [&](const FieldDecl *F) {
                auto It = Index.find(F);
                if (It == Index.end())
                  return;
                BlockEvents.push_back(
                    {DefAssignEventKind::Write, It->second, CI->getInit()});
              });
        }
        continue;
      }
      auto OptStmt = Elem.getAs<CFGStmt>();
      if (!OptStmt)
        continue;
      const Stmt *St = OptStmt->getStmt();
      if (!BodyStmts.count(St))
        continue;
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(St)) {
        if (ICE->getCastKind() != CK_LValueToRValue)
          continue;
        const FieldDecl *F = getCurrentObjectMember(ICE->getSubExpr());
        if (!F)
          continue;
        auto It = Index.find(F);
        if (It != Index.end())
          BlockEvents.push_back({DefAssignEventKind::Read, It->second, ICE});
      } else if (const auto *BO = dyn_cast<BinaryOperator>(St)) {
        if (!BO->isAssignmentOp())
          continue;
        const FieldDecl *F = getCurrentObjectMember(BO->getLHS());
        if (!F)
          continue;
        auto It = Index.find(F);
        if (It == Index.end())
          continue;
        BlockEvents.push_back({BO->isCompoundAssignmentOp()
                                   ? DefAssignEventKind::ReadWrite
                                   : DefAssignEventKind::Write,
                               It->second, BO});
      } else if (const auto *UO = dyn_cast<UnaryOperator>(St)) {
        // A built-in ++m / m++ / --m / m-- reads the old value and then writes,
        // but unlike -m / !m it carries no lvalue-to-rvalue cast, so the Read
        // arm above never sees it. Model it like the compound-assignment case:
        // a ReadWrite that also marks the member assigned.
        if (!UO->isIncrementDecrementOp())
          continue;
        const FieldDecl *F = getCurrentObjectMember(UO->getSubExpr());
        if (!F)
          continue;
        auto It = Index.find(F);
        if (It == Index.end())
          continue;
        BlockEvents.push_back({DefAssignEventKind::ReadWrite, It->second, UO});
      } else if (const auto *CE = dyn_cast<CallExpr>(St)) {
        appendLifecycleCallEvents(CE, N, Index, BlockEvents);
      } else if (const auto *LE = dyn_cast<LambdaExpr>(St)) {
        appendThisCaptureLambdaReadEvents(LE, Index, StarThisCopyTrusted,
                                          BlockEvents);
      }
    }
  }

  // Nothing is assigned at function entry: written initializers generate
  // their writes at their CFGInitializer elements.
  std::vector<SmallVector<const Expr *, 2>> Offending =
      runDefiniteAssignment(*cfg, AC, N, Events);
  reportMemberReadsBeforeInit(S, AC, Offending, Members, Entry.Name);
}

// If E (stripped of the transparent casts the parse-order credit sees
// through -- parens, implicit casts including the derived-to-base cast of an
// inherited-member access, and explicit glvalue/pointer casts like
// `(int &)V.m`, which denote the same storage, §4.3) is a member access
// `V.m` on a directly named variable, return the base DeclRefExpr and the
// field through \p F. The peel applies at the member and base positions
// alike, so a cast store credits exactly the accessed member (not a
// whole-object escape) and a cast read is detected. Anonymous-struct/union
// steps are peeled: `x.a` on an anonymous-aggregate member is
// MemberExpr(MemberExpr(x, <anon>), a), and reaching `a` can no more
// initialize x's other members than reaching a named sibling can, so the
// leaf flows into the caller's classification unchanged (no tracked member
// can live inside an anonymous record -- the harvest drops nameless and
// non-scalar fields -- so a scalar leaf is an untracked sibling and any
// other leaf stays a conservative escape). An arrow access or an access
// through any other expression is not a tracked local's member.
static const DeclRefExpr *getLocalMemberAccess(const Expr *E,
                                               const FieldDecl *&F) {
  const auto *ME =
      dyn_cast<MemberExpr>(SemaProfiles::ignoreTransparentCasts(E));
  if (!ME || ME->isArrow())
    return nullptr;
  const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (!FD)
    return nullptr;
  const Expr *Base = SemaProfiles::ignoreTransparentCasts(ME->getBase());
  while (const auto *BME = dyn_cast<MemberExpr>(Base)) {
    const auto *BFD = dyn_cast<FieldDecl>(BME->getMemberDecl());
    if (BME->isArrow() || !BFD || !BFD->isAnonymousStructOrUnion())
      break;
    Base = SemaProfiles::ignoreTransparentCasts(BME->getBase());
  }
  const auto *DRE = dyn_cast<DeclRefExpr>(Base);
  if (!DRE)
    return nullptr;
  F = FD;
  return DRE;
}

// The type-shape half of the tracked guards: a non-union, non-dependent
// class with no user-provided constructor anywhere in the contributing
// subtree (paper §5.1 trusts one: its body may assign, which local analysis
// cannot see). A reference type aliases an object also reachable other ways
// and never qualifies.
static const CXXRecordDecl *getTrackableSlotClass(QualType T) {
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

// The declaration-shape half for a local: V is a non-parameter local, not
// itself [[uninit]]-marked (its subobject accesses are the parse-time
// read-through / uninit_write rules' territory, and tracking it here would
// double-diagnose), of a trackable class. How V's declaration *initializes*
// the members is the callers' half.
static const CXXRecordDecl *getTrackableLocalClass(const VarDecl *V) {
  if (!V->hasLocalStorage() || isa<ParmVarDecl>(V) || isa<DecompositionDecl>(V))
    return nullptr;
  if (V->isInvalidDecl() || V->hasAttr<UninitAttr>())
    return nullptr;
  return getTrackableSlotClass(V->getType());
}

// If V is a local whose [[uninit]] members this pass may soundly flow-track
// from an all-unassigned start, return its class definition; null otherwise.
// Sound means nothing can have assigned the members before V's declaration:
// the class shape qualifies (above) and the declaration ran nothing but the
// implicit no-op default-construction.
static const CXXRecordDecl *getTrackedLocalAggregate(const VarDecl *V) {
  const CXXRecordDecl *RD = getTrackableLocalClass(V);
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
  // the copy harvest in checkInitProfileLocalMembers; for an arbitrary
  // untracked source that state is unknowable, so those copies stay
  // untracked -- a missed diagnostic, never a false positive.
  if (!SemaProfiles::isDefaultInitShape(V->getInit()))
    return nullptr;
  return RD;
}

// If V is copy- or move-constructed from a directly named variable, return
// that source's DeclRefExpr; null otherwise. The explicit-cast peel resolves
// the move form (`Agg b = static_cast<Agg&&>(a);`) to its named operand,
// mirroring the parse-time recognizers' pass-through.
static const DeclRefExpr *getLocalCopySourceRef(const VarDecl *V) {
  const Expr *Init = V->getInit();
  if (!Init)
    return nullptr;
  const auto *CCE = dyn_cast<CXXConstructExpr>(Init->IgnoreImplicit());
  if (!CCE || CCE->getNumArgs() < 1 ||
      !CCE->getConstructor()->isCopyOrMoveConstructor())
    return nullptr;
  const Expr *Arg = CCE->getArg(0)->IgnoreParenImpCasts();
  while (const auto *CE = dyn_cast<ExplicitCastExpr>(Arg)) {
    if (!CE->getSubExpr()->isGLValue())
      break;
    Arg = CE->getSubExpr()->IgnoreParenImpCasts();
  }
  return dyn_cast<DeclRefExpr>(Arg);
}

// std::init local-aggregate member check (paper §7.1 "initialized ... before
// use", the local-variable analog of the ctor-body pass above).
//
// An [[uninit]] scalar member of a constructor-less aggregate local is given
// a value by a plain member store (`a.m = e`; for a built-in type a write is
// its initialization, §4.5) -- the §5.3 "class exposing uninitialized
// members" pattern. A read of such a member before it is definitely assigned
// (on every path, §1.3) is the violation. This is the flow tracking the
// parse-time read-through rule's top-level drop relies on for locals: the
// drop trusts a direct member read so the legal write-then-read sequence is
// not rejected, and this pass supplies the missing read-before-write
// diagnosis.
//
// Soundness over completeness: any appearance of the variable outside a
// recognized member read or write -- &a, &a.m, a reference binding, passing a
// to any function (construct_at, memcpy), a member call, a lambda capture --
// conservatively marks every member assigned from that point (the address may
// be used to initialize the object). This subsumes [[now_init]] callees
// (§6.2): passing &a.m to one is an escape like any other, so no dedicated
// call arm is needed here, unlike the strict ctor-body pass above. Members of
// an object with a user-provided constructor stay untracked (trusted, §5.1), as
// do objects reached through parameters, references, or other objects. A
// backward goto across the declaration re-default-initializes the object, which
// the gen-only dataflow cannot model -- a possible missed diagnostic, matching
// the ctor-body pass's accepted imprecision level.
static void checkInitProfileLocalMembers(Sema &S, AnalysisDeclContext &AC,
                                         const CFGProfileEntry &Entry) {
  CFG *cfg = AC.getCFG();
  if (!cfg)
    return;

  // Harvest tracked (local, member) pairs from the CFG's (single-decl)
  // DeclStmt elements; each variable's pairs are contiguous so an escape can
  // set a range.
  SmallVector<const FieldDecl *, 4> PairField;
  llvm::DenseMap<const FieldDecl *, unsigned> FieldIdxScratch;
  llvm::DenseMap<const VarDecl *, std::pair<unsigned, unsigned>> VarRange;
  llvm::DenseMap<std::pair<const VarDecl *, const FieldDecl *>, unsigned>
      PairIdx;
  auto HarvestVar = [&](const VarDecl *V, const CXXRecordDecl *RD) {
    SmallVector<const FieldDecl *, 4> Members;
    FieldIdxScratch.clear();
    collectTrackedUninitMembers(S, RD, Members, FieldIdxScratch);
    if (Members.empty())
      return false;
    unsigned Begin = PairField.size();
    for (const FieldDecl *F : Members) {
      PairIdx[{V, F}] = PairField.size();
      PairField.push_back(F);
    }
    VarRange[V] = {Begin, PairField.size()};
    return true;
  };

  // A by-value slot parameter is tracked from an all-unassigned start
  // (which is exactly the dataflow's entry state): the parameter is a
  // *copy* of the caller's argument, and a copy does not inherit
  // initialization (paper §5.2) -- §4.4's Slot contract makes a marked
  // member uninitialized until locally proven otherwise ("you can't ask a
  // slot if it is initialized"), and the paper's way to hand
  // uninitialized-capable storage across a call is a marked pointer or
  // reference (§4.3), not a by-value slot. This is the call-boundary twin
  // of the ctor-body pass's deliberate strictness: a caller that assigned
  // the member before the call is rejected here all the same, with escape
  // crediting (any bare use of the parameter) and [[profiles::suppress]]
  // as the remedies. Reference parameters alias the caller's own object
  // and stay untracked.
  if (const auto *EnclosingFD = dyn_cast_or_null<FunctionDecl>(AC.getDecl()))
    for (const ParmVarDecl *P : EnclosingFD->parameters()) {
      if (P->isInvalidDecl() || P->hasAttr<UninitAttr>())
        continue;
      // A parameter with a default argument is not tracked: a defaulted
      // call initializes the parameter object directly (guaranteed elision
      // -- no copy), falsifying the tracked-from-unassigned premise. The
      // shape of the default is irrelevant (no default-argument spelling is
      // a vacuous default-init), so the mere presence gates -- also
      // avoiding getDefaultArg() on unparsed/uninstantiated defaults. An
      // explicit call passing an uninitialized copy into a defaulted
      // parameter is a missed diagnostic (see the Limitations doc).
      if (P->hasDefaultArg())
        continue;
      if (const CXXRecordDecl *RD = getTrackableSlotClass(P->getType()))
        HarvestVar(P, RD);
    }

  for (const CFGBlock *B : *cfg) {
    for (const CFGElement &Elem : *B) {
      auto CS = Elem.getAs<CFGStmt>();
      if (!CS)
        continue;
      const auto *DS = dyn_cast<DeclStmt>(CS->getStmt());
      if (!DS)
        continue;
      for (const Decl *Dcl : DS->decls()) {
        const auto *V = dyn_cast<VarDecl>(Dcl);
        if (!V || VarRange.count(V))
          continue;
        const CXXRecordDecl *RD = getTrackedLocalAggregate(V);
        if (!RD)
          continue;
        HarvestVar(V, RD);
      }
    }
  }

  // Nothing tracked so far means nothing can become tracked: the copy
  // harvest below only chains from an already-tracked source (it requires
  // VarRange.count(Src)), so bail out before its repeated whole-CFG walks
  // -- the common case for a function with no tracked aggregates.
  if (PairField.empty())
    return;

  // Second harvest: locals copy- or move-constructed from a *tracked* local.
  // The copy's members inherit the source's per-member state at the copy
  // point -- a copy does not inherit initialization (paper §5.2), it
  // inherits whatever state the source has -- modeled by a per-member Copy
  // transfer event at the DeclStmt. Keyed per source *field* (not position:
  // a copy sliced from a tracked derived object shares its base's
  // FieldDecls). Iterated to a fixpoint so a copy of a copy resolves
  // regardless of CFG block order.
  llvm::DenseMap<const VarDecl *, const VarDecl *> CopySource;
  for (bool Added = true; Added;) {
    Added = false;
    for (const CFGBlock *B : *cfg) {
      for (const CFGElement &Elem : *B) {
        auto CS = Elem.getAs<CFGStmt>();
        if (!CS)
          continue;
        const auto *DS = dyn_cast<DeclStmt>(CS->getStmt());
        if (!DS)
          continue;
        for (const Decl *Dcl : DS->decls()) {
          const auto *V = dyn_cast<VarDecl>(Dcl);
          if (!V || VarRange.count(V))
            continue;
          const CXXRecordDecl *RD = getTrackableLocalClass(V);
          if (!RD)
            continue;
          const DeclRefExpr *SrcRef = getLocalCopySourceRef(V);
          const auto *Src =
              SrcRef ? dyn_cast<VarDecl>(SrcRef->getDecl()) : nullptr;
          if (!Src || !VarRange.count(Src))
            continue;
          if (!HarvestVar(V, RD))
            continue;
          CopySource[V] = Src;
          Added = true;
        }
      }
    }
  }

  const unsigned N = PairField.size();

  // A `V.m` access on a tracked local, classified into one of three named
  // states. NotMemberAccess: E is not such an access at all (also the
  // conservative answer for member shapes that must stay escapes, below).
  // TrackedMember: m is one of the tracked members; Idx is its pair index
  // and Base the consumed base reference. UntrackedSibling: m is an
  // untracked sibling of a shape that cannot reach another member, so the
  // base is consumed (benign) rather than escaping -- the escape arm below
  // would otherwise credit every tracked member of V and lose the
  // diagnostic for them.
  struct TrackedAccess {
    enum Kind { NotMemberAccess, TrackedMember, UntrackedSibling };
    Kind K = NotMemberAccess;
    unsigned Idx = ~0u;                // TrackedMember only.
    const DeclRefExpr *Base = nullptr; // Null iff NotMemberAccess.
  };
  auto LookupPair = [&](const Expr *E) -> TrackedAccess {
    const FieldDecl *F = nullptr;
    const DeclRefExpr *DRE = getLocalMemberAccess(E, F);
    if (!DRE)
      return {};
    const auto *V = dyn_cast<VarDecl>(DRE->getDecl());
    if (!V || !VarRange.count(V))
      return {};
    auto It = PairIdx.find({V, F});
    if (It != PairIdx.end())
      return {TrackedAccess::TrackedMember, It->second, DRE};
    // An untracked sibling. Reaching it consumes the base without exposing
    // the object -- but only for a member that cannot itself reach one: a
    // pointer or reference member may denote a tracked member (a
    // [[ref_to_uninit]] member aimed at one by a default member initializer
    // needs no `&V.m` in the body, so nothing else escapes the object), and a
    // class or array member can hold such a value. Restricting the benign
    // shapes to the scalars this pass tracks keeps the escape crediting
    // conservative, which for locals must stay a missed diagnostic rather
    // than become a false positive.
    QualType FT = F->getType();
    if (!FT->isIntegralOrEnumerationType() && !FT->isFloatingType())
      return {};
    return {TrackedAccess::UntrackedSibling, ~0u, DRE};
  };

  // First pass: the base DeclRefExprs consumed by a recognized member read or
  // write are benign -- they do not escape the object. A DeclRefExpr element
  // precedes its consuming cast/operator element in a block, so the benign
  // set must be complete before elements are classified.
  llvm::SmallPtrSet<const DeclRefExpr *, 16> Benign;
  for (const CFGBlock *B : *cfg) {
    for (const CFGElement &Elem : *B) {
      auto CS = Elem.getAs<CFGStmt>();
      if (!CS)
        continue;
      const Stmt *St = CS->getStmt();
      const DeclRefExpr *Base = nullptr;
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(St)) {
        if (ICE->getCastKind() == CK_LValueToRValue)
          Base = LookupPair(ICE->getSubExpr()).Base;
      } else if (const auto *BO = dyn_cast<BinaryOperator>(St)) {
        if (BO->isAssignmentOp())
          Base = LookupPair(BO->getLHS()).Base;
      } else if (const auto *UO = dyn_cast<UnaryOperator>(St)) {
        if (UO->isIncrementDecrementOp())
          Base = LookupPair(UO->getSubExpr()).Base;
      } else if (const auto *DS = dyn_cast<DeclStmt>(St)) {
        // The source ref of a tracked copy is consumed by the copy's
        // implicit (retention-free) constructor and modeled by the Copy
        // event, so it is not an escape -- an escape here would wrongly
        // mark the *source* fully assigned.
        for (const Decl *Dcl : DS->decls())
          if (const auto *V = dyn_cast<VarDecl>(Dcl))
            if (CopySource.count(V))
              Base = getLocalCopySourceRef(V);
      }
      if (Base)
        Benign.insert(Base);
    }
  }

  // Second pass: per-block ordered events. A load of `V.m` is a Read; a
  // member store is a Write (a compound assignment or ++/-- reads the old
  // value first); a tracked copy's DeclStmt is a per-member Copy, whose
  // dest-member state becomes the source member's at that point; any
  // non-benign DeclRefExpr naming a tracked local is an escape, modeled as
  // a Write of every one of its tracked members. Every statement class
  // matched here must be CFG-shape stable: see the linearized-CFG invariant
  // at addNonLinearizedAlwaysAddClasses.
  const unsigned NumBlocks = cfg->getNumBlockIDs();
  std::vector<SmallVector<DefAssignEvent, 4>> Events(NumBlocks);
  for (const CFGBlock *B : *cfg) {
    auto &BlockEvents = Events[B->getBlockID()];
    for (const CFGElement &Elem : *B) {
      auto CS = Elem.getAs<CFGStmt>();
      if (!CS)
        continue;
      const Stmt *St = CS->getStmt();
      if (const auto *ICE = dyn_cast<ImplicitCastExpr>(St)) {
        if (ICE->getCastKind() != CK_LValueToRValue)
          continue;
        TrackedAccess TA = LookupPair(ICE->getSubExpr());
        if (TA.K == TrackedAccess::TrackedMember)
          BlockEvents.push_back({DefAssignEventKind::Read, TA.Idx, ICE});
      } else if (const auto *BO = dyn_cast<BinaryOperator>(St)) {
        if (!BO->isAssignmentOp())
          continue;
        TrackedAccess TA = LookupPair(BO->getLHS());
        if (TA.K != TrackedAccess::TrackedMember)
          continue;
        BlockEvents.push_back({BO->isCompoundAssignmentOp()
                                   ? DefAssignEventKind::ReadWrite
                                   : DefAssignEventKind::Write,
                               TA.Idx, BO});
      } else if (const auto *UO = dyn_cast<UnaryOperator>(St)) {
        if (!UO->isIncrementDecrementOp())
          continue;
        TrackedAccess TA = LookupPair(UO->getSubExpr());
        if (TA.K != TrackedAccess::TrackedMember)
          continue;
        BlockEvents.push_back({DefAssignEventKind::ReadWrite, TA.Idx, UO});
      } else if (const auto *DRE = dyn_cast<DeclRefExpr>(St)) {
        if (Benign.count(DRE))
          continue;
        const auto *V = dyn_cast<VarDecl>(DRE->getDecl());
        if (!V)
          continue;
        auto It = VarRange.find(V);
        if (It == VarRange.end())
          continue;
        for (unsigned Idx = It->second.first; Idx != It->second.second; ++Idx)
          BlockEvents.push_back({DefAssignEventKind::Write, Idx, DRE});
      } else if (const auto *DS = dyn_cast<DeclStmt>(St)) {
        // A tracked copy: each dest member's state becomes its source
        // member's, keyed per FieldDecl (a sliced copy shares the base's
        // FieldDecls with a differently laid out source range). The
        // DeclStmt element follows its initializer's subexpression
        // elements, so the transfer sees the source's state at the copy
        // point. A source field the source range does not track cannot
        // occur (the dest's fields are a subset of the source's); fall
        // back to Write (assume assigned) if it somehow does.
        for (const Decl *Dcl : DS->decls()) {
          const auto *V = dyn_cast<VarDecl>(Dcl);
          if (!V)
            continue;
          auto CopyIt = CopySource.find(V);
          if (CopyIt == CopySource.end())
            continue;
          auto Range = VarRange.find(V)->second;
          for (unsigned Idx = Range.first; Idx != Range.second; ++Idx) {
            auto SrcIt = PairIdx.find({CopyIt->second, PairField[Idx]});
            if (SrcIt != PairIdx.end())
              BlockEvents.push_back(
                  {DefAssignEventKind::Copy, Idx, V->getInit(), SrcIt->second});
            else
              BlockEvents.push_back(
                  {DefAssignEventKind::Write, Idx, V->getInit()});
          }
        }
      } else if (const auto *CE = dyn_cast<CallExpr>(St)) {
        // A [[now_uninit]] callee destroys the storage bound to each of its
        // pointer/reference parameters: a tracked member passed as `&x.m` /
        // `x.m` is killed, and a whole tracked object passed as `&x` / `x`
        // has every tracked member killed. The argument's base DeclRefExpr
        // stays a non-benign escape whose whole-range Write precedes this
        // call element in block order (a DeclRefExpr element precedes its
        // consuming call element), so under the escape leniency the
        // siblings keep the escape credit while the destroyed member nets
        // to killed. Storage-release callees (free) are deliberately not
        // kills: a release ends no object's lifetime, and the allocator
        // trust split stays out of this file. (CXXOperatorCallExpr is-a
        // CallExpr: keep this the chain's only CallExpr arm.)
        const FunctionDecl *Callee = CE->getDirectCallee();
        if (!Callee || !Callee->hasAttr<NowUninitAttr>())
          continue;
        unsigned ArgOffset = 0;
        if (isa<CXXOperatorCallExpr>(CE))
          if (const auto *MD = dyn_cast<CXXMethodDecl>(Callee);
              MD && !MD->isExplicitObjectMemberFunction())
            ArgOffset = 1;
        for (unsigned PI = 0, NP = Callee->getNumParams(); PI != NP; ++PI) {
          if (PI + ArgOffset >= CE->getNumArgs())
            break;
          QualType PT = Callee->getParamDecl(PI)->getType();
          if (!PT->isPointerType() && !PT->isReferenceType())
            continue;
          const Expr *G = nullptr;
          peelLifecycleArgument(CE->getArg(PI + ArgOffset), G);
          TrackedAccess TA = LookupPair(G);
          if (TA.K == TrackedAccess::TrackedMember) {
            BlockEvents.push_back({DefAssignEventKind::Kill, TA.Idx, CE});
            continue;
          }
          const auto *DRE = dyn_cast<DeclRefExpr>(G->IgnoreParenImpCasts());
          if (!DRE)
            continue;
          const auto *V = dyn_cast<VarDecl>(DRE->getDecl());
          if (!V)
            continue;
          auto It = VarRange.find(V);
          if (It == VarRange.end())
            continue;
          for (unsigned Idx = It->second.first; Idx != It->second.second; ++Idx)
            BlockEvents.push_back({DefAssignEventKind::Kill, Idx, CE});
        }
      }
    }
  }

  // Nothing is assigned at function entry: a tracked local cannot be
  // referenced before its DeclStmt anyway, and a tracked by-value slot
  // parameter starts all-unassigned by design (above).
  std::vector<SmallVector<const Expr *, 2>> Offending =
      runDefiniteAssignment(*cfg, AC, N, Events);
  reportMemberReadsBeforeInit(S, AC, Offending, PairField, Entry.Name);
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

bool sema::AnalysisBasedWarnings::hasEnforcedCFGProfile() const {
  return S.Profiles().anyProfileEnforced(CFGProfiles);
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

/// Apply the ConfigureCFG hooks of the enforced CFGProfiles rows to \p
/// Options; both analysis paths call it so they build the same CFG shape.
static void configureProfileCFGOptions(Sema &S, CFG::BuildOptions &Options) {
  for (const CFGProfileEntry &E : CFGProfiles)
    if (E.ConfigureCFG && S.Profiles().isProfileEnforced(E.Name))
      E.ConfigureCFG(Options);
}

/// Run the ExtraPass hooks of the enforced CFGProfiles rows; both analysis
/// paths call it after the uninitialized-variables reporter has flushed.
static void runProfileExtraCFGPasses(Sema &S, const Decl *D,
                                     AnalysisDeclContext &AC) {
  for (const CFGProfileEntry &E : CFGProfiles)
    if (E.ExtraPass && S.Profiles().isProfileEnforced(E.Name))
      E.ExtraPass(S, D, AC, E);
}

/// Pattern-2 profiles (the CFGProfiles table) ride the uninitialized-
/// variables analysis, but profile rules emit errors, so they must run even
/// where IssueWarnings skips the warning pipeline: once the TU has an
/// uncompilable error, and when warnings are disabled for the declaration
/// (-w, or a system-header decl under -fno-profiles-exempt-system-headers).
/// Run just the profile-relevant analyses for a single function on those
/// paths. Diagnostics are restricted to the profiles via the ProfileOnly
/// reporter and the hooks' own gates.
static void runProfileOnlyCFGAnalysis(Sema &S, const Decl *D) {
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
    if (!cast<DeclContext>(D)->isDependentContext() &&
        hasEnforcedCFGProfile() &&
        !S.Context.isProfileExemptSystemHeaderLoc(D->getLocation()) &&
        !D->isInvalidDecl() && !Diags.hasFatalErrorOccurred())
      runProfileOnlyCFGAnalysis(S, D);
    return;
  }

  // Cached for the uses below (the enforced set cannot change within one
  // invocation); the skip path above queries hasEnforcedCFGProfile directly.
  const bool CFGProfileEnforced = hasEnforcedCFGProfile();

  if (S.hasUncompilableErrorOccurred()) {
    // Flush out any possibly unreachable diagnostics.
    flushDiagnostics(S, fscope);
    // Profile rules are errors: keep the CFG profile pass alive after a TU
    // error, on a valid decl so the CFG is buildable (see
    // runProfileOnlyCFGAnalysis). Other analyses keep the early-out.
    if (CFGProfileEnforced && !D->isInvalidDecl() &&
        !Diags.hasFatalErrorOccurred())
      runProfileOnlyCFGAnalysis(S, D);
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
    if (CFGProfileEnforced)
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

  if (CFGProfileEnforced ||
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

  if (CFGProfileEnforced)
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
