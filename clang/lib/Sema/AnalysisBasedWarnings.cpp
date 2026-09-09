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

/// One flow-tracked std::init storage entity of the analyzed body: an
/// [[uninit]] scalar member of the current object or of a directly named
/// local or by-value parameter, an [[uninit]] local or parameter as a whole,
/// or the referent of a [[ref_to_uninit]] local pointer or reference
/// (parameters included).
struct TrackedEntity {
  enum class Kind { CurrentObjectMember, LocalMember, WholeLocal, Pointee };
  Kind K;
  /// The local (LocalMember: the object; WholeLocal: the entity; Pointee:
  /// the pointer or reference); null for the current object.
  const VarDecl *Base;
  /// The derived-to-base path from the object's class to Field's class
  /// (member kinds).
  BasePath Path;
  /// The member (member kinds); null otherwise.
  const FieldDecl *Field = nullptr;
  /// Read events are extracted for this entity (a constructor body's or a
  /// default-initialized constructor-less local's members).
  bool TrackReads = true;
  /// The entity belongs to an enclosing function (a variable a lambda body
  /// reaches by capture): its entry state is unknown -- assigned on some
  /// path, on no path definitely -- rather than unassigned.
  bool UnknownEntry = false;
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

/// The entity table of one analyzed body and the lvalue-shape resolvers
/// over it. Each object's entities are contiguous, so a whole-object event
/// covers a range.
struct TrackedStorage {
  SmallVector<TrackedEntity, 8> Entities;
  /// The contiguous entity range of each tracked local's members.
  llvm::DenseMap<const VarDecl *, std::pair<unsigned, unsigned>> LocalRange;
  /// The WholeLocal or Pointee entity of a marked local or parameter.
  llvm::DenseMap<const VarDecl *, unsigned> LocalEntity;
  /// The current object's range (an instance member function body); empty
  /// otherwise.
  std::pair<unsigned, unsigned> CurrentObjectRange{0, 0};
  /// The tracked local a tracked copy was copy- or move-constructed from,
  /// and the derived-to-base path of the conversion a slicing copy applied
  /// to it (empty for a same-class copy).
  struct CopyOrigin {
    const VarDecl *Src = nullptr;
    BasePath Path;
  };
  llvm::DenseMap<const VarDecl *, CopyOrigin> CopySource;

  /// Append one entity per flow-tracked member of \p RD
  /// (SemaProfiles::isFlowTrackedMemberField, including the members of its
  /// non-virtual, constructor-less bases, which nothing can have assigned
  /// before the containing object's user code runs) for the object \p Base
  /// (null: the current object), with the given read-tracking and entry
  /// state. Class-type and array members (which would need construct_at
  /// flow modeling) and pointers (rejected under [[uninit]]) are out of
  /// scope. Returns the appended range; a local's range is recorded only
  /// when non-empty.
  std::pair<unsigned, unsigned> addObject(Sema &S, const VarDecl *Base,
                                          const CXXRecordDecl *RD,
                                          bool TrackReads, bool UnknownEntry);

  /// Append the WholeLocal or Pointee entity of the marked local or
  /// parameter \p V (once); returns its index.
  unsigned addLocalEntity(TrackedEntity::Kind K, const VarDecl *V,
                          bool UnknownEntry);

  /// The entity of member \p F reached through \p Path in the object
  /// \p Base (null: the current object), if tracked.
  std::optional<unsigned> find(const VarDecl *Base,
                               ArrayRef<const CXXRecordDecl *> Path,
                               const FieldDecl *F) const;

  /// What resolving a glvalue found: the tracked entity it names -- Subobject
  /// when it reaches storage below that entity (a member or element of an
  /// [[uninit]] object, a member of a marked pointee), which a store does not
  /// initialize and a binding judges by the entity's state -- or, for an
  /// access that reaches a tracked aggregate local without naming a tracked
  /// member, the consumed base and whether the access is benign: it cannot
  /// reach a tracked member (an untracked scalar sibling, or a sub-access
  /// below a member whose type holds no pointer, reference, or member
  /// pointer), so the base does not escape.
  struct Resolution {
    std::optional<unsigned> Entity;
    bool Subobject = false;
    const DeclRefExpr *LocalBase = nullptr;
    bool Benign = false;
  };
  /// Resolve the glvalue \p E through SemaProfiles::flowLeafShape: a tracked
  /// member of the current object (this->m, m, (*this).m) or of a directly
  /// named tracked local (V.m), an [[uninit]] local or a subobject of it (u,
  /// u.x, arr[i]), or a marked local pointer's or reference's referent or a
  /// subobject of it (*p, p->m, r, r.m; never an element, P4222R2 §5.4). A
  /// sub-access below a named class or array member of a tracked aggregate
  /// (x.in.v, x.arr[0]) names no entity and is benign or an escape by that
  /// member's type. The one resolver behind every event arm, so a leaf
  /// affects flow state exactly as SemaProfiles::isFlowTrackedLeaf says it
  /// is tracked.
  Resolution resolve(const Expr *E) const;

  /// Resolve a binding or argument leaf: the glvalue \p E (resolve), or --
  /// with \p AsPointerValue, a leaf used as a pointer value -- the storage
  /// `&x` names, a marked local pointer's referent (its value), or a decayed
  /// [[uninit]] array as a whole.
  Resolution resolveFlowLeaf(const Expr *E, bool AsPointerValue) const;

  /// The Pointee entity of the marked local pointer \p V (a reference's
  /// referent cannot be reseated and is not one), if tracked.
  std::optional<unsigned> markedPointerObject(const VarDecl *V) const;
  /// The same for the pointer object the glvalue \p E directly names through
  /// transparent casts: the object a store reseats or an alias binding
  /// escapes.
  std::optional<unsigned> markedPointerObject(const Expr *E) const;

