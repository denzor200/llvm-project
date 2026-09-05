//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MultipleSmartPtrOwnersCheck.h"

#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/Analyses/CFGReachabilityAnalysis.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallPtrSet.h"

#include "../utils/ExprSequence.h"
#include <optional>

using namespace clang::ast_matchers;
using namespace clang::tidy::utils;

namespace clang::tidy::bugprone {

namespace {

/// A matcher fragment for the constructor of an owning smart pointer that
/// takes a raw pointer, e.g. `std::unique_ptr<T>(p)` / `std::shared_ptr<T>(p)`.
/// Copy/move constructors (whose argument is another smart pointer, not a
/// raw pointer) never match `hasType(pointerType())` on the referenced
/// variable, so they're naturally excluded.
static auto smartPtrCtorTakingRawPointer() {
  // TODO: smart pointer names must be loaded from options
  return cxxConstructExpr(hasDeclaration(cxxConstructorDecl(ofClass(
      cxxRecordDecl(hasAnyName("::std::unique_ptr", "::std::shared_ptr"))))));
}

static auto smartPtrResetTakingRawPointer() {
  static const auto IsSharedPtr = hasAnyName("::std::shared_ptr");
  static const auto IsUniquePtr = hasAnyName("::std::unique_ptr");
  static const auto IsSmartPtr = anyOf(IsSharedPtr, IsUniquePtr);
  static const auto IsSmartPtrRecord = cxxRecordDecl(IsSmartPtr);

  return cxxMemberCallExpr(
        on(hasType(hasUnqualifiedDesugaredType(recordType(
            hasDeclaration(classTemplateSpecializationDecl(IsSmartPtr)))))),
        callee(cxxMethodDecl(ofClass(IsSmartPtrRecord), hasName("reset"))));
}

/// Contains information about a second "ownership transfer" of a raw
/// pointer that already belongs to a smart pointer.
struct OwnershipTransfer {
  // The DeclRefExpr used as the argument of the second (problematic)
  // smart-pointer construction.
  const DeclRefExpr *DeclRef;

  // TODO: change the comment
  // The CXXConstructExpr of that second construction (used to print the
  // smart-pointer type in the diagnostic).
  const Expr *ConstructOrResetExpr;

  // Is the order in which the two constructions are evaluated undefined?
  bool EvaluationOrderUndefined = false;

  // Does the second transfer happen in a later loop iteration than the
  // first one?
  bool UseHappensInLaterLoopIteration = false;
};

/// Finds a second ownership transfer of a raw-pointer variable that already
/// owns a smart pointer (and maintains state required by the various
/// internal helper functions). Structured the same way as
/// `UseAfterMoveFinder` in `UseAfterMoveCheck.cpp`.
class OwnershipTransferFinder {
public:
  explicit OwnershipTransferFinder(ASTContext *TheContext)
      : Context(TheContext) {}

  // Within the given code block, finds the first ownership transfer of
  // 'RawPtrVar' that occurs after 'FirstTransfer' (the construct expression
  // that first handed the pointer to a smart pointer). Returns std::nullopt
  // if none is found.
  std::optional<OwnershipTransfer> find(Stmt *CodeBlock,
                                        const Expr *FirstTransfer,
                                        const DeclRefExpr *RawPtrVar);

private:
  std::optional<OwnershipTransfer> findInternal(const CFGBlock *Block,
                                                const Expr *FirstTransfer,
                                                const ValueDecl *RawPtrVar);

  void getOwnershipTransfers(
      const CFGBlock *Block, const Decl *RawPtrVar,
      SmallVectorImpl<std::pair<const DeclRefExpr *, const Expr *>>
          *Transfers);

  void getReinits(const CFGBlock *Block, const ValueDecl *RawPtrVar,
                  llvm::SmallPtrSetImpl<const Stmt *> *Stmts);

