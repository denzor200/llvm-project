//===----------------------------------------------------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

#include "DoubleSharedPtrCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include <sstream>

using namespace clang::ast_matchers;

namespace clang::tidy::misc {

DoubleSharedPtrCheck::DoubleSharedPtrCheck(StringRef Name,
                                           ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      IgnoredFunctions(Options.get("IgnoredFunctions", "")) {}

void DoubleSharedPtrCheck::storeOptions(ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "IgnoredFunctions", IgnoredFunctions);
}

void DoubleSharedPtrCheck::registerMatchers(MatchFinder *Finder) {
  // Ищем функции, которые содержат потенциально опасные операции
  const auto DangerousFunction = functionDecl(
      hasAnyBody(anything()),  // hasAnyBody требует аргумент!
      anyOf(
          hasDescendant(binaryOperator(
              hasOperatorName("="),
              hasLHS(declRefExpr(to(varDecl().bind("ptr-var")))),
              hasRHS(cxxNewExpr().bind("new-expr")))
              .bind("assign-new")),
          hasDescendant(cxxConstructExpr().bind("shared-ctor"))
      )
  ).bind("func");

  Finder->addMatcher(DangerousFunction, this);
}

void DoubleSharedPtrCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
  if (!Func || !Func->hasBody())
    return;

  // Проверяем, не игнорируется ли эта функция
  if (!IgnoredFunctions.empty()) {
    std::string FuncName = Func->getNameAsString();
    if (IgnoredFunctions.find(FuncName) != std::string::npos)
      return;
  }

  analyzeFunction(*Result.Context, Func);
}

bool DoubleSharedPtrCheck::analyzeFunction(ASTContext &Context,
                                           const FunctionDecl *Func) {
  const auto *Body = Func->getBody();
  if (!Body)
    return false;

  // Настройка построения CFG
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  Options.AddInitializers = true;
  // Pruned был удален в новых версиях - убираем

  // Строим CFG - нужен const_cast, т.к. buildCFG принимает Stmt* (не const)
  auto TheCFG = CFG::buildCFG(Func, const_cast<Stmt *>(Body), &Context, Options);
  if (!TheCFG)
    return false;

  // Инициализируем контекст анализа
  FunctionAnalysisContext FuncCtx(const_cast<ASTContext *>(&Context), Func);
  FuncCtx.TheCFG = std::move(TheCFG);

  // Собираем все переменные-указатели в функции
  llvm::SmallPtrSet<const VarDecl *, 32> PtrVars;
  
  // Ручной обход AST вместо VisitStmt
  std::function<void(const Stmt*)> CollectPtrVars = 
      [&](const Stmt *S) {
    if (!S) return;
    
    if (const auto *DS = dyn_cast<DeclStmt>(S)) {
      for (const auto *D : DS->decls()) {
        if (const auto *VD = dyn_cast<VarDecl>(D)) {
          if (VD->getType()->isPointerType()) {
            PtrVars.insert(VD);
            FuncCtx.VarIndexMap[VD] = FuncCtx.GlobalStates.size();
            FuncCtx.GlobalStates.emplace_back(VD);
          }
        }
      }
    }
    
    // Рекурсивно обходим детей
    for (const auto *Child : S->children()) {
      CollectPtrVars(Child);
    }
  };
  
  CollectPtrVars(Body);

  // Анализируем каждый блок CFG
  for (const auto *Block : *FuncCtx.TheCFG) {
    llvm::DenseMap<const VarDecl *, VarStateInfo> BlockStates;
    // Инициализируем состояния для всех переменных
    for (const auto *VD : PtrVars) {
      BlockStates[VD] = VarStateInfo(VD);
    }
    
    analyzeBlock(Block, BlockStates, FuncCtx);
    
    // Обновляем глобальные состояния
    for (const auto &KV : BlockStates) {
      const auto *Var = KV.first;
      const VarStateInfo &State = KV.second;
      auto It = FuncCtx.VarIndexMap.find(Var);
      if (It != FuncCtx.VarIndexMap.end()) {
        FuncCtx.GlobalStates[It->second] = State;
      }
    }
  }

  return true;
}

void DoubleSharedPtrCheck::analyzeBlock(
    const CFGBlock *Block,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx) {
  
  for (const auto &Element : *Block) {
    if (auto Stmt = Element.getAs<CFGStmt>()) {
      const auto *S = Stmt->getStmt();

      if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (BO->getOpcode() == BO_Assign) {
          handleAssignment(BO, States, FuncCtx);
        }
      } else if (const auto *Ctor = dyn_cast<CXXConstructExpr>(S)) {
        if (isSharedPtrConstructor(Ctor)) {
          handleSharedPtrConstructor(Ctor, States, FuncCtx);
        }
      } else if (const auto *DS = dyn_cast<DeclStmt>(S)) {
        handleDeclStmt(DS, States, FuncCtx);
      } else if (const auto *RS = dyn_cast<ReturnStmt>(S)) {
        handleReturnStmt(RS, States, FuncCtx);
      }
    }
  }
}