  /// The range of the object \p E names as a whole -- `this` / `*this`, or
  /// a tracked local's DeclRefExpr -- empty otherwise.
  std::pair<unsigned, unsigned> wholeObject(const Expr *E) const;
};

std::pair<unsigned, unsigned>
TrackedStorage::addObject(Sema &S, const VarDecl *Base, const CXXRecordDecl *RD,
                          bool TrackReads, bool UnknownEntry) {
  unsigned Begin = Entities.size();
  SemaProfiles::forEachCandidateUninitField(
      RD, [&](const BasePath &Path, const FieldDecl *F) {
        if (!SemaProfiles::isFlowTrackedMemberField(S.Context, F))
          return;
        for (unsigned I = Begin, N = Entities.size(); I != N; ++I)
          if (Entities[I].Field == F && Entities[I].Path == Path)
            return;
        TrackedEntity E{Base ? TrackedEntity::Kind::LocalMember
                             : TrackedEntity::Kind::CurrentObjectMember,
                        Base, Path, F};
        E.TrackReads = TrackReads;
        E.UnknownEntry = UnknownEntry;
        Entities.push_back(std::move(E));
      });
  std::pair<unsigned, unsigned> Range{Begin, (unsigned)Entities.size()};
  if (!Base)
    CurrentObjectRange = Range;
  else if (Range.first != Range.second)
    LocalRange[Base] = Range;
  return Range;
}

unsigned TrackedStorage::addLocalEntity(TrackedEntity::Kind K, const VarDecl *V,
                                        bool UnknownEntry) {
  auto It = LocalEntity.find(V);
  if (It != LocalEntity.end())
    return It->second;
  TrackedEntity E{K, V, BasePath(), nullptr};
  E.TrackReads = false;
  E.UnknownEntry = UnknownEntry;
  Entities.push_back(std::move(E));
  return LocalEntity[V] = Entities.size() - 1;
}

std::optional<unsigned>
TrackedStorage::find(const VarDecl *Base, ArrayRef<const CXXRecordDecl *> Path,
                     const FieldDecl *F) const {
  std::pair<unsigned, unsigned> Range = CurrentObjectRange;
  if (Base) {
    auto It = LocalRange.find(Base);
    if (It == LocalRange.end())
      return std::nullopt;
    Range = It->second;
  }
  for (unsigned I = Range.first; I != Range.second; ++I)
    if (Entities[I].Field == F && llvm::ArrayRef(Entities[I].Path) == Path)
      return I;
  return std::nullopt;
}

std::pair<unsigned, unsigned> TrackedStorage::wholeObject(const Expr *E) const {
  E = SemaProfiles::ignoreTransparentCasts(E);
  if (isCurrentObjectBase(E))
    return CurrentObjectRange;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    if (const auto *V = dyn_cast<VarDecl>(DRE->getDecl()))
      if (auto It = LocalRange.find(V); It != LocalRange.end())
        return It->second;
  return {0, 0};
}

TrackedStorage::Resolution TrackedStorage::resolve(const Expr *E) const {
  std::optional<SemaProfiles::FlowLeafShape> Shape =
      SemaProfiles::flowLeafShape(E);
  if (!Shape)
    return {};
  Resolution R;
  if (Shape->IsThis) {
    if (Shape->Named.size() != 1 || Shape->Subscript)
      return {};
    R.Entity = find(nullptr, Shape->Path, Shape->Named.front());
    return R;
  }
  const VarDecl *V = Shape->Local;
  // *p, p->m: a marked pointer's referent or a subobject of it.
  if (Shape->ThroughPointer) {
    if (Shape->Subscript)
      return {};
    if (std::optional<unsigned> Idx = markedPointerObject(V)) {
      R.Entity = Idx;
      R.Subobject = !Shape->Named.empty();
    }
    return R;
  }
  if (auto It = LocalEntity.find(V); It != LocalEntity.end()) {
    // u, u.x, arr[i]: an [[uninit]] local as a whole or a subobject of it;
    // r, r.m: a marked reference's referent or a subobject of it.
    const TrackedEntity &Ent = Entities[It->second];
    if (Ent.K == TrackedEntity::Kind::WholeLocal) {
      R.Entity = It->second;
      R.Subobject = !Shape->Named.empty() || Shape->Subscript;
    } else if (Ent.K == TrackedEntity::Kind::Pointee &&
               V->getType()->isReferenceType() && !Shape->Subscript) {
      R.Entity = It->second;
      R.Subobject = !Shape->Named.empty();
    }
    return R;
  }
  // V.m: a member of a tracked aggregate local, or a consuming access of one.
  auto RangeIt = LocalRange.find(V);
  if (RangeIt == LocalRange.end() || Shape->Named.empty())
    return {};
  R.LocalBase = Shape->LocalRef;
  // The member directly on the object.
  const FieldDecl *Top = Shape->Named.back();
  if (Shape->Named.size() == 1 && !Shape->Subscript) {
    if (std::optional<unsigned> Idx = find(V, Shape->Path, Top)) {
      R.Entity = Idx;
      R.Benign = true;
      return R;
    }
    // An untracked sibling consumes the base without exposing the object
    // only when it cannot itself reach a tracked member: a pointer or
    // reference member may denote one, and a class or array member may hold
    // such a value.
    QualType FT = Top->getType();
    R.Benign = FT->isIntegralOrEnumerationType() || FT->isFloatingType();
    return R;
  }
  llvm::SmallPtrSet<const RecordDecl *, 8> Visited;
  R.Benign = !typeHoldsPointerOrReference(Top->getType(), Visited);
  return R;
}

TrackedStorage::Resolution
TrackedStorage::resolveFlowLeaf(const Expr *E, bool AsPointerValue) const {
  if (!AsPointerValue)
    return resolve(E);
  E = SemaProfiles::ignoreTransparentCasts(E);
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_AddrOf)
    return resolve(UO->getSubExpr());
  Resolution R;
  if (std::optional<unsigned> Idx = markedPointerObject(E)) {
    R.Entity = Idx;
    return R;
  }
  // A decayed [[uninit]] array.
  const auto *VD =
      dyn_cast_or_null<VarDecl>(SemaProfiles::getDirectlyNamedDecl(E));
  if (VD && VD->getType()->isArrayType())
    if (auto It = LocalEntity.find(VD);
        It != LocalEntity.end() &&
        Entities[It->second].K == TrackedEntity::Kind::WholeLocal)
      R.Entity = It->second;
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
TrackedStorage::markedPointerObject(const Expr *E) const {
  const auto *V = dyn_cast_or_null<VarDecl>(SemaProfiles::getDirectlyNamedDecl(
      SemaProfiles::ignoreTransparentCasts(E)));
  return V ? markedPointerObject(V) : std::nullopt;
}

/// Per-block ordered events recovered from the CFG by the std::init dataflow
/// engine and replayed over a FlowState in element order. Read: a value load
/// of entity Idx (read-tracked members). Write: Idx is assigned. ReadWrite: a
/// compound assignment or built-in ++/--, a Read then a Write. MayWrite: a
/// store whose target names Idx on one of several arms. Copy: Idx takes
/// entity Aux's state (the tracked-copy transfer). Kill: a storage release
/// (free, realloc, operator delete, a delete-expression) leaves Idx
/// unassigned. Destroy: a [[now_uninit]] destruction (DestroySites[Aux]),
/// judged by the destroy rules; its storage is then unassigned and destroyed
/// until stored again -- or, for an argument naming several storages,
/// escaped. Reseat: marked pointer Idx is reassigned, retiring every fact
/// about its pointee. Escape: Idx may have been reseated or destroyed by
/// something the analysis cannot see -- a mutable alias of marked pointer
/// Idx handed out, or a destroy or release on one of several arms -- so it
/// is possibly assigned, not definitely, and not destroyed. AggregateEscape:
/// the
/// tracked aggregate local owning member
/// Idx escapes as a whole (a non-benign DeclRefExpr), so reads of the member
/// are trusted from here (the local read leniency) while its assignment
/// state is unchanged. Binding: a pointer or reference binding whose source
/// has a tracked leaf (BindingSites[Aux]), judged by the binding rule.
/// ReadThrough / SubobjectWrite: a read through, or a store below, the
/// storage of Idx, judged by uninit_read / uninit_write. Every event carries
/// the anchor expression the diagnostic and the suppression walk use.
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
  AggregateEscape,
  Binding,
  ReadThrough,
  SubobjectWrite
};
struct DefAssignEvent {
  DefAssignEventKind Kind;
  unsigned Idx;
  const Expr *E;
  /// Copy: the source entity. Binding: the BindingSite index. Destroy: the
  /// DestroySite index. ReadThrough and SubobjectWrite: the diagnostic's
  /// provenance select.
  unsigned Aux = 0;
  /// SubobjectWrite: the diagnostic's "not a member access" flag.
  bool Flag = false;
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

/// One leaf of a binding source: the tracked entity it names (judged by
/// that entity's flow state; Subobject when the leaf reaches storage below
/// the entity) or, for a leaf no entity tracks, its form classification.
struct BindingLeaf {
  std::optional<unsigned> Entity;
  bool Subobject = false;
  LeafState State = LeafState::Unknown;
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
};

/// A [[now_uninit]] destruction site derived from a call element: the
/// argument's leaves (a tracked entity, judged by its flow state, or a leaf
/// no entity tracks, classified by form), the entities the destruction
/// affects, and whether it is exempt from destroy_uninit -- a reinitializer
/// (both lifecycle attributes: destroy-then-construct on fresh storage is
/// its purpose, and the exemption is call-wide), or a parameter that itself
/// carries [[ref_to_uninit]] (the marker declares that the parameter takes
/// possibly-uninitialized storage: the annotation spelling for an
/// unrecognized storage-release function). The rules are applied only when a
/// leaf names tracked storage (Judged), the parse-time funnel having judged
/// every other destroy; the transfer applies either way.
struct DestroySite {
  SourceLocation Loc;
  bool Exempt = false;
  bool Judged = false;
  /// The argument names several storages (a conditional or comma shape):
  /// each may have been destroyed, none definitely.
  bool Conditional = false;
  SmallVector<BindingLeaf, 2> Leaves;
  /// The entities destroyed: the whole entity each leaf names, or every
  /// entity of an object destroyed as a whole (`this`, `*this`, `&x`, `x`).
  SmallVector<unsigned, 2> Affected;
};

/// The dataflow state of every tracked entity at a program point: assigned
/// on every path (Must), on some path (May), escaped as a local aggregate on
/// every path (Esc: the read leniency for locals, ProfilesFramework.rst,
/// "Limitations"), and destroyed on every path and not stored since
/// (Destroyed). The join of predecessors intersects Must, Esc, and
/// Destroyed and unites May.
struct FlowState {
  llvm::BitVector Must, May, Esc, Destroyed;
  /// \p Top is the identity of the join: all-assigned, may-assigned on no
  /// path, escaped, destroyed -- the state of an unreached predecessor.
  FlowState(unsigned N, bool Top)
      : Must(N, Top), May(N, false), Esc(N, Top), Destroyed(N, Top) {}
  void meet(const FlowState &O) {
    Must &= O.Must;
    May |= O.May;
    Esc &= O.Esc;
    Destroyed &= O.Destroyed;
  }
  bool operator==(const FlowState &O) const {
    return Must == O.Must && May == O.May && Esc == O.Esc &&
           Destroyed == O.Destroyed;
  }
  bool operator!=(const FlowState &O) const { return !(*this == O); }
};

/// A violation the reporting replay collected, emitted through the shared
/// gate once the replay is complete.
struct PendingViolation {
  unsigned DiagID;
  SourceLocation Loc;
  const Expr *Anchor;
  /// Binding: the pointer/reference select; ReadThrough and SubobjectWrite:
  /// the provenance select.
  unsigned Select = 0;
  /// SubobjectWrite: the "not a member access" flag.
  bool Flag = false;
  /// The entity a kind-specific binding wording names; null for the generic
  /// wordings.
  const NamedDecl *Subject = nullptr;
};

/// The state of one binding leaf under \p St.
static LeafState leafState(const BindingLeaf &L, const FlowState &St) {
  if (!L.Entity)
    return L.State;
  if (St.Must.test(*L.Entity))
    return LeafState::Initialized;
  if (!St.May.test(*L.Entity))
    return LeafState::Uninitialized;
  return LeafState::Unknown;
}

/// Judge a binding site against \p St with the funnel's verdict rule: a
/// marked target rejects a source that is initialized (on every path) or
/// Mixed, an unmarked one a source that is uninitialized (on every path) or
/// Mixed; an Unknown source fires in neither direction.
static void judgeBindingSite(const BindingSite &Site, const FlowState &St,
                             SmallVectorImpl<PendingViolation> &Out) {
  using K = SemaProfiles::InitBindingKind;
  if (Site.Leaves.empty())
    return;
  LeafState Acc = leafState(Site.Leaves.front(), St);
  for (const BindingLeaf &L : llvm::drop_begin(Site.Leaves))
    Acc = combineLeafStates(Acc, leafState(L, St));
  if (Site.TargetMarked) {
    if (Acc == LeafState::Initialized || Acc == LeafState::Mixed)
      Out.push_back({diag::err_init_ref_to_uninit_requires_uninit,
                     Site.Loc, nullptr,
                     Site.IsReference ? 1u : 0u});
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
  Out.push_back({DiagID, Site.Loc, nullptr,
                 Site.IsReference ? 1u : 0u, false, Subject});
}

/// Judge a destroy site against \p St. Storage destroyed on every path
/// (every leaf's entity Destroyed; a leaf no entity tracks is never
/// destroyed) is double_destroy whatever the exemption -- a reinitializer's
/// destroy half is as invalid on destroyed storage as anyone else's, and
/// construct_at is the sanctioned recovery path. Otherwise a non-exempt
/// destroy of storage uninitialized on every path, or of a Mixed combination
/// of arms (P4222R2 §4.9), is destroy_uninit: destruction makes an object
/// uninitialized, so a first destroy of never-constructed storage is as much
/// an access to raw memory as a second one (P4222R2 §4.4).
static void judgeDestroySite(const DestroySite &Site, const FlowState &St,
                             SmallVectorImpl<PendingViolation> &Out) {
  if (Site.Leaves.empty())
    return;
  bool AllDestroyed = true;
  std::optional<LeafState> Acc;
  for (const BindingLeaf &L : Site.Leaves) {
    AllDestroyed &= L.Entity && St.Destroyed.test(*L.Entity);
    LeafState S = leafState(L, St);
    Acc = Acc ? combineLeafStates(*Acc, S) : S;
  }
  if (AllDestroyed) {
    Out.push_back(
        {diag::err_init_double_destroy, Site.Loc, nullptr});
    return;
  }
  if (!Site.Exempt &&
      (*Acc == LeafState::Uninitialized || *Acc == LeafState::Mixed))
    Out.push_back(
        {diag::err_init_destroy_uninit, Site.Loc, nullptr});
}

/// Replay a block's events over \p St: the engine's one block-level transfer
/// function, shared by the fixpoint and the reporting replay so the two can
/// never disagree. With \p Offending and \p Violations non-null (the
/// reporting replay), a read of a read-tracked entity that is not definitely
/// assigned nor escaped is collected, and every judged event's verdict is
/// taken against the state at its program point.
static void
applyDefAssignEvents(ArrayRef<DefAssignEvent> BlockEvents,
                     ArrayRef<BindingSite> Sites, ArrayRef<DestroySite> DSites,
                     FlowState &St,
                     std::vector<SmallVector<const Expr *, 2>> *Offending,
                     SmallVectorImpl<PendingViolation> *Violations) {
  auto Write = [&](unsigned I) {
    St.Must.set(I);
    St.May.set(I);
    St.Destroyed.reset(I);
  };
  for (const DefAssignEvent &Ev : BlockEvents) {
    switch (Ev.Kind) {
    case DefAssignEventKind::Read:
      if (Offending && !St.Must.test(Ev.Idx) && !St.Esc.test(Ev.Idx))
        (*Offending)[Ev.Idx].push_back(Ev.E);
      break;
    case DefAssignEventKind::Write:
      Write(Ev.Idx);
      break;
    case DefAssignEventKind::ReadWrite:
      if (Offending && !St.Must.test(Ev.Idx) && !St.Esc.test(Ev.Idx))
        (*Offending)[Ev.Idx].push_back(Ev.E);
      Write(Ev.Idx);
      break;
    case DefAssignEventKind::MayWrite:
      St.May.set(Ev.Idx);
      break;
    case DefAssignEventKind::Copy:
      St.Must[Ev.Idx] = St.Must.test(Ev.Aux);
      St.May[Ev.Idx] = St.May.test(Ev.Aux);
      St.Esc[Ev.Idx] = St.Esc.test(Ev.Aux);
      St.Destroyed[Ev.Idx] = St.Destroyed.test(Ev.Aux);
      break;
    case DefAssignEventKind::Kill:
      St.Must.reset(Ev.Idx);
      St.May.reset(Ev.Idx);
      St.Esc.reset(Ev.Idx);
      break;
    case DefAssignEventKind::Destroy: {
      const DestroySite &Site = DSites[Ev.Aux];
      if (Violations && Site.Judged) {
        size_t Before = Violations->size();
        judgeDestroySite(Site, St, *Violations);
        for (PendingViolation &V : llvm::drop_begin(*Violations, Before))
          V.Anchor = Ev.E;
      }
      for (unsigned I : Site.Affected) {
        St.Must.reset(I);
        if (Site.Conditional) {
          St.May.set(I);
          St.Destroyed.reset(I);
        } else {
          St.May.reset(I);
          St.Esc.reset(I);
          St.Destroyed.set(I);
        }
      }
      break;
    }
    case DefAssignEventKind::Reseat:
      St.Must.reset(Ev.Idx);
      St.May.reset(Ev.Idx);
      St.Destroyed.reset(Ev.Idx);
      break;
    case DefAssignEventKind::Escape:
      St.Must.reset(Ev.Idx);
      St.May.set(Ev.Idx);
      St.Destroyed.reset(Ev.Idx);
      break;
    case DefAssignEventKind::AggregateEscape:
      St.Esc.set(Ev.Idx);
      break;
    case DefAssignEventKind::Binding:
      if (Violations) {
        size_t Before = Violations->size();
        judgeBindingSite(Sites[Ev.Aux], St, *Violations);
        for (PendingViolation &V : llvm::drop_begin(*Violations, Before))
          V.Anchor = Ev.E;
      }
      break;
    case DefAssignEventKind::ReadThrough:
      if (Violations && !St.May.test(Ev.Idx))
        Violations->push_back({diag::err_init_uninit_read_through,
                               Ev.E->getExprLoc(), Ev.E,
                               Ev.Aux});
      break;
    case DefAssignEventKind::SubobjectWrite:
      if (Violations && !St.May.test(Ev.Idx))
        Violations->push_back({diag::err_init_uninit_subobject_write,
                               Ev.E->getExprLoc(), Ev.E, Ev.Aux,
                               Ev.Flag});
      break;
    }
  }
}

/// The forward dataflow of the std::init engine: nothing is assigned at
/// function entry (an entity of an enclosing function is may-assigned), a
/// block's entry is the join of its predecessors' exits (an entity is
/// definitely assigned at a point only if assigned on every incoming path,
/// the paper's all-branches rule, P4222R2 §1.3), and a block's transfer is
/// the event replay above -- the same replay the reporting pass uses. Every
/// transfer is monotone in each lattice (Read, Write, ReadWrite, MayWrite,
/// and AggregateEscape only set bits, Copy projects the source's bits, Kill,
/// Destroy, Reseat, and Escape are constant functions of their bits), so a
/// block's
/// exit only ever descends from the join identity in a finite lattice and
/// the iteration terminates. Unprocessed (unreachable) predecessors keep the
/// join identity, so unreachable code is never flagged. Returns, per
/// read-tracked entity, the reads at program points where the entity is not
/// definitely assigned nor escaped, and appends every other rule's
/// violations to \p Violations.
static std::vector<SmallVector<const Expr *, 2>>
runDefiniteAssignment(CFG &cfg, AnalysisDeclContext &AC,
                      const TrackedStorage &Storage,
                      ArrayRef<SmallVector<DefAssignEvent, 4>> Events,
                      ArrayRef<BindingSite> Sites, ArrayRef<DestroySite> DSites,
                      SmallVectorImpl<PendingViolation> &Violations) {
  const unsigned NumTracked = Storage.Entities.size();
  const unsigned NumBlocks = cfg.getNumBlockIDs();
  std::vector<FlowState> EntryState(NumBlocks, FlowState(NumTracked, true));
  std::vector<FlowState> ExitState(NumBlocks, FlowState(NumTracked, true));
  FlowState FunctionEntry(NumTracked, false);
  for (unsigned I = 0; I != NumTracked; ++I)
    if (Storage.Entities[I].UnknownEntry)
      FunctionEntry.May.set(I);
  const CFGBlock &CFGEntry = cfg.getEntry();
  ForwardDataflowWorklist Worklist(cfg, AC);
  Worklist.enqueueBlock(&CFGEntry);
  llvm::BitVector Visited(NumBlocks, false);
  while (const CFGBlock *B = Worklist.dequeue()) {
    FlowState In(NumTracked, true);
    if (B == &CFGEntry) {
      In = FunctionEntry;
    } else {
      bool First = true;
      for (const CFGBlock *Pred : B->preds()) {
        if (!Pred)
          continue;
        if (First) {
          In = ExitState[Pred->getBlockID()];
          First = false;
        } else {
          In.meet(ExitState[Pred->getBlockID()]);
        }
      }
    }
    EntryState[B->getBlockID()] = In;
    applyDefAssignEvents(Events[B->getBlockID()], Sites, DSites, In,
                         /*Offending=*/nullptr, /*Violations=*/nullptr);
    // Enqueue-skip subtlety: ExitState starts at the join identity, and a
    // block whose computed exit *equals* its stored exit enqueues no
    // successors. A transfer that drops a bit (Kill, Destroy, Reseat,
    // Escape) makes a block entered at the identity exit *below* it, so a
    // reachable block's first visit must propagate even when its computed
    // exit equals the stored identity (the Visited disjunct below) -- which
    // also makes Visited mean exactly "reachable from the entry".
    // Unreachable blocks are never enqueued (only successors of dequeued
    // blocks are), so they keep the identity: an unreachable destroy spoils
    // no reachable join. The reporting replay below walks only the visited
    // blocks: a destroy followed by a read inside an unreachable block would
    // otherwise flag unreachable code. Lowering the initial ExitState,
    // seeding EntryState differently, or replaying unvisited blocks would
    // each break the others' assumption; change them together or not at
    // all.
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
    applyDefAssignEvents(Events[B->getBlockID()], Sites, DSites, St, &Offending,
                         &Violations);
  }
  return Offending;
}

/// Report at the first offending read (in source order) that is not
/// suppressed, once per read-tracked entity, mirroring the local-variable
/// reporter. The profile name and rule arrive from the std::init CFGProfiles
/// row.
static void reportMemberReadsBeforeInit(
    Sema &S, AnalysisDeclContext &AC,
    MutableArrayRef<SmallVector<const Expr *, 2>> Offending,
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
              diag::err_init_member_read_before_init, R->getBeginLoc(),
              /*D=*/nullptr, /*PostParse=*/true))
        continue;
      const FieldDecl *F = Storage.Entities[I].Field;
      S.Diag(R->getBeginLoc(), diag::err_init_member_read_before_init)
          << Name << F->getDeclName();
      S.Diag(F->getLocation(), diag::note_init_uninit_member_here)
          << F->getDeclName();
      break;
    }
  }
}

