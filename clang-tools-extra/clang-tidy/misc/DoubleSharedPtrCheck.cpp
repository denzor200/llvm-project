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

namespace {
std::string printSourceLocation(clang::SourceLocation Loc, 
                         const clang::SourceManager& SM) {
    if (Loc.isInvalid()) {
      return "<invalid>";
    }

      // Получить полное имя файла, строку и колонку
      StringRef filepath = SM.getFilename(Loc);
      unsigned line = SM.getSpellingLineNumber(Loc);
      unsigned column = SM.getSpellingColumnNumber(Loc);
      StringRef filename = llvm::sys::path::filename(filepath);

      return filename.str() + ":" + std::to_string(line) + ":" +
             std::to_string(column);
}
}

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

  analyzeFunction(*Result.Context, Func, *Result.SourceManager);
}

bool DoubleSharedPtrCheck::analyzeFunction(ASTContext &Context,
                                           const FunctionDecl *Func,
                                           const SourceManager& SM) {
  llvm::errs() << "analizing function " << Func->getNameAsString() << "\n";
  const auto *Body = Func->getBody();
  if (!Body)
    return false;

  // Настройка построения CFG
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  Options.AddInitializers = true;

  auto TheCFG = CFG::buildCFG(Func, const_cast<Stmt *>(Body), &Context, Options);
  if (!TheCFG)
    return false;

  // Инициализируем контекст анализа
  FunctionAnalysisContext FuncCtx(const_cast<ASTContext *>(&Context), Func);
  FuncCtx.TheCFG = std::move(TheCFG);

  // Собираем все переменные-указатели в функции
  llvm::SmallPtrSet<const VarDecl *, 32> PtrVars;
  
  // TODO: arguments must be collected too
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
    
    for (const auto *Child : S->children()) {
      CollectPtrVars(Child);
    }
  };
  
  CollectPtrVars(Body);

  dumpPtrVars(PtrVars);

  // Инициализируем состояния для каждого блока
  llvm::DenseMap<const CFGBlock *, llvm::DenseMap<const VarDecl *, VarStateInfo>> BlockStates;
  
  // Начальное состояние - все переменные PLAIN_PTR
  llvm::DenseMap<const VarDecl *, VarStateInfo> InitialState;
  for (const auto *VD : PtrVars) {
    InitialState[VD] = VarStateInfo(VD);
  }
  
  // Итеративный анализ до достижения фиксированной точки
  bool Changed = true;
  int Iteration = 0;
  const int MaxIterations = 100; // Предотвращаем бесконечный цикл
  
  while (Changed && Iteration < MaxIterations) {
    Changed = false;
    Iteration++;
    
    for (const auto *Block : *FuncCtx.TheCFG) {
      // Для entry блока используем начальное состояние
      llvm::DenseMap<const VarDecl *, VarStateInfo> BlockInState;
      
      if (Block == &FuncCtx.TheCFG->getEntry()) {
        BlockInState = InitialState;
      } else {
        // Сливаем состояния всех предшественников
        bool FirstPred = true;
        for (CFGBlock::const_pred_iterator I = Block->pred_begin(), 
                                           E = Block->pred_end(); 
             I != E; ++I) {
          const CFGBlock *Pred = *I;
          if (!Pred) continue; // Нулевой указатель для exit блока
          
          auto It = BlockStates.find(Pred);
          if (It != BlockStates.end()) {
            if (FirstPred) {
              BlockInState = It->second;
              FirstPred = false;
            } else {
              // Сливаем состояния - берем объединение (meet)
              mergeStates(BlockInState, It->second);
            }
          }
        }
        
        // Если нет предшественников (недостижимый блок)
        if (FirstPred) {
          BlockInState = InitialState;
        }
      }
      
      // Анализируем блок с этим состоянием
      auto BlockOutState = BlockInState;
      analyzeBlock(Block, BlockOutState, FuncCtx, SM);
      
      // Проверяем, изменилось ли состояние блока
      auto It = BlockStates.find(Block);
      if (It == BlockStates.end() || !statesEqual(It->second, BlockOutState)) {
        Changed = true;
        BlockStates[Block] = BlockOutState;
      }
    }
  }
  
    // dumpState для отладки (можно закомментировать)
  // for (const auto *Block : *FuncCtx.TheCFG) {
  //   auto It = BlockStates.find(Block);
  //   if (It != BlockStates.end()) {
  //     std::string Label = "Block " + std::to_string(Block->getBlockID());
  //     dumpState(It->second, Label.c_str());
  //   }
  // }

  // После достижения фиксированной точки, проверяем все блоки на наличие ошибок
  for (const auto *Block : *FuncCtx.TheCFG) {
    auto It = BlockStates.find(Block);
    if (It != BlockStates.end()) {
      // Повторно анализируем с финальными состояниями для генерации диагностик
      analyzeBlockForDiagnostics(Block, It->second, FuncCtx, SM);
    }
  }

  return true;
}

