//===--- AvoidRecursiveInstantiationCheck.cpp - clang-tidy ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AvoidRecursiveInstantiationCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace modernize {

void AvoidRecursiveInstantiationCheck::registerMatchers(MatchFinder *Finder) {
  // TODO: think about template specialization
  Finder->addMatcher(cxxRecordDecl(clang::ast_matchers::isTemplateInstantiation()).bind("record"), this);
  Finder->addMatcher(functionDecl(clang::ast_matchers::isTemplateInstantiation()).bind("function"), this);
  Finder->addMatcher(varDecl(clang::ast_matchers::isTemplateInstantiation()).bind("var"), this);
}

void AvoidRecursiveInstantiationCheck::check(const MatchFinder::MatchResult &Result) {
    {
        const auto *MatchedDecl = Result.Nodes.getNodeAs<CXXRecordDecl>("record");
        if (MatchedDecl)
        {
            diag(MatchedDecl->getLocation(), "template class %0 was detected")
                    << MatchedDecl;
        }
    }
    {
        const auto *MatchedDecl = Result.Nodes.getNodeAs<FunctionDecl>("function");
        if (MatchedDecl)
        {
            diag(MatchedDecl->getLocation(), "template function %0 was detected")
                    << MatchedDecl;
        }
    }
    {
        const auto *MatchedDecl = Result.Nodes.getNodeAs<VarDecl>("var");
        if (MatchedDecl)
        {
            diag(MatchedDecl->getLocation(), "template var %0 was detected")
                    << MatchedDecl;
        }
    }
}

} // namespace modernize
} // namespace tidy
} // namespace clang