  ASTContext *Context;
  std::unique_ptr<ExprSequence> Sequence;
  std::unique_ptr<StmtToBlockMap> BlockMap;
  llvm::SmallPtrSet<const CFGBlock *, 8> Visited;
};

} // namespace

std::optional<OwnershipTransfer>
OwnershipTransferFinder::find(Stmt *CodeBlock, const Expr *FirstTransfer,
                              const DeclRefExpr *RawPtrVar) {
  // Same rationale as UseAfterMoveCheck: build the CFG manually (rather than
  // via an AnalysisDeclContext) so this also works for lambda bodies, and
  // include implicit/temporary destructors so [[noreturn]] destructors are
  // handled correctly by the control-flow analysis.
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  std::unique_ptr<CFG> TheCFG =
      CFG::buildCFG(nullptr, CodeBlock, Context, Options);
  if (!TheCFG)
    return std::nullopt;

  Sequence = std::make_unique<ExprSequence>(TheCFG.get(), CodeBlock, Context);
  BlockMap = std::make_unique<StmtToBlockMap>(TheCFG.get(), Context);
  Visited.clear();

  const CFGBlock *FirstBlock = BlockMap->blockContainingStmt(FirstTransfer);
  if (!FirstBlock) {
    // Can happen if FirstTransfer is in a constructor initializer.
    FirstBlock = &TheCFG->getEntry();
  }

  auto TheTransfer =
      findInternal(FirstBlock, FirstTransfer, RawPtrVar->getDecl());

  if (TheTransfer) {
    if (const CFGBlock *UseBlock =
            BlockMap->blockContainingStmt(TheTransfer->DeclRef)) {
      // Same reasoning as UseAfterMoveCheck: figure out whether the second
      // transfer can only happen in a later loop iteration than the first.
      CFGReverseBlockReachabilityAnalysis CFA(*TheCFG);
      TheTransfer->UseHappensInLaterLoopIteration =
          UseBlock == FirstBlock ? Visited.contains(UseBlock)
                                 : CFA.isReachable(UseBlock, FirstBlock);
    }
  }
  return TheTransfer;
}

std::optional<OwnershipTransfer>
OwnershipTransferFinder::findInternal(const CFGBlock *Block,
                                      const Expr *FirstTransfer,
                                      const ValueDecl *RawPtrVar) {
  if (Visited.contains(Block))
    return std::nullopt;

  // Mark the block as visited, except if this is the block containing the
  // very first transfer and it's being visited for the first time -- mirrors
  // UseAfterMoveFinder's handling of the initial std::move() block.
  if (!FirstTransfer)
    Visited.insert(Block);

  SmallVector<std::pair<const DeclRefExpr *, const Expr *>, 1>
      Transfers;
  llvm::SmallPtrSet<const Stmt *, 1> Reinits;
  getOwnershipTransfers(Block, RawPtrVar, &Transfers);
  getReinits(Block, RawPtrVar, &Reinits);

  // A reassignment of the raw pointer (e.g. `ptr = new B();` or
  // `ptr = nullptr;`) only protects a transfer if it doesn't itself
  // potentially happen after the first transfer -- otherwise we can't be
  // sure the pointer was reset before the second construction ran.
  SmallVector<const Stmt *, 1> ReinitsToDelete;
  for (const Stmt *Reinit : Reinits)
    if (FirstTransfer && Sequence->potentiallyAfter(FirstTransfer, Reinit))
      ReinitsToDelete.push_back(Reinit);
  for (const Stmt *Reinit : ReinitsToDelete)
    Reinits.erase(Reinit);

  for (const auto &[DeclRef, ConstructOrResetExpr] : Transfers) {
    // Never match a transfer against itself.
    if (ConstructOrResetExpr == FirstTransfer)
      continue;

    if (!FirstTransfer ||
        Sequence->potentiallyAfter(ConstructOrResetExpr, FirstTransfer)) {
      // Does this transfer have a "saving" reinit -- i.e. one that
      // definitely (not just potentially) happens before it?
      bool HaveSavingReinit = false;
      for (const Stmt *Reinit : Reinits)
        if (!Sequence->potentiallyAfter(Reinit, ConstructOrResetExpr))
          HaveSavingReinit = true;

      if (!HaveSavingReinit) {
        OwnershipTransfer Result;
        Result.DeclRef = DeclRef;
        Result.ConstructOrResetExpr = ConstructOrResetExpr;

        // Same order-of-evaluation caveat as UseAfterMoveCheck: if the
        // first transfer could also potentially come after this one, the
        // relative order between them is unspecified.
        Result.EvaluationOrderUndefined =
            FirstTransfer != nullptr &&
            Sequence->potentiallyAfter(FirstTransfer, ConstructOrResetExpr);

        return Result;
      }
    }
  }

  // If the pointer wasn't reassigned in this block, keep looking in
  // successor blocks (branches, loop bodies, etc.).
  if (Reinits.empty()) {
    for (const auto &Succ : Block->succs()) {
      if (Succ) {
        if (auto Found = findInternal(Succ, nullptr, RawPtrVar))
          return Found;
      }
    }
  }

  return std::nullopt;
}

void OwnershipTransferFinder::getOwnershipTransfers(
    const CFGBlock *Block, const Decl *RawPtrVar,
    SmallVectorImpl<std::pair<const DeclRefExpr *, const Expr *>>
        *Transfers) {
  Transfers->clear();

  const auto DeclRefMatcher =
      declRefExpr(hasDeclaration(equalsNode(RawPtrVar))).bind("declref");
  const auto TransferMatcher = anyOf(
      cxxConstructExpr(smartPtrCtorTakingRawPointer(),
                       hasArgument(0, ignoringParenImpCasts(DeclRefMatcher)))
          .bind("construct"),
      cxxMemberCallExpr(smartPtrResetTakingRawPointer(),
                       hasArgument(0, ignoringParenImpCasts(DeclRefMatcher)))
          .bind("reset"));

  for (const auto &Elem : *Block) {
    std::optional<CFGStmt> S = Elem.getAs<CFGStmt>();
    if (!S)
      continue;

    const SmallVector<BoundNodes, 1> Matches =
        match(findAll(expr(TransferMatcher)), *S->getStmt(), *Context);

    for (const auto &Match : Matches) {
      const auto *DeclRef = Match.getNodeAs<DeclRefExpr>("declref");
      const auto *ConstructExpr =
          Match.getNodeAs<CXXConstructExpr>("construct");
      const auto* ResetExpr =
          Match.getNodeAs<CXXMemberCallExpr>("reset");
      const Expr* ConstructOrResetExpr = ConstructExpr ? static_cast<const Expr*>(ConstructExpr) : static_cast<const Expr*>(ResetExpr);
      if (DeclRef && ConstructOrResetExpr &&
          BlockMap->blockContainingStmt(DeclRef) == Block)
        Transfers->push_back({DeclRef, ConstructOrResetExpr});
    }
  }

  llvm::sort(*Transfers, [](const auto &A, const auto &B) {
    return A.first->getExprLoc() < B.first->getExprLoc();
  });
}

void OwnershipTransferFinder::getReinits(
    const CFGBlock *Block, const ValueDecl *RawPtrVar,
    llvm::SmallPtrSetImpl<const Stmt *> *Stmts) {
  Stmts->clear();

  // Reassigning the raw-pointer variable itself (to a new object, or to
  // null) means it's no longer the same pointer, so any smart-pointer
  // construction after this point refers to a different object and is not
  // a double-deletion risk. Redeclaring the variable inside the block (e.g.
  // via a shadowing DeclStmt in a nested scope) has the same effect.
  const auto DeclRefMatcher =
      declRefExpr(hasDeclaration(equalsNode(RawPtrVar)));
  const auto ReinitMatcher =
      stmt(anyOf(binaryOperation(hasOperatorName("="),
                                 hasLHS(ignoringParenImpCasts(DeclRefMatcher))),
                 declStmt(hasDescendant(equalsNode(RawPtrVar)))))
          .bind("reinit");

  for (const auto &Elem : *Block) {
    std::optional<CFGStmt> S = Elem.getAs<CFGStmt>();
    if (!S)
      continue;

    const SmallVector<BoundNodes, 1> Matches =
        match(findAll(ReinitMatcher), *S->getStmt(), *Context);

    for (const auto &Match : Matches) {
      const auto *TheStmt = Match.getNodeAs<Stmt>("reinit");
      if (TheStmt && BlockMap->blockContainingStmt(TheStmt) == Block)
        Stmts->insert(TheStmt);
    }
  }
}

static void emitDiagnostic(const ASTContext *Context,
                           const OwnershipTransfer &Transfer,
                           ClangTidyCheck *Check) {
  const SourceLocation UseLoc = Transfer.DeclRef->getExprLoc();
  if (const auto *SmartPtrCtor = dyn_cast<const CXXConstructExpr>(Transfer.ConstructOrResetExpr)) {
      
    Check->diag(UseLoc,
              "passing a raw pointer %0 to %1 constructor may cause "
              "double deletion")
        << Transfer.DeclRef->getType() << SmartPtrCtor->getType();

  } else if (const auto *ResetCall = dyn_cast<const CXXMemberCallExpr>(Transfer.ConstructOrResetExpr)) {
  
    Check->diag(UseLoc,
              "passing a raw pointer %0 to %1 reset method may cause double deletion")
        << Transfer.DeclRef->getType() << ResetCall->getObjectType();

  }

  if (Transfer.EvaluationOrderUndefined) {
    Check->diag(UseLoc,
               "the two smart-pointer constructions are unsequenced, i.e. "
               "there is no guarantee about the order in which they are "
               "evaluated",
               DiagnosticIDs::Note);
  } else if (Transfer.UseHappensInLaterLoopIteration) {
    Check->diag(UseLoc,
               "the second construction happens in a later loop iteration "
               "than the first",
               DiagnosticIDs::Note);
  }
}

void MultipleSmartPtrOwnersCheck::registerMatchers(MatchFinder *Finder) {
  const auto RawPtrArg =
      declRefExpr(to(varDecl(hasType(pointerType())).bind("raw-ptr-var")))
          .bind("arg");

  // Mirrors the shape of UseAfterMoveCheck's matcher: find the construct
  // expression, then walk up to whichever kind of body contains it so we
  // know what to build a CFG for.
  Finder->addMatcher(
      traverse(
          TK_AsIs,
          expr(anyOf(
            cxxConstructExpr(
                smartPtrCtorTakingRawPointer(),
                hasArgument(0, ignoringParenImpCasts(RawPtrArg)),
                anyOf(hasAncestor(compoundStmt(
                          hasParent(lambdaExpr().bind("containing-lambda")))),
                      hasAncestor(functionDecl(
                          anyOf(cxxConstructorDecl().bind("containing-ctor"),
                                functionDecl().bind("containing-func")))))),
            cxxMemberCallExpr(
              smartPtrResetTakingRawPointer(),
              hasArgument(0, ignoringParenImpCasts(RawPtrArg)),
              anyOf(hasAncestor(compoundStmt(
                          hasParent(lambdaExpr().bind("containing-lambda")))),
                      hasAncestor(functionDecl(
                          anyOf(cxxConstructorDecl().bind("containing-ctor"),
                                functionDecl().bind("containing-func")))))
              )
              )).bind("transfer-call")),
      this);
}

void MultipleSmartPtrOwnersCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *ContainingCtor =
      Result.Nodes.getNodeAs<CXXConstructorDecl>("containing-ctor");
  const auto *ContainingLambda =
      Result.Nodes.getNodeAs<LambdaExpr>("containing-lambda");
  const auto *ContainingFunc =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-func");
  const auto *TransferCall =
      Result.Nodes.getNodeAs<Expr>("transfer-call");
  const auto *Arg = Result.Nodes.getNodeAs<DeclRefExpr>("arg");

  if (!TransferCall || !Arg)
    return;

  // Only locals/parameters are in scope for this per-function CFG analysis.
  if (!Arg->getDecl()->getDeclContext()->isFunctionOrMethod())
    return;

  Stmt *CodeBlock = nullptr;
  if (ContainingCtor)
    CodeBlock = ContainingCtor->getBody();
  else if (ContainingLambda)
    CodeBlock = ContainingLambda->getBody();
  else if (ContainingFunc)
    CodeBlock = ContainingFunc->getBody();

  if (!CodeBlock)
    return;

  OwnershipTransferFinder Finder(Result.Context);
  if (auto Transfer = Finder.find(CodeBlock, TransferCall, Arg))
    emitDiagnostic(Result.Context, *Transfer, this);
}

} // namespace clang::tidy::bugprone