// Слияние состояний (meet operation)
void DoubleSharedPtrCheck::mergeStates(
    llvm::DenseMap<const VarDecl *, VarStateInfo> &Dest,
    const llvm::DenseMap<const VarDecl *, VarStateInfo> &Src) {
  
  for (const auto &KV : Src) {
    const auto *Var = KV.first;
    const VarStateInfo &SrcState = KV.second;
    
    auto DestIt = Dest.find(Var);
    if (DestIt == Dest.end()) {
      Dest[Var] = SrcState;
      continue;
    }
    
    VarStateInfo &DestState = DestIt->second;
    
    // Если состояния различаются, объединяем их в наиболее общее
    if (DestState.State != SrcState.State) {
      // Берем менее строгое состояние
      // Порядок: PLAIN_PTR < OWNING_PTR < SHARED_PTR < DEAD_PTR
      if (DestState.State > SrcState.State) {
        DestState.State = SrcState.State;
      }
      // Если состояния различаются, CurrentNew не определен
      DestState.CurrentNew = nullptr;
    } else {
      // Если состояния одинаковы, но разные new выражения, тоже не определено
      if (DestState.CurrentNew != SrcState.CurrentNew) {
        DestState.CurrentNew = nullptr;
      }
    }
    
    // Берем наиболее раннюю позицию изменения
    if (SrcState.LastChangeLoc.isValid() &&
        (DestState.LastChangeLoc.isInvalid() ||
         DestState.LastChangeLoc > SrcState.LastChangeLoc)) {
      DestState.LastChangeLoc = SrcState.LastChangeLoc;
    }
  }
}

// Проверка равенства состояний
bool DoubleSharedPtrCheck::statesEqual(
    const llvm::DenseMap<const VarDecl *, VarStateInfo> &A,
    const llvm::DenseMap<const VarDecl *, VarStateInfo> &B) {
  
  if (A.size() != B.size())
    return false;
  
  for (const auto &KV : A) {
    const auto *Var = KV.first;
    const VarStateInfo &StateA = KV.second;
    
    auto It = B.find(Var);
    if (It == B.end())
      return false;
    
    const VarStateInfo &StateB = It->second;
    if (StateA.State != StateB.State ||
        StateA.CurrentNew != StateB.CurrentNew) {
      return false;
    }
  }
  
  return true;
}

// Анализ блока с генерацией диагностик
void DoubleSharedPtrCheck::analyzeBlockForDiagnostics(
    const CFGBlock *Block,
    const llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx, const SourceManager& SM) {
  
  // Копируем состояния для модификации внутри блока
  auto LocalStates = States;
  
  for (const auto &Element : *Block) {
    if (auto Stmt = Element.getAs<CFGStmt>()) {
      const auto *S = Stmt->getStmt();

      if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (BO->getOpcode() == BO_Assign) {
          handleAssignment(BO, LocalStates, FuncCtx, SM);
        }
      } else if (const auto *Ctor = dyn_cast<CXXConstructExpr>(S)) {
        if (isSharedPtrConstructor(Ctor)) {
          handleSharedPtrConstructor(Ctor, LocalStates, FuncCtx, SM);
        }
      } else if (const auto *DS = dyn_cast<DeclStmt>(S)) {
        handleDeclStmt(DS, LocalStates, FuncCtx, SM);
      }
    }
  }
}