/// Emit the collected binding, destroy, read-through, and subobject-write
/// violations in source order, each through the shared gate with its own
/// anchor for the suppression walk.
static void reportFlowViolations(Sema &S, AnalysisDeclContext &AC,
                                 MutableArrayRef<PendingViolation> Violations,
                                 StringRef Name) {
  llvm::stable_sort(
      Violations, [&](const PendingViolation &A, const PendingViolation &B) {
        return S.SourceMgr.isBeforeInTranslationUnit(A.Loc, B.Loc);
      });
  for (const PendingViolation &V : Violations) {
    if (!S.Profiles().shouldEmitProfileViolation(V.DiagID, V.Loc,
                                                 /*D=*/nullptr,
                                                 /*PostParse=*/true))
      continue;
    if (V.Subject)
      S.Diag(V.Loc, V.DiagID) << Name << V.Subject;
    else if (V.DiagID == diag::err_init_uninit_subobject_write)
      S.Diag(V.Loc, V.DiagID) << Name << V.Flag << V.Select;
    else if (V.DiagID == diag::err_init_double_destroy ||
             V.DiagID == diag::err_init_destroy_uninit)
      S.Diag(V.Loc, V.DiagID) << Name;
    else
      S.Diag(V.Loc, V.DiagID) << Name << V.Select;
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

/// The lifecycle effects of the call \p CE on the storage bound to its
/// parameters (\p Roles, derived by the caller; \p ArgOffset skips a member
/// operator's object argument). A [[now_init]] callee writes the storage
/// bound to each of its [[ref_to_uninit]] parameters (P4222R2 §6.2): a
/// member passed as `&m` / `m`, a marked pointer's pointee passed as `p`, a
/// decayed [[uninit]] array, an [[uninit]] local passed as `&u` / `u`, or
/// every tracked member of an object passed as a whole (`this`, `*this`,
/// `&x`, `x`); an argument whose arms name several storages may-writes each.
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
/// is replay order -- so a dual-attributed reinitializer nets to assigned. A
/// plain callee earns nothing. The bindings judged at this call precede
/// these effects in the block, so they see the pre-call state.
static void appendLifecycleCallEvents(
    Sema &S, const Decl *D, const CallExpr *CE, const FunctionDecl *Callee,
    const SemaProfiles::CalleeLifecycleRoles &Roles, unsigned ArgOffset,
    const TrackedStorage &Storage, SmallVectorImpl<DefAssignEvent> &BlockEvents,
    SmallVectorImpl<DestroySite> &DSites) {
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
  };
  auto ResolveArg = [&](const Expr *Arg, QualType ParamTy,
                        SmallVectorImpl<ArgLeaf> &Leaves) {
    bool AsPointerValue;
    const Expr *Operand =
        SemaProfiles::bindingSourceOperand(Arg, ParamTy, AsPointerValue);
    if (!Operand)
      return false;
    bool AnyTracked = false;
    SemaProfiles::forEachTargetLeaf(
        Operand, /*ConditionalArm=*/false, [&](const Expr *Leaf, bool) {
          ArgLeaf L{Leaf, Storage.resolveFlowLeaf(Leaf, AsPointerValue)};
          AnyTracked |= SemaProfiles::isFlowTrackedLeaf(S.Context, Leaf,
                                                        AsPointerValue, DC);
          if (!L.R.Entity) {
            const Expr *G = SemaProfiles::ignoreTransparentCasts(Leaf);
            if (const auto *AddrOf = dyn_cast<UnaryOperator>(G);
                AsPointerValue && AddrOf && AddrOf->getOpcode() == UO_AddrOf)
              G = AddrOf->getSubExpr();
            L.Whole = Storage.wholeObject(G);
          }
          Leaves.push_back(L);
        });
    return AnyTracked;
  };
  // The storage an argument names, as events of \p Kind: the resolved whole
  // entity, or every entity of the object passed as a whole.
  auto AppendFor = [&](const Expr *Arg, QualType ParamTy,
                       DefAssignEventKind Kind) {
    SmallVector<ArgLeaf, 2> Leaves;
    ResolveArg(Arg, ParamTy, Leaves);
    if (Leaves.size() != 1)
      Kind = Kind == DefAssignEventKind::Write ? DefAssignEventKind::MayWrite
                                               : DefAssignEventKind::Escape;
    for (const ArgLeaf &L : Leaves) {
      if (L.R.Entity) {
        if (!L.R.Subobject)
          BlockEvents.push_back({Kind, *L.R.Entity, CE});
        continue;
      }
      for (unsigned Idx = L.Whole.first; Idx != L.Whole.second; ++Idx)
        BlockEvents.push_back({Kind, Idx, CE});
    }
  };
  // A [[now_uninit]] parameter's argument: one destroy site.
  auto AppendDestroy = [&](const ParmVarDecl *P, const Expr *Arg) {
    QualType PT = P->getType();
    SmallVector<ArgLeaf, 2> Leaves;
    DestroySite Site;
    Site.Loc = Arg->IgnoreImplicit()->getExprLoc();
    Site.Exempt =
        Roles.InitializesRefToUninitParams || P->hasAttr<RefToUninitAttr>();
    Site.Judged = ResolveArg(Arg, PT, Leaves);
    Site.Conditional = Leaves.size() > 1;
    for (const ArgLeaf &L : Leaves) {
      BindingLeaf Leaf;
      if (L.R.Entity) {
        Leaf.Entity = L.R.Entity;
        Leaf.Subobject = L.R.Subobject;
        if (!L.R.Subobject)
          Site.Affected.push_back(*L.R.Entity);
      } else {
        Leaf.State =
            formState(S.Profiles().classifyInitBindingLeaf(L.E, PT, D));
        for (unsigned Idx = L.Whole.first; Idx != L.Whole.second; ++Idx)
          Site.Affected.push_back(Idx);
      }
      Site.Leaves.push_back(Leaf);
    }
    if (!Site.Judged && Site.Affected.empty())
      return;
    DSites.push_back(std::move(Site));
    BlockEvents.push_back(
        {DefAssignEventKind::Destroy, 0, CE, (unsigned)(DSites.size() - 1)});
  };
  auto ForEachParam =
      [&](llvm::function_ref<void(const ParmVarDecl *, const Expr *)> F) {
        for (unsigned PI = 0, NP = Callee->getNumParams(); PI != NP; ++PI) {
          if (PI + ArgOffset >= CE->getNumArgs())
            break;
          F(Callee->getParamDecl(PI), CE->getArg(PI + ArgOffset));
        }
      };
  if (Roles.DestroysPointerParams || Roles.ReleasesStorageTrusted)
    ForEachParam([&](const ParmVarDecl *P, const Expr *Arg) {
      QualType PT = P->getType();
      if (!PT->isPointerType() && !PT->isReferenceType())
        return;
      if (Roles.DestroysPointerParams)
        AppendDestroy(P, Arg);
      else
        AppendFor(Arg, PT, DefAssignEventKind::Kill);
    });
  if (Roles.InitializesRefToUninitParams)
    ForEachParam([&](const ParmVarDecl *P, const Expr *Arg) {
      if (P->hasAttr<RefToUninitAttr>())
        AppendFor(Arg, P->getType(), DefAssignEventKind::Write);
    });
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
    if (G)
      if (std::optional<unsigned> Idx = Storage.resolve(G).Entity;
          Idx &&
          Storage.Entities[*Idx].K ==
              TrackedEntity::Kind::CurrentObjectMember &&
          Storage.Entities[*Idx].TrackReads)
        BlockEvents.push_back(
            {DefAssignEventKind::Read, *Idx, cast<Expr>(Cur)});
    for (const Stmt *Child : Cur->children())
      Stack.push_back(Child);
  }
}

