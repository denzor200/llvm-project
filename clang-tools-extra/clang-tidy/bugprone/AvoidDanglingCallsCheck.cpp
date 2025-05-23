//===--- AvoidDanglingCallsCheck.cpp - clang-tidy -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AvoidDanglingCallsCheck.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::bugprone {

namespace {
clang::QualType removeCVRef(clang::QualType type) {
  return type.getNonReferenceType()    // remove & (or && for rvalue)
             .getUnqualifiedType();    // remove const and volatile
}
}

  // TODO: pass varDecl by reference
FixItHint AvoidDanglingCallsCheck::fixDanglingReference(const VarDecl* varDecl) {
  if (!varDecl->getType()->isReferenceType()) 
    return FixItHint();

  const bool IsRvalue = varDecl->getType()->isRValueReferenceType();

  // Check if we can succesfully rewrite declaration of the variable.
  const TypeLoc ParamTL = varDecl->getTypeSourceInfo()->getTypeLoc();
  const auto RefTL = ParamTL.getAs<ReferenceTypeLoc>();
  if (RefTL.isNull()) {
    // We cannot rewrite this instance. The type is probably hidden behind
    // some `typedef`. Do not offer a fix-it in this case.
    return FixItHint();
  }

  // Get the SourceRange of type
  const TypeSourceInfo* typeInfo = varDecl->getTypeSourceInfo();
  if (!typeInfo)
    return FixItHint();

  const SourceRange typeRange = typeInfo->getTypeLoc().getSourceRange();
  const SourceManager& sm = varDecl->getASTContext().getSourceManager();

  // Search '&'(or '&&' for a rvalue) in source code
  const CharSourceRange range = Lexer::getAsCharRange(typeRange, sm, LangOptions());
  const StringRef text = Lexer::getSourceText(range, sm, LangOptions());

  const size_t ampEnd = text.rfind('&');
  if (ampEnd == StringRef::npos) 
    return FixItHint();

  if (IsRvalue && (ampEnd == 0 || text[ampEnd-1] != '&'))
    return FixItHint();

  // Calculate precise range of '&' or '&&'
  const SourceLocation ampLocEnd = typeRange.getBegin().getLocWithOffset(ampEnd);
  const SourceLocation ampLocBeg = IsRvalue ?typeRange.getBegin().getLocWithOffset(ampEnd-1) :
    ampLocEnd;

  return FixItHint::CreateRemoval(SourceRange(ampLocBeg, ampLocEnd));
}

bool AvoidDanglingCallsCheck::canChangeReferenceToValue(const VarDecl& decl,
                                                        const CallExpr& CE) {
  const auto Type = removeCVRef(decl.getType());
  const auto *ND = dyn_cast<NamedDecl>(CE.getCalleeDecl());
  const auto ReturnType = CE.getCallReturnType(ND->getASTContext());

  if (const clang::CXXRecordDecl *record = Type->getAsCXXRecordDecl()) {
    if (ReturnType->isRValueReferenceType()) {
      for (const clang::CXXConstructorDecl *ctor : record->ctors()) {
        if (ctor->isMoveConstructor() && !ctor->isDeleted() && ctor->getAccess() != AS_private) {
            // TODO: test for without ctor->getAccess() != AS_private
            return true;
        }
      }
    }
    for (const clang::CXXConstructorDecl *ctor : record->ctors()) {
        if (ctor->isCopyConstructor() && (ctor->isDeleted() ||
                                          ctor->getAccess() == AS_private)) {
            return false;
        }
    }
  }
  return true;
}

void AvoidDanglingCallsCheck::registerMatchers(MatchFinder *Finder) {
  auto CallMatcher = callExpr(
      callee(namedDecl(hasAnyName("::std::min", "::std::max", "::std::move"))));
  Finder->addMatcher(
      varDecl(hasInitializer(
        // as we don't want to match `const auto& c = std::min(a, b);` or `auto&& c = std::move(a);`
        // anyOf(CallMatcher,
              hasDescendant(CallMatcher)
        //     )
      )).bind("var_decl"),
      this);
}

void AvoidDanglingCallsCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *MatchedDecl = Result.Nodes.getNodeAs<VarDecl>("var_decl");
  if (!MatchedDecl || !MatchedDecl->getIdentifier()) {
    llvm::dbgs() << "can't find var decl\n";
    return;
  }

  if (!MatchedDecl->getType()->isReferenceType()) {
    llvm::dbgs() << "not a reference\n";
    return;
  }

  const Expr* init = MatchedDecl->getInit()->IgnoreUnlessSpelledInSource();
  // Пропускаем случаи, где инициализатор — не прямой вызов
  if (!init || !isa<CallExpr>(init)) {
    llvm::dbgs() << "not a call expr\n";
    return;
  }
  // Дополнительная проверка имени функции
  auto* call = dyn_cast<CallExpr>(init);
  //llvm::errs() << "Found a call: " << call->getDirectCallee()->getName() << "\n";

  const auto *ND = dyn_cast<NamedDecl>(call->getCalleeDecl());
  TypeSourceInfo* typeInfo = MatchedDecl->getTypeSourceInfo();
    // TODO: llvms::any_of
  if (!ND || !typeInfo || !(ND->getQualifiedNameAsString() == "std::min" ||
                            ND->getQualifiedNameAsString() == "std::max" ||
                            ND->getQualifiedNameAsString() == "std::move")) {
    llvm::dbgs() << "not a type info " << (ND ? ND->getQualifiedNameAsString() : "") << "\n";
    return;
  }
  const bool AllowValue = canChangeReferenceToValue(*MatchedDecl, *call);
  diag(MatchedDecl->getLocation(), "dangling reference '%0' due to bad assignment of result of call to '%1' with a prvalue passed")
    << MatchedDecl->getName()
    << ND->getQualifiedNameAsString()
    << MatchedDecl->getSourceRange()
    << (AllowValue ? fixDanglingReference(MatchedDecl) : FixItHint());
  if (AllowValue) {
    diag(typeInfo->getTypeLoc().getBeginLoc(), "consider changing reference to value", DiagnosticIDs::Note)
      << typeInfo->getTypeLoc().getSourceRange();
  } else {
    // FIXME: show the parameter(parameters) that triggers the problem
    diag(call->getBeginLoc(), "consider storing the temporary object in a variable before passing it", DiagnosticIDs::Note)
      << call->getSourceRange();
  }
}

} // namespace clang::tidy::bugprone
