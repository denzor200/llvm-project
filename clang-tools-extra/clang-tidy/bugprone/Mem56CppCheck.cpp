// Mem56CppCheck.cpp
#include "Mem56CppCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/AST/DeclCXX.h"

using namespace clang::ast_matchers;

namespace clang::tidy::bugprone {

void Mem56CppCheck::registerMatchers(ast_matchers::MatchFinder *Finder) {
  // Матчер для обнаружения инициализации std::shared_ptr через сырой указатель
  auto SharedPtrCtorMatcher =
      cxxConstructExpr(
          hasDeclaration(
              cxxConstructorDecl(
                  ofClass(hasName("std::shared_ptr")),
                  parameterCountIs(1),
                  hasParameter(0, hasType(pointsTo(qualType(anything()))))
              )
          ),
          hasArgument(0, ignoringParenCasts(declRefExpr(to(varDecl().bind("rawPtr")))))
      )
      .bind("sharedPtrInit");

  Finder->addMatcher(
      varDecl(
          hasInitializer(anyOf(
              SharedPtrCtorMatcher,
              exprWithCleanups(has(SharedPtrCtorMatcher))
          ))
      ).bind("sharedPtrVar"),
      this
  );
}

void Mem56CppCheck::check(const ast_matchers::MatchFinder::MatchResult &Result) {
  const auto *RawPtrVar = Result.Nodes.getNodeAs<VarDecl>("rawPtr");
  if (!RawPtrVar) {
    return;
  }

  const auto *SharedPtrInit = Result.Nodes.getNodeAs<CXXConstructExpr>("sharedPtrInit");
  if (!SharedPtrInit) {
    return;
  }

  const auto *SharedPtrVar = Result.Nodes.getNodeAs<VarDecl>("sharedPtrVar");
  if (!SharedPtrVar) {
    return;
  }

  // Проверяем, что это инициализация через конструктор, а не через make_shared
  const auto *CtorDecl = dyn_cast<CXXConstructorDecl>(SharedPtrInit->getConstructor());
  if (!CtorDecl) {
    return;
  }

  // Проверяем, что это конструктор с одним параметром - сырым указателем
  if (CtorDecl->getNumParams() != 1) {
    return;
  }

  // Получаем функцию, в которой находится объявление shared_ptr
  const auto *Context = SharedPtrVar->getLexicalDeclContext();
  const auto *CurrentFunction = dyn_cast_or_null<FunctionDecl>(Context);
  
  if (!CurrentFunction) {
    // Если это не функция, возможно это метод класса или блок
    return;
  }

  // Сохраняем информацию о сыром указателе и его инициализациях
  // Используем пару (функция, сырой указатель) как ключ
  auto Key = std::make_pair(CurrentFunction->getCanonicalDecl(), RawPtrVar);
  
  auto It = SharedPtrInitMap.find(Key);
  if (It == SharedPtrInitMap.end()) {
    SmallVector<const CXXConstructExpr *, 2> Inits;
    Inits.push_back(SharedPtrInit);
    SharedPtrInitMap[Key] = std::move(Inits);
  } else {
    It->second.push_back(SharedPtrInit);
  }

  // Проверяем, не использовался ли этот сырой указатель для инициализации
  // нескольких shared_ptr в одной функции
  auto &Inits = SharedPtrInitMap[Key];
  if (Inits.size() > 1) {
    // Сообщаем о проблеме для всех вхождений кроме первого
    diag(SharedPtrInit->getSourceRange().getBegin(),
         "raw pointer '%0' used to initialize multiple std::shared_ptr objects "
         "- this can lead to double deletion or undefined behavior")
        << RawPtrVar->getName();
  }
}

} // namespace clang::tidy::bugprone