static const DeclRefExpr *getLocalCopySourceRef(const VarDecl *V,
                                                BasePath &CastPath);

/// The statements of a constructor body the engine takes current-object
/// events from: the body plus each *written* member/base initializer
/// expression (the CFG is built with AddInitializers=true, so those run as
/// CFG elements in execution order, and a written initializer's own member is
/// assigned at its CFGInitializer element, so declaration order decides what
/// an initializer may read). An NSDMI's subexpressions are excluded although
/// AddCXXDefaultInitExprInCtors puts them in the CFG, so a read of a tracked
/// member inside another member's default initializer stays undetected (see
/// ProfilesFramework.rst, "Limitations").
static void collectCtorBodyStmts(const CXXConstructorDecl *Ctor,
                                 llvm::SmallPtrSetImpl<const Stmt *> &Body) {
  SmallVector<const Stmt *, 32> Stack;
  if (const Stmt *B = Ctor->getBody())
    Stack.push_back(B);
  for (const CXXCtorInitializer *Init : Ctor->inits())
    if (Init->isWritten())
      Stack.push_back(Init->getInit());
  while (!Stack.empty()) {
    const Stmt *Cur = Stack.pop_back_val();
    if (!Cur || !Body.insert(Cur).second)
      continue;
    for (const Stmt *Child : Cur->children())
      Stack.push_back(Child);
  }
}

