//===--- BitwiseOperationOnBoolCheck.cpp - clang-tidy ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "BitwiseOperationOnBoolCheck.h"
#include "clang/Tooling/Transformer/Stencil.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;
using namespace clang::transformer;

namespace clang::tidy::performance {
namespace {
RewriteRuleWith<std::string> BitwiseOperationOnBoolCheckImpl() {
  auto WarningMessage = cat("use logical operator instead of bitwise one for bool");

  auto Applier =
      [](const MatchFinder::MatchResult &Match) -> llvm::Expected<std::string> {
        const auto *Op = Match.Nodes.getNodeAs<BinaryOperator>("op");

        SourceLocation Loc;
        Loc = Op->getOperatorLoc();
        if (Loc.isInvalid())
            return llvm::make_error<llvm::StringError>("invalid operator loc",
                                                       std::error_code());

        Loc = Match.SourceManager->getSpellingLoc(Loc);
        if (Loc.isInvalid() || Loc.isMacroID())
            return llvm::make_error<llvm::StringError>("invalid operator spelling loc",
                std::error_code());

        const CharSourceRange TokenRange = CharSourceRange::getTokenRange(Loc);
        if (TokenRange.isInvalid())
            return llvm::make_error<llvm::StringError>("invalid operator token range",
                std::error_code());

        const StringRef Spelling = Lexer::getSourceText(TokenRange, *Match.SourceManager,
                                                        Match.Context->getLangOpts());
        if (Spelling == "|" || Spelling == "|=")
            return "||";
        else if (Spelling == "&" || Spelling == "&=")
            return "&&";
        else if (Spelling == "bitand" || Spelling == "and_eq")
          return "and";
        else if (Spelling == "bitor" || Spelling == "or_eq")
          return "or";
        else
          return llvm::make_error<llvm::StringError>("invalid operator spelling: " + Spelling,
                                                     std::error_code());
      };

  auto MatcherCreator = [](auto op, auto... submatchers) {
    return binaryOperator(
               unless(isExpansionInSystemHeader()),
               hasAnyOperatorName(op),
               hasEitherOperand(expr(ignoringImpCasts(hasType(booleanType())))),
               optionally(hasEitherOperand(
                   expr(ignoringImpCasts(hasType(isVolatileQualified())))
                       .bind("vol"))),
               submatchers...
               )
        .bind("op");
  };

  auto IgnoreVolatileOperands = [](auto edit) {
    return ifBound("vol", noopEdit(opcode("op")), edit);
  };

  auto IgnoreComplicatedLHS = [](auto edit) {
    return ifBound("l", edit, noopEdit(opcode("op")));
  };

  auto ComplicatedLHSMatcher = optionally(hasLHS(ignoringParenCasts(
      declRefExpr().bind("l")
  )));

  auto RuleCreator = [=](auto replacement, auto op) {
    return makeRule(MatcherCreator(op, ComplicatedLHSMatcher),
                    IgnoreVolatileOperands(IgnoreComplicatedLHS(
                      edit(change(opcode("op"), replacement))
                    )),
                    WarningMessage);
  };

  auto ComplicatedRuleCreator = [=](auto replacement, auto op, auto... parent_ops) {
    return makeRule(MatcherCreator(op, optionally(hasAncestor(
                                         binaryOperator(hasAnyOperatorName(parent_ops...)).bind("p")))),
                    IgnoreVolatileOperands(
                      ifBound("p", editList({insertBefore(node("op"), cat("(")),
                                             change(opcode("op"), replacement),
                                             insertAfter(node("op"), cat(")"))}),
                                   edit(change(opcode("op"), replacement)))),
                    WarningMessage);
  };

  auto ComplicatedRuleForCompoundCreator = [=](auto replacement, auto op, auto... rhs_ops) {
    return makeRule(MatcherCreator(op, optionally(hasRHS(ignoringParenCasts(
                                          binaryOperator(hasAnyOperatorName(rhs_ops...)).bind("r")))),
                                       ComplicatedLHSMatcher),
                    IgnoreVolatileOperands(IgnoreComplicatedLHS(
                      ifBound("r", editList({change(opcode("op"), replacement),
                                             insertBefore(node("r"), cat("(")),
                                             insertAfter(node("r"), cat(")"))}),
                                   edit(change(opcode("op"), replacement)))
                    )),
                    WarningMessage);
  };

  auto CompoundReplacement = cat("= ", lhs("op"), " ", run(Applier));

  auto HandleOrBinaryOperator = ComplicatedRuleCreator(run(Applier), "|", /*parent_ops=*/ "&&");
  auto HandleAndBinaryOperator = ComplicatedRuleCreator(run(Applier), "&", /*parent_ops=*/ "^");
  auto HandleOrAssignBinaryOperator = RuleCreator(CompoundReplacement, "|=");
  auto HandleAndAssignBinaryOperator = ComplicatedRuleForCompoundCreator(CompoundReplacement, "&=", /*rhs_ops=*/ "||");

  return applyFirst(
        {HandleOrBinaryOperator,
         HandleAndBinaryOperator,
         HandleOrAssignBinaryOperator,
         HandleAndAssignBinaryOperator});
}
}

BitwiseOperationOnBoolCheck::BitwiseOperationOnBoolCheck(StringRef Name, ClangTidyContext *Context)
    : utils::TransformerClangTidyCheck(BitwiseOperationOnBoolCheckImpl(), Name, Context) {}

} // namespace clang::tidy::performance
