//===----------------------------------------------------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

#include "DoubleSharedPtrCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Casting.h"
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
  const auto IsSharedPtr = hasAnyName(SharedPointers);
  const auto IsUniquePtr = hasAnyName(UniquePointers);
  const auto IsSmartPtr = anyOf(IsSharedPtr, IsUniquePtr);

  const auto IsSharedPtrRecord = cxxRecordDecl(IsSharedPtr);
  const auto IsUniquePtrRecord = cxxRecordDecl(IsUniquePtr);
  const auto IsSmartPtrRecord = cxxRecordDecl(IsSmartPtr);

  auto ResetCallMatcher = cxxMemberCallExpr(
      on(hasType(hasUnqualifiedDesugaredType(recordType(
          hasDeclaration(classTemplateSpecializationDecl(IsSmartPtr)))))),
      callee(cxxMethodDecl(ofClass(IsSmartPtrRecord),
                           hasName("reset"))
                 ));

  auto SmartPtrGetCallMatcher = cxxMemberCallExpr(callee(cxxMethodDecl(hasName("get"))), on(hasType(hasUnqualifiedDesugaredType(recordType(
          hasDeclaration(classTemplateSpecializationDecl(IsSmartPtr)))))));

  auto SmartPtrConstructorMatcher = cxxConstructExpr(
    hasDeclaration(
        cxxConstructorDecl(ofClass(IsSmartPtrRecord))),
    hasArgument(0, anyOf(ignoringParenCasts(cxxThisExpr()), ignoringParenCasts(SmartPtrGetCallMatcher)))).bind("ctor-with-this-expr");

  auto ResetCallWithThisMatcher = cxxMemberCallExpr(
      on(hasType(hasUnqualifiedDesugaredType(recordType(
          hasDeclaration(classTemplateSpecializationDecl(IsSmartPtr)))))),
      callee(cxxMethodDecl(ofClass(IsSmartPtrRecord),
                           hasName("reset"))),
      hasArgument(0, anyOf(ignoringParenCasts(cxxThisExpr()), ignoringParenCasts(SmartPtrGetCallMatcher)))).bind("reset-with-this-expr");

  // Ищем функции, которые содержат потенциально опасные операции
  // TODO: rename to PotentiallyDangerousFunction
  const auto DangerousFunction = functionDecl(
      hasAnyBody(anything()),
      anyOf(
          hasDescendant(cxxNewExpr()),
          hasDescendant(ResetCallMatcher),
          hasDescendant(cxxConstructExpr(hasDeclaration(
            cxxConstructorDecl(ofClass(IsSmartPtrRecord)))))

      )
  ).bind("func");

  Finder->addMatcher(DangerousFunction, this);
  Finder->addMatcher(SmartPtrConstructorMatcher, this);
  Finder->addMatcher(ResetCallWithThisMatcher, this);
}

void DoubleSharedPtrCheck::check(const MatchFinder::MatchResult &Result) {
  // TODO: rename to "dangerous-ctor" and "dangerous-reset"
  const auto *CtorWithThisExpr = Result.Nodes.getNodeAs<CXXConstructExpr>("ctor-with-this-expr");
  const auto *ResetWithThisExpr = Result.Nodes.getNodeAs<CXXMemberCallExpr>("reset-with-this-expr");
  if (CtorWithThisExpr)
    emitDiagnostic(*Result.Context, CtorWithThisExpr);
  else if (ResetWithThisExpr)
    emitDiagnostic(*Result.Context, ResetWithThisExpr);
  else
    checkFlowSensitive(Result);
}

void DoubleSharedPtrCheck::checkFlowSensitive(const MatchFinder::MatchResult &Result) {
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
          if (const auto* E = dyn_cast<const Expr>(t.stmt))
            emitDiagnostic(Context, E);
      }
    }
  }

  return true;
}

void DoubleSharedPtrCheck::emitDiagnostic(ASTContext &Context, const Expr* ConstructorOrMember) {
  if (const auto* SmartPtrCtor = dyn_cast<const CXXConstructExpr>(ConstructorOrMember)) {
    const Expr* PointerArg = SmartPtrCtor->getArg(0);
    if (!PointerArg)
      return;
    const SourceLocation Loc = PointerArg->getBeginLoc();
    if (Loc.isInvalid())
      return;
    diag(Loc, "passing a raw pointer '%0' to %1 constructor may cause double deletion")
        << getRawPointerDescription(PointerArg, Context) << SmartPtrCtor->getType();
  } else if (const auto* ResetCall = dyn_cast<const CXXMemberCallExpr>(ConstructorOrMember)) {
    const Expr* PointerArg = ResetCall->getArg(0);
    if (!PointerArg)
      return;
    const SourceLocation Loc = PointerArg->getBeginLoc();
    if (Loc.isInvalid())
      return;
    diag(Loc, "passing a raw pointer '%0' to '%1::reset' may cause double deletion")
        << getRawPointerDescription(PointerArg, Context) << getSmartPointerDescription(ResetCall->getMethodDecl()->getParent(), Context);
  }
}

std::string DoubleSharedPtrCheck::getSmartPointerDescription(
    const CXXRecordDecl *RecordDecl, const ASTContext &Context) {
  const PrintingPolicy Policy = Context.getPrintingPolicy();

  std::string Result;
  llvm::raw_string_ostream OS(Result);
  RecordDecl->getNameForDiagnostic(OS, Policy, /*Qualified=*/true);

  return Result;
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