void DoubleSharedPtrCheck::analyzeBlock(
    const CFGBlock *Block,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx, const SourceManager& SM) {
  
  for (const auto &Element : *Block) {
    if (auto Stmt = Element.getAs<CFGStmt>()) {
      const auto *S = Stmt->getStmt();

      if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (BO->getOpcode() == BO_Assign) {
          handleAssignment(BO, States, FuncCtx, SM);
        }
      } else if (const auto *Ctor = dyn_cast<CXXConstructExpr>(S)) {
        if (isSharedPtrConstructor(Ctor)) {
          handleSharedPtrConstructor(Ctor, States, FuncCtx, SM);
        }
      } else if (const auto *DS = dyn_cast<DeclStmt>(S)) {
        handleDeclStmt(DS, States, FuncCtx, SM);
      } else if (const auto *RS = dyn_cast<ReturnStmt>(S)) {
        handleReturnStmt(RS, States, FuncCtx, SM);
      }
    }
  }
}

void DoubleSharedPtrCheck::handleAssignment(
    const BinaryOperator *BO,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx, const SourceManager& SM) {
  
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
    dumpStateSwitch(State, SM);
    
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
    dumpStateSwitch(State, SM);
  }
}

void DoubleSharedPtrCheck::handleSharedPtrConstructor(
    const CXXConstructExpr *Ctor,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx, const SourceManager& SM) {
  
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
    dumpStateSwitch(State, SM);
  } else {
    State.State = SHARED_PTR;
    State.CurrentNew = nullptr;
    State.LastChangeLoc = Ctor->getBeginLoc();
    dumpStateSwitch(State, SM);
  }
}

void DoubleSharedPtrCheck::handleDeclStmt(
    const DeclStmt *DS,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx, const SourceManager& SM) {
  
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
          dumpStateSwitch(State, SM);
        } else if (isNullPointer(Init)) {
          State.State = PLAIN_PTR;
          State.CurrentNew = nullptr;
          State.LastChangeLoc = VD->getLocation();
          dumpStateSwitch(State, SM);
        }
      }
    }
  }
}

void DoubleSharedPtrCheck::handleReturnStmt(
    const ReturnStmt *RS,
    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    FunctionAnalysisContext &FuncCtx, const SourceManager& SM) {
  
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
    dumpStateSwitch(It->second, SM);
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

void DoubleSharedPtrCheck::dumpState(
    const llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
    const char *Label) {
  llvm::errs() << "=== " << Label << " ===\n";
  for (const auto &KV : States) {
    const auto *Var = KV.first;
    const VarStateInfo &State = KV.second;
    llvm::errs() << "  " << Var->getName() << ": ";
    switch (State.State) {
      case PLAIN_PTR: llvm::errs() << "PLAIN_PTR"; break;
      case OWNING_PTR: llvm::errs() << "OWNING_PTR"; break;
      case SHARED_PTR: llvm::errs() << "SHARED_PTR"; break;
      case DEAD_PTR: llvm::errs() << "DEAD_PTR"; break;
    }
    llvm::errs() << "\n";
  }
}

void DoubleSharedPtrCheck::dumpStateSwitch(const VarStateInfo& SI, const SourceManager& SM) {
  StringRef StateName;
  switch (SI.State) {
    case PLAIN_PTR: StateName = "PLAIN_PTR"; break;
    case OWNING_PTR: StateName = "OWNING_PTR"; break;
    case SHARED_PTR: StateName = "SHARED_PTR"; break;
    case DEAD_PTR: StateName = "DEAD_PTR"; break;
  }
  llvm::errs() << "[" << reinterpret_cast<const void*>(&SI) << "] " << SI.Var->getNameAsString() << " switched to " << StateName << " at "
               << printSourceLocation(SI.LastChangeLoc, SM) << "\n";
}

void DoubleSharedPtrCheck::dumpPtrVars(const llvm::SmallPtrSet<const VarDecl *, 32> &PtrVars) {
  llvm::errs() << "PTR_VARS: ";
  for (const VarDecl *PtrVar : PtrVars)
    llvm::errs() << PtrVar->getQualifiedNameAsString() << " ";
  llvm::errs() << "\n";
}

} // namespace clang::tidy::misc