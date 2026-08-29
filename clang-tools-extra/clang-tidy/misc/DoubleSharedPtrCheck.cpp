//===----------------------------------------------------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

#include "DoubleSharedPtrCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "PointerStateAnalyzer.hpp"

using namespace clang::ast_matchers;

namespace clang::tidy::misc {

DoubleSharedPtrCheck::DoubleSharedPtrCheck(StringRef Name,
                                           ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context)
     {}

void DoubleSharedPtrCheck::storeOptions(ClangTidyOptions::OptionMap &Opts) {
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

  analyzeFunction(*Result.Context, Func, *Result.SourceManager);
}

bool DoubleSharedPtrCheck::analyzeFunction(ASTContext &Context,
                                           const FunctionDecl *Func,
                                           const SourceManager& SM) {
  //llvm::outs() << "analizing function " << Func->getNameAsString() << "\n";
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

  // Собираем все переменные-указатели в функции
  llvm::SmallPtrSet<const VarDecl *, 32> PtrVars;
  llvm::SmallPtrSet<const FieldDecl*, 32> PtrFields;

  for (const ParmVarDecl* PVD : Func->parameters()) {
    PtrVars.insert(PVD);
  }
  
  
  std::function<void(const Stmt*)> CollectPtrVars = 
      [&](const Stmt *S) {
    if (!S) return;
    
    if (const auto *DS = dyn_cast<DeclStmt>(S)) {
      for (const auto *D : DS->decls()) {
        if (const auto *VD = dyn_cast<VarDecl>(D)) {
          if (VD->getType()->isPointerType()) {
            PtrVars.insert(VD);
          }
        }
      }
    } else if (const auto *MS = dyn_cast<MemberExpr>(S)) {
      if (const auto* MD = MS->getMemberDecl()) {
        if (const auto *FD = dyn_cast<FieldDecl>(MD)) {
          if (FD->getType()->isPointerType()) {
            PtrFields.insert(FD);
          }
        }
      }
    }
    
    for (const auto *Child : S->children()) {
      CollectPtrVars(Child);
    }
  };
  
  CollectPtrVars(Body);

  //dumpPtrVars(PtrVars);
  //dumpPtrFields(PtrFields);

  const auto transitions = analyzeTransitions(PtrVars, PtrFields, *TheCFG);
  // for (const auto& [var, transList] : transitions) {
  //     llvm::outs() << "Var: " << describeLocation(var) << "\n";
  //     for (const auto& t : transList) {
  //         llvm::outs() << "  " << t.fromState << " -> " << t.toState << "\n";
  //     }
  // }

  for (const auto& [var, transList] : transitions) {
    for (const auto& t : transList) {
      if (t.fromState == t.toState && t.fromState == PS_SmartPtrWrapper) {
          const auto* SmartPtrCtor = dyn_cast<const CXXConstructExpr>(t.stmt);
          if (!SmartPtrCtor)
            continue;
          const Expr* PointerArg = SmartPtrCtor->getArg(0);
          if (!PointerArg)
            continue;
          const SourceLocation Loc = PointerArg->getBeginLoc();
          if (Loc.isInvalid())
            continue;
          diag(Loc, "passing a raw pointer '%0' to %1 constructor may cause double deletion")
              << getRawPointerDescription(PointerArg, Context) << SmartPtrCtor->getType();
      }
    }
  }

  return true;
}

std::string DoubleSharedPtrCheck::getRawPointerDescription(
    const Expr *PointerExpr, const ASTContext &Context) {
  const QualType ExprType = PointerExpr->getType();

  PrintingPolicy Policy(Context.getLangOpts());
  Policy.SuppressSpecifiers = false;
  Policy.SuppressTagKeyword = true;

  std::string Result = ExprType.getAsString(Policy);

  size_t Pos = Result.find(" *");
  while (Pos != std::string::npos) {
    Result.erase(Pos, 1); // remove the space
    Pos = Result.find(" *", Pos);
  }

  return Result;
}

void DoubleSharedPtrCheck::dumpPtrVars(const llvm::SmallPtrSet<const VarDecl *, 32> &PtrVars) {
  llvm::outs() << "PTR_VARS: ";
  for (const VarDecl *PtrVar : PtrVars)
    llvm::outs() << PtrVar->getQualifiedNameAsString() << " ";
  llvm::outs() << "\n";
}

void DoubleSharedPtrCheck::dumpPtrFields(const llvm::SmallPtrSet<const FieldDecl *, 32> &PtrFields) {
  llvm::outs() << "PTR_FIELDS: ";
  for (const FieldDecl *PtrField : PtrFields)
    llvm::outs() << PtrField->getQualifiedNameAsString() << " ";
  llvm::outs() << "\n";
}

} // namespace clang::tidy::misc