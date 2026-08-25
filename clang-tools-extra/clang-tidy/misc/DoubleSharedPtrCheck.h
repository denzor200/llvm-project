//===----------------------------------------------------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_DOUBLESHAREDPTRCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_DOUBLESHAREDPTRCHECK_H

#include "../ClangTidyCheck.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace clang::tidy::misc {

/// Detects when a raw pointer from 'new' is used to initialize
/// multiple std::shared_ptr objects, leading to double ownership.
class DoubleSharedPtrCheck : public ClangTidyCheck {
public:
  DoubleSharedPtrCheck(StringRef Name, ClangTidyContext *Context);
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
  void storeOptions(ClangTidyOptions::OptionMap &Opts) override;

private:
  // Состояния указателя в конечном автомате
  enum PtrState {
    PLAIN_PTR,     // Обычный указатель (не владеет памятью)
    OWNING_PTR,    // Владеет памятью от new, но не передан в shared_ptr
    SHARED_PTR,    // Передан в shared_ptr, но продолжает хранить указатель
    DEAD_PTR       // Указатель больше не используется
  };

  // Информация о состоянии переменной
  struct VarStateInfo {
    PtrState State = PLAIN_PTR;
    const CXXNewExpr *CurrentNew = nullptr;
    SourceLocation LastChangeLoc;
    const VarDecl *Var = nullptr;
    
    VarStateInfo() = default;
    VarStateInfo(const VarDecl *V) : Var(V) {}
  };

  // Контекст анализа для одной функции
  struct FunctionAnalysisContext {
    ASTContext *Context;
    const FunctionDecl *Function;
    std::unique_ptr<CFG> TheCFG;
    llvm::DenseMap<const VarDecl *, size_t> VarIndexMap;
    std::vector<VarStateInfo> GlobalStates;
    
    FunctionAnalysisContext(ASTContext *C, const FunctionDecl *F) 
        : Context(C), Function(F) {}
  };

  bool analyzeFunction(ASTContext &Context, const FunctionDecl *Func);
  void analyzeBlock(const CFGBlock *Block,
                    llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
                    FunctionAnalysisContext &FuncCtx);
  
  void handleAssignment(const BinaryOperator *BO,
                        llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
                        FunctionAnalysisContext &FuncCtx);
  void handleSharedPtrConstructor(const CXXConstructExpr *Ctor,
                                  llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
                                  FunctionAnalysisContext &FuncCtx);
  void handleDeclStmt(const DeclStmt *DS,
                      llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
                      FunctionAnalysisContext &FuncCtx);
  void handleReturnStmt(const ReturnStmt *RS,
                        llvm::DenseMap<const VarDecl *, VarStateInfo> &States,
                        FunctionAnalysisContext &FuncCtx);
  
  bool isSharedPtrConstructor(const CXXConstructExpr *Ctor);
  const VarDecl *getUnderlyingVarDecl(const Expr *E);
  bool isNullPointer(const Expr *E);
  bool isNewExpression(const Expr *E);
  
  void reportDoubleOwnership(const VarStateInfo &State,
                             const CXXConstructExpr *SecondCtor,
                             FunctionAnalysisContext &FuncCtx);
  
  // Опции - используем StringRef вместо vector<string>
  std::string IgnoredFunctions;
};

} // namespace clang::tidy::misc

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_DOUBLESHAREDPTRCHECK_H