/// Recover the per-block std::init events of \p cfg over \p Storage: the one
/// extraction loop of the engine. Every store arm resolves through
/// TrackedStorage::resolve and distributes a comma, conditional, or GNU ?:
/// lvalue to its leaves (SemaProfiles::forEachTargetLeaf): a load reads
/// whichever member the chosen arm names; a store writes the one whole
/// entity every leaf names and otherwise may-writes each named entity (a
/// compound one reads each leaf); a store to a marked pointer reseats its
/// pointee. A written member or base initializer writes its members; a
/// lifecycle call kills or writes the storage bound to its parameters
/// (appendLifecycleCallEvents); a this-capturing lambda reads members at its
/// creation; a non-benign DeclRefExpr of a tracked aggregate local is an
/// escape (the read leniency for locals, ProfilesFramework.rst, "Reads of
/// Uninitialized Objects"); a tracked copy's DeclStmt copies each member's
/// state. Every pointer or reference binding the body performs -- a variable
/// or member initializer, an aggregate element, a call or constructor
/// argument (declared, variadic, or the implicit object argument), a return,
/// a throw, a new-expression's initializer, a pointer assignment, a lambda
/// capture -- becomes a Binding site when a leaf of its source names tracked
/// storage (SemaProfiles::isFlowTrackedLeaf: the parse-time funnel left
/// exactly those to this pass), its other leaves classified by form; a
/// binding that hands out a mutable alias of a marked pointer (`T *&` to
/// `p`, `T **` to `&p`, a by-reference capture) escapes its pointee, and a
/// delete-expression kills its operand's storage; a [[now_uninit]] call's
/// arguments become Destroy sites (appendLifecycleCallEvents). Current-object
/// events of a constructor (\p Ctor non-null) come from the constructor body
/// and its written initializers only. \p cfg is the engine's own, fully
/// linearized CFG (runStdInitMemberReadChecks), so every statement class has
/// an element.
static void extractStdInitEvents(
    Sema &S, const Decl *D, const CFG &cfg, const TrackedStorage &Storage,
    const CXXConstructorDecl *Ctor, bool StarThisCopyTrusted,
    std::vector<SmallVector<DefAssignEvent, 4>> &Events,
    SmallVectorImpl<BindingSite> &Sites, SmallVectorImpl<DestroySite> &DSites) {
  using K = SemaProfiles::InitBindingKind;
  ASTContext &Ctx = S.Context;
  const auto *DC = cast<DeclContext>(D);
  llvm::SmallPtrSet<const Stmt *, 32> BodyStmts;
  if (Ctor)
    collectCtorBodyStmts(Ctor, BodyStmts);
  auto ForEachLeaf =
      [&](const Expr *E,
          llvm::function_ref<void(const TrackedStorage::Resolution &)> F) {
        SemaProfiles::forEachTargetLeaf(
            E, /*ConditionalArm=*/false,
            [&](const Expr *Leaf, bool) { F(Storage.resolve(Leaf)); });
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
      }
    }
  }

  // Pass two: the events, in CFG element order.
  for (const CFGBlock *B : cfg) {
    auto &BlockEvents = Events[B->getBlockID()];
    // The resolutions of a glvalue's leaves, one per leaf.
    SmallVector<TrackedStorage::Resolution, 4> Leaves;
    auto CollectLeaves = [&](const Expr *G) {
      Leaves.clear();
      ForEachLeaf(
          G, [&](const TrackedStorage::Resolution &R) { Leaves.push_back(R); });
    };
    // One Read per distinct resolved, read-tracked whole-entity leaf.
    auto AppendReads = [&](const Expr *At) {
      SmallVector<unsigned, 4> Seen;
      for (const TrackedStorage::Resolution &R : Leaves)
        if (R.Entity && !R.Subobject &&
            Storage.Entities[*R.Entity].TrackReads &&
            !llvm::is_contained(Seen, *R.Entity)) {
          Seen.push_back(*R.Entity);
          BlockEvents.push_back({DefAssignEventKind::Read, *R.Entity, At});
        }
    };
    // A store to \p G: one Write (or ReadWrite) when every leaf names the
    // same whole entity; otherwise the chosen arm is unknown, so each named
    // whole entity is may-written and a compound store reads each leaf. A
    // store below a whole entity (a subobject) initializes nothing. Every
    // marked pointer a leaf names as its object is reseated.
    auto AppendStore = [&](const Expr *G, const Expr *At, bool ReadsFirst) {
      CollectLeaves(G);
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
      if (AllSame) {
        BlockEvents.push_back({ReadsFirst && Storage.Entities[*Same].TrackReads
                                   ? DefAssignEventKind::ReadWrite
                                   : DefAssignEventKind::Write,
                               *Same, At});
      } else {
        if (ReadsFirst)
          AppendReads(At);
        for (const TrackedStorage::Resolution &R : Leaves)
          if (R.Entity && !R.Subobject)
            BlockEvents.push_back(
                {DefAssignEventKind::MayWrite, *R.Entity, At});
      }
      SmallVector<unsigned, 2> Reseated;
      SemaProfiles::forEachTargetLeaf(
          G, /*ConditionalArm=*/false, [&](const Expr *Leaf, bool) {
            if (std::optional<unsigned> Idx = Storage.markedPointerObject(Leaf);
                Idx && !llvm::is_contained(Reseated, *Idx)) {
              Reseated.push_back(*Idx);
              BlockEvents.push_back({DefAssignEventKind::Reseat, *Idx, At});
            }
          });
    };
    // The mutable alias a binding of \p Src as \p T hands out (P4222R2
    // §4.3): `p` bound to a `T *&`, or `&p` bound to a `T **`, per leaf. A
    // const pointer cannot be reseated through either.
    auto AppendAliasEscapes = [&](const Expr *Src, QualType T, const Expr *At) {
      if (!Src || T.isNull())
        return;
      bool ByRef = SemaProfiles::pointerAliasKind(T) ==
                   SemaProfiles::PointerAliasKind::Mutable;
      bool ByAddr = T->isPointerType() &&
                    T->getPointeeType()->isPointerType() &&
                    !T->getPointeeType().isConstQualified();
      if (!ByRef && !ByAddr)
        return;
      SemaProfiles::forEachTargetLeaf(
          Src, /*ConditionalArm=*/false, [&](const Expr *Leaf, bool) {
            const Expr *G = SemaProfiles::ignoreTransparentCasts(Leaf);
            if (ByAddr) {
              const auto *UO = dyn_cast<UnaryOperator>(G);
              if (!UO || UO->getOpcode() != UO_AddrOf)
                return;
              G = UO->getSubExpr();
            }
            if (std::optional<unsigned> Idx = Storage.markedPointerObject(G))
              BlockEvents.push_back({DefAssignEventKind::Escape, *Idx, At});
          });
    };
    // A pointer or reference binding of \p Src as \p T, with the parse-time
    // funnel's kind, location, and target marking (SemaProfiles::
    // checkInitProfileBinding): a site judged at \p At against the flow state
    // when a leaf names tracked storage; the funnel judged every other
    // binding at parse time. A leaf no entity tracks is classified by form.
    auto AddBinding = [&](K Kind, SourceLocation Loc, bool TargetMarked,
                          QualType T, const Expr *Src, const NamedDecl *Subject,
                          const Expr *At) {
      if (!Src || T.isNull() || T->isDependentType() ||
          (!T->isPointerType() && !T->isReferenceType()) ||
          isa<RecoveryExpr>(Src->IgnoreParens()))
        return;
      bool AsPointerValue;
      const Expr *Operand =
          SemaProfiles::bindingSourceOperand(Src, T, AsPointerValue);
      if (!Operand)
        return;
      BindingSite Site{Kind,    Loc, TargetMarked, T->isReferenceType(),
                       Subject, {}};
      bool AnyTracked = false;
      SemaProfiles::forEachTargetLeaf(
          Operand, /*ConditionalArm=*/false, [&](const Expr *Leaf, bool) {
            BindingLeaf L;
            if (SemaProfiles::isFlowTrackedLeaf(Ctx, Leaf, AsPointerValue,
                                                DC)) {
              AnyTracked = true;
              TrackedStorage::Resolution R =
                  Storage.resolveFlowLeaf(Leaf, AsPointerValue);
              L.Entity = R.Entity;
              L.Subobject = R.Subobject;
            }
            if (!L.Entity)
              L.State =
                  formState(S.Profiles().classifyInitBindingLeaf(Leaf, T, D));
            Site.Leaves.push_back(L);
          });
      if (!AnyTracked)
        return;
      Sites.push_back(std::move(Site));
      BlockEvents.push_back(
          {DefAssignEventKind::Binding, 0, At, (unsigned)(Sites.size() - 1)});
    };
    // The element bindings of an aggregate initializer of type \p T: each
    // pointer or reference field of a record (the bases consume the leading
    // initializers, unnamed bit-fields none) is a member binding, each
    // element of an array, vector, or complex a bare element binding. A
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
                   Init);
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
        unsigned I = 0;
        if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD))
          I = CRD->getNumBases();
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

    for (const CFGElement &Elem : *B) {
      if (auto OptInit = Elem.getAs<CFGInitializer>()) {
        // A written member initializer binds and then assigns its member at
        // this point in execution order, after its init expression's events
        // above it; a written base initializer (e.g. `: Base{1}`) gives the
        // tracked members of that constructor-less base subtree their
        // values.
        const CXXCtorInitializer *CI = OptInit->getInitializer();
        if (!Ctor || !CI->isWritten())
          continue;
        if (CI->isAnyMemberInitializer()) {
          const FieldDecl *F = CI->getAnyMember();
          if (!F)
            continue;
          AddBinding(K::DataMember, CI->getMemberLocation(),
                     F->hasAttr<RefToUninitAttr>(), F->getType(), CI->getInit(),
                     nullptr, CI->getInit());
          AppendAliasEscapes(CI->getInit(), F->getType(), CI->getInit());
          if (std::optional<unsigned> Idx = Storage.find(nullptr, {}, F))
            BlockEvents.push_back(
                {DefAssignEventKind::Write, *Idx, CI->getInit()});
        } else if (CI->isBaseInitializer()) {
          // Every entity below that base: its path starts with the base.
          const auto *BRD = CI->getBaseClass()->getAsCXXRecordDecl();
          if (!BRD)
            continue;
          for (unsigned Idx = Storage.CurrentObjectRange.first;
               Idx != Storage.CurrentObjectRange.second; ++Idx) {
            const BasePath &Path = Storage.Entities[Idx].Path;
            if (!Path.empty() && Path.front() == BRD->getCanonicalDecl())
              BlockEvents.push_back(
                  {DefAssignEventKind::Write, Idx, CI->getInit()});
          }
        }
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
      } else if (const auto *BO = dyn_cast<BinaryOperator>(St)) {
        if (!BO->isAssignmentOp())
          continue;
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
                    /*ReadsFirst=*/BO->isCompoundAssignmentOp());
      } else if (const auto *UO = dyn_cast<UnaryOperator>(St)) {
        // A built-in ++m / m++ / --m / m-- reads the old value and then
        // writes, but unlike -m / !m it carries no lvalue-to-rvalue cast, so
        // the Read arm never sees it: a ReadWrite.
        if (!UO->isIncrementDecrementOp())
          continue;
        AppendStore(UO->getSubExpr(), UO, /*ReadsFirst=*/true);
      } else if (const auto *CE = dyn_cast<CallExpr>(St)) {
        const FunctionDecl *Callee = CE->getDirectCallee();
        // Zip declared parameters with arguments. A member operator called
        // through CXXOperatorCallExpr receives the object as argument 0
        // ahead of its declared parameters -- for a C++23 static operator
        // too, whose object argument is still evaluated -- so skip it. An
        // explicit-object member function instead declares its object as
        // parameter 0, so its mapping is already direct.
        unsigned ArgOffset = 0;
        const CXXMethodDecl *ObjectMethod = nullptr;
        const Expr *Object = nullptr;
        if (isa<CXXOperatorCallExpr>(CE)) {
          if (const auto *MD = dyn_cast_or_null<CXXMethodDecl>(Callee);
              MD && !MD->isExplicitObjectMemberFunction()) {
            ArgOffset = 1;
            if (!MD->isStatic() && CE->getNumArgs() > 0) {
              ObjectMethod = MD;
              Object = CE->getArg(0);
            }
          }
        } else if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE)) {
          if (const CXXMethodDecl *MD = MCE->getMethodDecl();
              MD && !MD->isStatic() && !MD->isExplicitObjectMemberFunction()) {
            ObjectMethod = MD;
            Object = MCE->getImplicitObjectArgument();
          }
        }
        // The implicit object parameter: a pointer for an arrow call, a
        // reference to the object otherwise. A destructor call is the destroy
        // rules' (SemaProfiles::checkInitProfileObjectArgument).
        if (ObjectMethod && Object && !isa<CXXDestructorDecl>(ObjectMethod)) {
          QualType OT = Object->getType();
          QualType T =
              OT->isPointerType()
                  ? OT
                  : Ctx.getLValueReferenceType(OT.getNonReferenceType());
          AddBinding(K::ObjectArgument, Object->IgnoreImplicit()->getExprLoc(),
                     /*TargetMarked=*/false, T, Object, ObjectMethod, CE);
        }
        QualType CalleeTy = CE->getCallee()->getType();
        if (CalleeTy->isAnyPointerType() || CalleeTy->isBlockPointerType() ||
            CalleeTy->isReferenceType())
          CalleeTy = CalleeTy->getPointeeType();
        const auto *Proto = CalleeTy->getAs<FunctionProtoType>();
        // A builtin with custom type checking has no parameter list to zip:
        // __builtin_operator_new / __builtin_operator_delete resolve to a
        // usual allocation or deallocation function and copy-initialize
        // their arguments into its parameters (type-only entities, so
        // nothing carries the marker), every other one converts its
        // arguments itself and binds nothing. A defaulted argument was
        // judged when its CXXDefaultArgExpr was built; a call with no
        // declared callee (through a function pointer) has parameter types
        // but no declaration to carry the marker.
        unsigned BuiltinID = Callee ? Callee->getBuiltinID() : 0;
        bool CustomBuiltin =
            BuiltinID && Ctx.BuiltinInfo.hasCustomTypechecking(BuiltinID);
        SemaProfiles::CalleeLifecycleRoles Roles =
            Callee ? SemaProfiles::getCalleeLifecycleRoles(Callee)
                   : SemaProfiles::CalleeLifecycleRoles();
        // A [[now_uninit]] or storage-release callee's parameter runs the
        // destroy rules instead of the binding check.
        bool DestroyOrRelease = Roles.DestroysPointerParams ||
                                Roles.ReleasesStorageTrusted ||
                                Roles.ReleasesStorageByName;
        if (CustomBuiltin) {
          if (BuiltinID == Builtin::BI__builtin_operator_new ||
              BuiltinID == Builtin::BI__builtin_operator_delete)
            for (const Expr *Arg : CE->arguments()) {
              AddBinding(K::Parameter, Arg->IgnoreImplicit()->getExprLoc(),
                         /*TargetMarked=*/false, Arg->getType(), Arg, nullptr,
                         CE);
              AppendAliasEscapes(Arg, Arg->getType(), CE);
            }
        } else if (Callee || Proto) {
          unsigned NP = Callee ? Callee->getNumParams() : Proto->getNumParams();
          for (unsigned I = ArgOffset, N = CE->getNumArgs(); I != N; ++I) {
            const Expr *Arg = CE->getArg(I);
            if (isa<CXXDefaultArgExpr>(Arg))
              continue;
            unsigned PI = I - ArgOffset;
            if (PI < NP) {
              const ParmVarDecl *Parm =
                  Callee ? Callee->getParamDecl(PI) : nullptr;
              QualType T = Parm ? Parm->getType() : Proto->getParamType(PI);
              if (!DestroyOrRelease)
                AddBinding(K::Parameter, Arg->IgnoreImplicit()->getExprLoc(),
                           Parm && Parm->hasAttr<RefToUninitAttr>(), T, Arg,
                           nullptr, CE);
              AppendAliasEscapes(Arg, T, CE);
            } else {
              AddBinding(
                  K::VariadicArgument, Arg->IgnoreImplicit()->getExprLoc(),
                  /*TargetMarked=*/false, Arg->getType(), Arg, nullptr, CE);
              AppendAliasEscapes(Arg, Arg->getType(), CE);
            }
          }
        }
        if (Callee)
          appendLifecycleCallEvents(S, D, CE, Callee, Roles, ArgOffset, Storage,
                                    BlockEvents, DSites);
      } else if (const auto *CCE = dyn_cast<CXXConstructExpr>(St)) {
        // A constructor call's parameter bindings, the copy of a class object
        // out of tracked storage included.
        const CXXConstructorDecl *CD = CCE->getConstructor();
        for (unsigned I = 0, N = CCE->getNumArgs(); I != N; ++I) {
          const Expr *Arg = CCE->getArg(I);
          if (isa<CXXDefaultArgExpr>(Arg))
            continue;
          if (I < CD->getNumParams()) {
            const ParmVarDecl *Parm = CD->getParamDecl(I);
            AddBinding(K::Parameter, Arg->IgnoreImplicit()->getExprLoc(),
                       Parm->hasAttr<RefToUninitAttr>(), Parm->getType(), Arg,
                       nullptr, CCE);
            AppendAliasEscapes(Arg, Parm->getType(), CCE);
          } else {
            AddBinding(K::VariadicArgument, Arg->IgnoreImplicit()->getExprLoc(),
                       /*TargetMarked=*/false, Arg->getType(), Arg, nullptr,
                       CCE);
            AppendAliasEscapes(Arg, Arg->getType(), CCE);
          }
        }
      } else if (const auto *LE = dyn_cast<LambdaExpr>(St)) {
        if (Ctor)
          appendThisCaptureLambdaReadEvents(LE, Storage, StarThisCopyTrusted,
                                            BlockEvents);
        // An init-capture is a variable binding; a by-reference capture
        // binds an unmarked reference to the variable and hands out a
        // mutable alias of a marked pointer; a by-copy capture copies a
        // pointer into a closure field, which cannot carry the marker.
        auto InitIt = LE->capture_init_begin();
        for (const LambdaCapture &C : LE->captures()) {
          const Expr *Init =
              InitIt != LE->capture_init_end() ? *InitIt++ : nullptr;
          if (!C.capturesVariable())
            continue;
          const auto *V = dyn_cast<VarDecl>(C.getCapturedVar());
          if (!V)
            continue;
          if (V->isInitCapture()) {
            AddBinding(K::Variable, C.getLocation(),
                       V->hasAttr<RefToUninitAttr>(), V->getType(),
                       V->getInit(), nullptr, LE);
            AppendAliasEscapes(V->getInit(), V->getType(), LE);
            continue;
          }
          QualType VT = V->getType().getNonReferenceType();
          if (C.getCaptureKind() == LCK_ByRef) {
            AddBinding(K::ByRefCapture, C.getLocation(),
                       /*TargetMarked=*/false, Ctx.getLValueReferenceType(VT),
                       Init, V, LE);
            if (!VT.isConstQualified())
              if (std::optional<unsigned> Idx = Storage.markedPointerObject(V))
                BlockEvents.push_back({DefAssignEventKind::Escape, *Idx, LE});
          } else if (C.getCaptureKind() == LCK_ByCopy) {
            AddBinding(K::ByCopyCapture, C.getLocation(),
                       /*TargetMarked=*/false, VT, Init, nullptr, LE);
          }
        }
      } else if (const auto *DRE = dyn_cast<DeclRefExpr>(St)) {
        if (Benign.count(DRE))
          continue;
        std::pair<unsigned, unsigned> Range = Storage.wholeObject(DRE);
        for (unsigned Idx = Range.first; Idx != Range.second; ++Idx)
          BlockEvents.push_back(
              {DefAssignEventKind::AggregateEscape, Idx, DRE});
      } else if (const auto *DS = dyn_cast<DeclStmt>(St)) {
        for (const Decl *Dcl : DS->decls()) {
          const auto *V = dyn_cast<VarDecl>(Dcl);
          if (!V || V->isInvalidDecl())
            continue;
          // The variable's own binding (a decomposition's hidden variable
          // included); the DeclStmt element follows its initializer's
          // subexpression elements, so the judgment sees their effects.
          if (const Expr *Init = V->getInit()) {
            AddBinding(K::Variable, V->getLocation(),
                       V->hasAttr<RefToUninitAttr>(), V->getType(), Init,
                       nullptr, Init);
            AppendAliasEscapes(Init, V->getType(), Init);
          }
          // A tracked copy: each dest member's state becomes its source
          // member's -- the source entity at the copy's derived-to-base path
          // followed by the member's own path, so a sliced copy maps onto the
          // right base subobject. A source entity that does not exist cannot
          // occur (the dest's members are a subset of the source's); fall
          // back to Write (assume assigned) if it somehow does.
          auto CopyIt = Storage.CopySource.find(V);
          if (CopyIt == Storage.CopySource.end())
            continue;
          auto Range = Storage.LocalRange.find(V)->second;
          for (unsigned Idx = Range.first; Idx != Range.second; ++Idx) {
            BasePath SrcPath(CopyIt->second.Path);
            SrcPath.append(Storage.Entities[Idx].Path);
            if (std::optional<unsigned> SrcIdx = Storage.find(
                    CopyIt->second.Src, SrcPath, Storage.Entities[Idx].Field))
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
                   RV);
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
        // A delete-expression releases its operand's storage.
        SmallVector<const Expr *, 2> Operands;
        SemaProfiles::forEachTargetLeaf(
            DE->getArgument(), /*ConditionalArm=*/false,
            [&](const Expr *Leaf, bool) { Operands.push_back(Leaf); });
        if (Operands.size() != 1)
          continue;
        TrackedStorage::Resolution R =
            Storage.resolveFlowLeaf(Operands.front(), /*AsPointerValue=*/true);
        if (R.Entity && !R.Subobject)
          BlockEvents.push_back({DefAssignEventKind::Kill, *R.Entity, DE});
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
// slicing copy converts it through; null otherwise. The cast peel resolves
// the move form (`Agg b = static_cast<Agg&&>(a);`) to its named operand,
// mirroring the parse-time recognizers' pass-through.
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

/// Harvest the tracked storage of the analyzed body \p D into \p Storage.
/// Read-tracked (TrackedEntity::TrackReads) are the members of by-value
/// parameters of a trackable class without a default argument, of
/// default-initialized constructor-less aggregate locals (the body's
/// DeclStmts in source order, a lambda's or block's body excluded as a
/// separate function), and of locals copy- or move-constructed from a
/// read-tracked local (recorded in TrackedStorage::CopySource; iterated to a
/// fixpoint so a copy of a copy resolves). Tracked for bindings only are the
/// members of every other local, handler variable, or parameter whose class
/// has marked scalar members (SemaProfiles::flowTrackedAggregateClass),
/// every [[uninit]] local or parameter as a whole, and the referent of every
/// [[ref_to_uninit]] local pointer or reference; a variable of an enclosing
/// function the body reaches by capture is tracked from an unknown entry
/// state. The read-tracking premises -- a copy does not inherit
/// initialization (P4222R2 §5.2), a by-value parameter is a copy of the
/// caller's argument, a defaulted parameter is not -- are argued in
/// ProfilesFramework.rst, "Reads of Uninitialized Objects" and
/// "Limitations".
static void harvestTrackedLocals(Sema &S, const Decl *D,
                                 TrackedStorage &Storage) {
  // A marked local or parameter is tracked as a whole ([[uninit]]) or by
  // its referent ([[ref_to_uninit]] pointer or reference).
  auto HarvestMarked = [&](const VarDecl *V, bool UnknownEntry) {
    if (V->isInvalidDecl() || !V->hasLocalStorage())
      return;
    if (V->hasAttr<UninitAttr>())
      Storage.addLocalEntity(TrackedEntity::Kind::WholeLocal, V, UnknownEntry);
    else if (V->hasAttr<RefToUninitAttr>() &&
             (V->getType()->isPointerType() || V->getType()->isReferenceType()))
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
      Storage.addObject(S, P, RD,
                        /*TrackReads=*/!P->hasDefaultArg() &&
                            SemaProfiles::getTrackableSlotClass(P->getType()),
                        /*UnknownEntry=*/false);
  }
  // The body's own variables (DeclStmts and handler declarations, in source
  // order) and the local-storage variables of enclosing functions it reaches
  // by capture.
  SmallVector<const VarDecl *, 8> Locals;
  SmallVector<const DeclStmt *, 8> DeclStmts;
  SmallVector<const VarDecl *, 4> Referenced;
  {
    SmallVector<const Stmt *, 32> Stack;
    if (const Stmt *Body = D->getBody())
      Stack.push_back(Body);
    while (!Stack.empty()) {
      const Stmt *Cur = Stack.pop_back_val();
      if (!Cur)
        continue;
      if (const auto *DS = dyn_cast<DeclStmt>(Cur)) {
        DeclStmts.push_back(DS);
        for (const Decl *Dcl : DS->decls())
          if (const auto *V = dyn_cast<VarDecl>(Dcl);
              V && Owned.insert(V).second)
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
      if (const auto *LE = dyn_cast<LambdaExpr>(Cur)) {
        for (const Expr *Init : llvm::reverse(LE->capture_inits()))
          Stack.push_back(Init);
        continue;
      }
      if (isa<BlockExpr>(Cur))
        continue;
      SmallVector<const Stmt *, 8> Children(Cur->children());
      for (const Stmt *Child : llvm::reverse(Children))
        Stack.push_back(Child);
    }
  }
  for (const VarDecl *V : Locals) {
    HarvestMarked(V, /*UnknownEntry=*/false);
    if (const CXXRecordDecl *RD = getTrackedLocalAggregate(V))
      Storage.addObject(S, V, RD, /*TrackReads=*/true, /*UnknownEntry=*/false);
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
            Storage.addObject(S, V, RD, /*TrackReads=*/true,
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
        Storage.addObject(S, V, RD, /*TrackReads=*/false,
                          /*UnknownEntry=*/false);
  // Variables of enclosing functions, from an unknown entry state.
  llvm::SmallPtrSet<const VarDecl *, 4> Seen;
  for (const VarDecl *V : Referenced) {
    if (Owned.count(V) || !Seen.insert(V).second)
      continue;
    HarvestMarked(V, /*UnknownEntry=*/true);
    if (!Storage.LocalRange.count(V) && !Storage.LocalEntity.count(V))
      if (const CXXRecordDecl *RD = SemaProfiles::flowTrackedAggregateClass(V))
        Storage.addObject(S, V, RD, /*TrackReads=*/false,
                          /*UnknownEntry=*/true);
  }
}

/// The std::init flow engine over one body: the member read-before-init rule
/// (P4222R2 §4.2, §5.1-§5.4) and the binding and destroy judgments of
/// flow-tracked storage (ProfilesFrameworkInternals.rst, "Flow-Tracked
/// Storage") -- one entity
/// table, one event extraction, one definite-assignment run, one report,
/// over a CFG of the engine's own. The current object's members are tracked
/// in every instance member function body (a lambda's or block's body inside
/// one included), from the unassigned entry state; their reads are tracked
/// in a constructor body only. A delegating constructor's target initializes
/// the members first and a union's members are mutually exclusive (§5.6), so
/// neither constructor's reads are tracked, as ctor_uninit_member exempts
/// both. The crediting policy -- whole-entity stores and lifecycle calls,
/// plus any escape for a local aggregate's reads -- is stated in
/// ProfilesFramework.rst, "Reads of Uninitialized Objects" and
/// "Limitations".
static void runStdInitMemberReadChecks(Sema &S, const Decl *D,
                                       AnalysisDeclContext &,
                                       const CFGProfileEntry &Entry) {
  TrackedStorage Storage;
  const auto *Ctor = dyn_cast<CXXConstructorDecl>(D);
  if (Ctor && (Ctor->isDelegatingConstructor() || Ctor->getParent()->isUnion()))
    Ctor = nullptr;
  bool StarThisCopyTrusted = false;
  if (const CXXMethodDecl *MD =
          SemaProfiles::enclosingInstanceMethod(dyn_cast<DeclContext>(D))) {
    if (const CXXRecordDecl *RD = MD->getParent()->getDefinition())
      Storage.addObject(S, /*Base=*/nullptr, RD, /*TrackReads=*/Ctor != nullptr,
                        /*UnknownEntry=*/false);
    // A `[*this]` capture copy-constructs the whole object; a user-provided
    // copy constructor is opaque and trusted (P4222R2 §5.1), so the
    // capture's whole-object reads are appended only without one.
    if (Ctor)
      StarThisCopyTrusted = llvm::any_of(
          Ctor->getParent()->ctors(), [](const CXXConstructorDecl *C) {
            return C->isCopyConstructor() && C->isUserProvided();
          });
  }
  harvestTrackedLocals(S, D, Storage);
  if (Storage.Entities.empty())
    return;
  // The engine's own CFG, built only for a body with tracked storage:
  // exception edges, so a call inside a try block reaches its handler before
  // the assignment that follows it (P4222R2 §1.3: every path is considered),
  // and every expression as an element, so the extraction arms match any
  // statement class. The shared CFG the other analyses see is untouched (see
  // ProfilesFrameworkInternals.rst, "Pattern 2").
  AnalysisDeclContext InitAC(/*Mgr=*/nullptr, D);
  CFG::BuildOptions &Options = InitAC.getCFGBuildOptions();
  Options.PruneTriviallyFalseEdges = true;
  Options.AddEHEdges = true;
  Options.AddInitializers = true;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  Options.AddCXXDefaultInitExprInCtors = true;
  Options.setAllAlwaysAdd();
  CFG *cfg = InitAC.getCFG();
  if (!cfg)
    return;
  std::vector<SmallVector<DefAssignEvent, 4>> Events(cfg->getNumBlockIDs());
  SmallVector<BindingSite, 8> Sites;
  SmallVector<DestroySite, 4> DSites;
  extractStdInitEvents(S, D, *cfg, Storage, Ctor, StarThisCopyTrusted, Events,
                       Sites, DSites);
  // Nothing is assigned at function entry: written initializers write at
  // their CFGInitializer elements, a tracked local cannot be referenced
  // before its DeclStmt, a by-value parameter starts unassigned by design,
  // and a marked local's marker asserts it.
  SmallVector<PendingViolation, 8> Violations;
  std::vector<SmallVector<const Expr *, 2>> Offending = runDefiniteAssignment(
      *cfg, InitAC, Storage, Events, Sites, DSites, Violations);
  reportMemberReadsBeforeInit(S, InitAC, Offending, Storage, Entry.Name);
  reportFlowViolations(S, InitAC, Violations, Entry.Name);
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