void DoubleSharedPtrCheck::handleAssignment(
    const BinaryOperator *BO,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx) {
  
  const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS());
  if (!LHS)
    return;

  const auto *Var = dyn_cast<VarDecl>(LHS->getDecl());
  if (!Var || !Var->getType()->isPointerType())
    return;

  // Получаем или создаем состояние
  auto It = States.find(Var);
  if (It == States.end()) {
    It = States.insert(std::make_pair(Var, VarStateInfo(Var))).first;
  }

  VarStateInfo &State = It->second;
  const auto *RHS = BO->getRHS();

  if (isNewExpression(RHS)) {
    State.State = OWNING_PTR;
    State.CurrentNew = dyn_cast<CXXNewExpr>(RHS->IgnoreParenImpCasts());
    State.LastChangeLoc = BO->getBeginLoc();
    
  } else if (isNullPointer(RHS)) {
    State.State = PLAIN_PTR;
    State.CurrentNew = nullptr;
    State.LastChangeLoc = BO->getBeginLoc();
    
  } else if (const auto *DeclRef = dyn_cast<DeclRefExpr>(RHS->IgnoreParenImpCasts())) {
    if (const auto *OtherVar = dyn_cast<VarDecl>(DeclRef->getDecl())) {
      auto OtherIt = States.find(OtherVar);
      if (OtherIt != States.end()) {
        State.State = OtherIt->second.State;
        State.CurrentNew = OtherIt->second.CurrentNew;
        State.LastChangeLoc = BO->getBeginLoc();
      }
    }
  } else {
    State.State = PLAIN_PTR;
    State.CurrentNew = nullptr;
    State.LastChangeLoc = BO->getBeginLoc();
  }
}

void DoubleSharedPtrCheck::handleSharedPtrConstructor(
    const CXXConstructExpr *Ctor,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx) {
  
  if (Ctor->getNumArgs() != 1)
    return;

  const auto *Arg = Ctor->getArg(0)->IgnoreParenImpCasts();
  const auto *Var = getUnderlyingVarDecl(Arg);
  if (!Var || !Var->getType()->isPointerType())
    return;

  auto It = States.find(Var);
  if (It == States.end()) {
    It = States.insert(std::make_pair(Var, VarStateInfo(Var))).first;
  }

  VarStateInfo &State = It->second;

  if (State.State == SHARED_PTR && State.CurrentNew) {
    // 🚨 Двойное владение!
    reportDoubleOwnership(State, Ctor, FuncCtx);
  } else if (State.State == OWNING_PTR && State.CurrentNew) {
    State.State = SHARED_PTR;
    State.LastChangeLoc = Ctor->getBeginLoc();
  } else {
    State.State = SHARED_PTR;
    State.CurrentNew = nullptr;
    State.LastChangeLoc = Ctor->getBeginLoc();
  }
}

void DoubleSharedPtrCheck::handleDeclStmt(
    const DeclStmt *DS,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx) {
  
  for (const auto *D : DS->decls()) {
    if (const auto *VD = dyn_cast<VarDecl>(D)) {
      if (!VD->getType()->isPointerType())
        continue;

      if (const auto *Init = VD->getInit()) {
        auto It = States.find(VD);
        if (It == States.end()) {
          It = States.insert(std::make_pair(VD, VarStateInfo(VD))).first;
        }

        VarStateInfo &State = It->second;
        
        if (isNewExpression(Init)) {
          State.State = OWNING_PTR;
          State.CurrentNew = dyn_cast<CXXNewExpr>(Init->IgnoreParenImpCasts());
          State.LastChangeLoc = VD->getLocation();
        } else if (isNullPointer(Init)) {
          State.State = PLAIN_PTR;
          State.CurrentNew = nullptr;
          State.LastChangeLoc = VD->getLocation();
        }
      }
    }
  }
}

void DoubleSharedPtrCheck::handleReturnStmt(
    const ReturnStmt *RS,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx) {
  
  const auto *RetVal = RS->getRetValue();
  if (!RetVal)
    return;

  const auto *Var = getUnderlyingVarDecl(RetVal);
  if (!Var)
    return;

  auto It = States.find(Var);
  if (It != States.end()) {
    It->second.State = DEAD_PTR;
    It->second.LastChangeLoc = RS->getBeginLoc();
  }
}

bool DoubleSharedPtrCheck::isSharedPtrConstructor(const CXXConstructExpr *Ctor) {
  if (Ctor->getNumArgs() != 1)
    return false;

  const auto *CtorDecl = Ctor->getConstructor();
  if (!CtorDecl)
    return false;

  const auto *RD = CtorDecl->getParent();
  if (!RD)
    return false;

  if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {
    const auto *Template = CTSD->getSpecializedTemplate();
    if (Template && Template->getName() == "shared_ptr") {
      QualType ArgType = Ctor->getArg(0)->getType();
      return ArgType->isPointerType();
    }
  }

  return false;
}

const VarDecl *DoubleSharedPtrCheck::getUnderlyingVarDecl(const Expr *E) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    return dyn_cast<VarDecl>(DRE->getDecl());
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf) {
      return getUnderlyingVarDecl(UO->getSubExpr());
    }
  }

  return nullptr;
}

bool DoubleSharedPtrCheck::isNullPointer(const Expr *E) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();
  
  if (isa<CXXNullPtrLiteralExpr>(E))
    return true;

  if (const auto *IL = dyn_cast<IntegerLiteral>(E)) {
    return IL->getValue() == 0;
  }

  return false;
}

bool DoubleSharedPtrCheck::isNewExpression(const Expr *E) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();
  return isa<CXXNewExpr>(E);
}

void DoubleSharedPtrCheck::reportDoubleOwnership(
    const VarStateInfo &State,
    const CXXConstructExpr *SecondCtor,
    FunctionAnalysisContext &FuncCtx) {
  
  diag(SecondCtor->getBeginLoc(),
       "raw pointer from 'new' used to initialize multiple std::shared_ptr objects");
  
  diag(State.LastChangeLoc, "first shared_ptr initialization here",
       DiagnosticIDs::Note);
  
  if (State.CurrentNew) {
    diag(State.CurrentNew->getBeginLoc(),
         "memory allocated here", DiagnosticIDs::Note);
  }
  
  diag(SecondCtor->getBeginLoc(),
       "consider using std::shared_ptr::reset() or "
       "reassigning the raw pointer to nullptr before second use",
       DiagnosticIDs::Note);
}

} // namespace clang::tidy::misc