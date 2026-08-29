//===----------------------------------------------------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_DOUBLESHAREDPTRCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_DOUBLESHAREDPTRCHECK_H

#include "../ClangTidyCheck.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallPtrSet.h"

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
  bool analyzeFunction(ASTContext &Context,
                      const FunctionDecl *Func,
                      const SourceManager& SM);
  std::string getRawPointerDescription(
    const Expr *PointerExpr, const ASTContext &Context);
  void dumpPtrVars(const llvm::SmallPtrSet<const VarDecl *, 32> &PtrVars);
  void dumpPtrFields(const llvm::SmallPtrSet<const FieldDecl *, 32> &PtrFields);

private:
  // TODO: must be loaded from config
  const std::vector<StringRef> SharedPointers{"::std::shared_ptr", "::boost::shared_ptr"};
  const std::vector<StringRef> UniquePointers{"::std::unique_ptr"};
  const std::vector<StringRef> DefaultDeleters{"::std::default_delete"};
};

} // namespace clang::tidy::misc

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_DOUBLESHAREDPTRCHECK_H