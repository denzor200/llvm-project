// Mem56CppCheck.h
#ifndef CLANG_TIDY_BUGPRONE_MEM56CPPCHECK_H
#define CLANG_TIDY_BUGPRONE_MEM56CPPCHECK_H

#include "../ClangTidyCheck.h"
#include "clang/AST/Decl.h"
#include <utility>

namespace clang::tidy::bugprone {

class Mem56CppCheck : public ClangTidyCheck {
public:
  Mem56CppCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;

private:
  // Ключ: пара (функция, сырой указатель)
  // Значение: список инициализаций shared_ptr
  llvm::DenseMap<std::pair<const FunctionDecl *, const VarDecl *>, 
                 SmallVector<const CXXConstructExpr *, 2>> 
      SharedPtrInitMap;
};

} // namespace clang::tidy::bugprone

#endif // CLANG_TIDY_BUGPRONE_MEM56CPPCHECK_H