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

  // TODO: implement this

  return true;
}

void DoubleSharedPtrCheck::dumpPtrVars(const llvm::SmallPtrSet<const VarDecl *, 32> &PtrVars) {
  llvm::errs() << "PTR_VARS: ";
  for (const VarDecl *PtrVar : PtrVars)
    llvm::errs() << PtrVar->getQualifiedNameAsString() << " ";
  llvm::errs() << "\n";
}

} // namespace clang::tidy::misc