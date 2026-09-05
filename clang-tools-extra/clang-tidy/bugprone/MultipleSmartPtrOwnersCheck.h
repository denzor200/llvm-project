//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_BUGPRONE_MULTIPLESMARTPTROWNERSCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_BUGPRONE_MULTIPLESMARTPTROWNERSCHECK_H

#include "../ClangTidyCheck.h"

namespace clang::tidy::bugprone {

/// Finds places where the same raw pointer is passed to more than one
/// owning smart pointer (`std::unique_ptr` / `std::shared_ptr`) along some
/// feasible control-flow path, which leads to double deletion once both
/// owners run their destructor.
///
/// Like `bugprone-use-after-move`, this check builds a CFG for the
/// enclosing function/lambda/constructor and walks it forward from each
/// "ownership transfer" (a raw pointer handed to a smart-pointer
/// constructor), so that branches, loops and reassignments of the raw
/// pointer are all taken into account instead of relying on lexical order.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/bugprone/multiple-smart-ptr-owners.html
class MultipleSmartPtrOwnersCheck : public ClangTidyCheck {
public:
  MultipleSmartPtrOwnersCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;

  std::optional<TraversalKind> getCheckTraversalKind() const override {
    return TK_IgnoreUnlessSpelledInSource;
  }
};

} // namespace clang::tidy::bugprone

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_BUGPRONE_MULTIPLESMARTPTROWNERSCHECK_H