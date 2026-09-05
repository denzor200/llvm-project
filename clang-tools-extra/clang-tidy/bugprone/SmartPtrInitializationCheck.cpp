//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SmartPtrInitializationCheck.h"
#include "../utils/OptionsUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include <set>
#include <tuple>
#include <vector>
#include <memory>
#include <optional>

using namespace clang::ast_matchers;

namespace clang::tidy::bugprone {

namespace {

const auto DefaultSharedPointers = "::std::shared_ptr;::boost::shared_ptr";
const auto DefaultUniquePointers = "::std::unique_ptr";
const auto DefaultDefaultDeleters = "::std::default_delete";

} // namespace

// Remove wrappers that do not carry semantic load for classifying the value:
// brackets, implicit casts, temporary objects, cleanup nodes.
static const clang::Expr *stripWrappers(const clang::Expr *E) {
  while (E) {
    const clang::Expr *Prev = E;
    E = E->IgnoreParens();
    switch (E->getStmtClass()) {
    case clang::Stmt::ImplicitCastExprClass:
      E = cast<clang::ImplicitCastExpr>(E)->getSubExpr();
      break;
    case clang::Stmt::ExprWithCleanupsClass:
      E = cast<clang::ExprWithCleanups>(E)->getSubExpr();
      break;
    case clang::Stmt::MaterializeTemporaryExprClass:
      E = cast<clang::MaterializeTemporaryExpr>(E)->getSubExpr();
      break;
    case clang::Stmt::CXXBindTemporaryExprClass:
      E = cast<clang::CXXBindTemporaryExpr>(E)->getSubExpr();
      break;
    case clang::Stmt::ConstantExprClass:
      E = cast<clang::ConstantExpr>(E)->getSubExpr();
      break;
    default:
      break;
    }
    if (E == Prev)
      break;
  }
  return E;
}

namespace {

/// Идентифицирует сырой указатель, за которым мы следим: переменную/
/// параметр либо поле, до которого дошли цепочкой `.field` от одной из них
/// (или от `this`).
struct PointerLocation {
  const clang::VarDecl *Root = nullptr; // nullptr означает "через `this`"
  std::vector<const clang::FieldDecl *> Path;

  bool operator<(const PointerLocation &Other) const {
    return std::tie(Root, Path) < std::tie(Other.Root, Other.Path);
  }
};

/// Проходит по телу одной функции один раз, в исходном порядке, запоминая,
/// какие `PointerLocation` в данный момент принадлежат какому-то умному
/// указателю. Если одна и та же локация передаётся *второму* умному
/// указателю (в конструктор или в `.reset()`) без переприсваивания между
/// этими двумя моментами — оба умных указателя попытаются удалить один и
/// тот же объект; это и есть диагностируемый баг.
///
/// Это одиночный линейный проход, а не CFG dataflow-анализ: `if`/`switch`
/// не обрабатываются специально (две взаимоисключающие ветки, каждая из
/// которых по одному разу оборачивает один и тот же указатель, могут быть
/// (ложно) помечены), а тело цикла проходится только один раз (баг,
/// проявляющийся начиная со второй итерации, может быть пропущен). Взамен
/// — ни CFG, ни фикспойнт-итерации, ни слияния состояний.
class DoubleWrapVisitor
    : public clang::RecursiveASTVisitor<DoubleWrapVisitor> {
public:
  DoubleWrapVisitor(SmartPtrInitializationCheck &Check,
                     clang::ASTContext &Context,
                     llvm::ArrayRef<StringRef> SharedPointers,
                     llvm::ArrayRef<StringRef> UniquePointers)
      : Check(Check), Context(Context), SharedPointers(SharedPointers),
        UniquePointers(UniquePointers) {}

  // Вложенные функции/методы -- это отдельные FunctionDecl, которые будут
  // проанализированы независимо, когда матчер дойдёт до них напрямую, так
  // что не спускаемся в их тела здесь -- иначе один и тот же баг был бы
  // сообщён дважды.
  bool TraverseLambdaExpr(clang::LambdaExpr *E) {
    for (clang::Expr *Init : E->capture_inits())
      if (Init)
        TraverseStmt(Init);
    return true;
  }
  bool TraverseDecl(clang::Decl *D) {
    if (llvm::isa_and_nonnull<clang::FunctionDecl>(D))
      return true;
    return RecursiveASTVisitor::TraverseDecl(D);
  }
  
  // if (cond) { ... } else { ... }
  // Каждая ветка обходится независимо, начиная с одного и того же
  // состояния "до if"; после if остаётся только то, что оказалось
  // обёрнутым НА ОБОИХ путях -- иначе (как в примере с p1/p2 в разных
  // ветках) обёртка одного и того же указателя в двух взаимоисключающих
  // ветках ложно считалась бы двойной обёрткой.
  bool TraverseIfStmt(clang::IfStmt *If) {
    if (clang::Stmt *Init = If->getInit())
      TraverseStmt(Init);
    if (clang::VarDecl *CondVar = If->getConditionVariable())
      TraverseDecl(CondVar);
    if (clang::Expr *Cond = If->getCond())
      TraverseStmt(Cond);

    const std::set<PointerLocation> Before = Wrapped;

    if (clang::Stmt *Then = If->getThen())
      TraverseStmt(Then);
    std::set<PointerLocation> AfterThen = std::move(Wrapped);

    Wrapped = Before;
    if (clang::Stmt *Else = If->getElse())
      TraverseStmt(Else);
    std::set<PointerLocation> AfterElse = std::move(Wrapped);

    Wrapped.clear();
    std::set_intersection(AfterThen.begin(), AfterThen.end(),
                           AfterElse.begin(), AfterElse.end(),
                           std::inserter(Wrapped, Wrapped.begin()));
    return true;
  }

  // a = ...;  (любое переприсваивание снимает пометку владения для `a`,
  // независимо от нового значения)
  bool VisitBinaryOperator(const clang::BinaryOperator *BO) {
    if (BO->getOpcode() == clang::BO_Assign) {
      if (auto Loc = resolveLocation(BO->getLHS()))
        Wrapped.erase(*Loc);
    }
    return true;
  }

  // int *a = ...;
  bool VisitVarDecl(const clang::VarDecl *VD) {
    if (VD->getInit() && VD->getType()->isPointerType())
      Wrapped.erase(PointerLocation{VD, {}});
    return true;
  }

  // std::shared_ptr<T> sp(arg); / std::unique_ptr<T> up(arg);
  bool VisitCXXConstructExpr(const clang::CXXConstructExpr *CE) {
    if (isSmartPtrType(CE->getType()))
      checkArgs(CE, CE->arguments());
    return true;
  }

  // sp.reset(arg);
  bool VisitCXXMemberCallExpr(const clang::CXXMemberCallExpr *ME) {
    const auto *MD = ME->getMethodDecl();
    if (MD && MD->getDeclName().isIdentifier() && MD->getName() == "reset" &&
        isSmartPtrType(ME->getImplicitObjectArgument()->getType()))
      checkArgs(ME, ME->arguments());
    return true;
  }

private:
  template <typename ArgRange>
  void checkArgs(const clang::Expr *WrapExpr, ArgRange Args) {
    for (const clang::Expr *Arg : Args) {
      auto Loc = resolveLocation(Arg);
      if (Loc && !Wrapped.insert(*Loc).second)
        Check.emitDiagnostic(WrapExpr); // уже принадлежит другому умному указателю
    }
  }

  bool isSmartPtrType(clang::QualType QT) {
    const auto *RD = QT.getCanonicalType()->getAsCXXRecordDecl();
    if (!RD || !RD->getDeclName().isIdentifier())
      return false;
    using namespace clang::ast_matchers;
    return !match(cxxRecordDecl(hasAnyName(SharedPointers)), *RD, Context)
                .empty() ||
           !match(cxxRecordDecl(hasAnyName(UniquePointers)), *RD, Context)
                .empty();
  }

  // Структурное распознавание базы цепочки доступа к полю:
  // DeclRefExpr(var) -> {var}; `this` -> {nullptr};
  // MemberExpr(base, field) -> resolveBase(base) + [field].
  // Всё остальное (индексация массива, вызов функции, ...) не
  // поддерживается и возвращает false.
  bool resolveBase(const clang::Expr *E, PointerLocation &Out) {
    E = stripWrappers(E);
    if (!E)
      return false;
    if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(E)) {
      const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl());
      if (!VD)
        return false;
      Out = PointerLocation{VD, {}};
      return true;
    }
    if (llvm::isa<clang::CXXThisExpr>(E)) {
      Out = PointerLocation{nullptr, {}};
      return true;
    }
    if (const auto *ME = llvm::dyn_cast<clang::MemberExpr>(E)) {
      const auto *FD = llvm::dyn_cast<clang::FieldDecl>(ME->getMemberDecl());
      if (!FD || !resolveBase(ME->getBase(), Out))
        return false;
      Out.Path.push_back(FD);
      return true;
    }
    return false;
  }

  // Распознаёт E как отслеживаемую локацию: указатель-переменную/параметр
  // либо указатель-поле, достижимое через resolveBase().
  std::optional<PointerLocation> resolveLocation(const clang::Expr *E) {
    const clang::Expr *S = stripWrappers(E);
    if (!S)
      return std::nullopt;
    if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(S)) {
      const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl());
      if (VD && VD->getType()->isPointerType())
        return PointerLocation{VD, {}};
      return std::nullopt;
    }
    if (const auto *ME = llvm::dyn_cast<clang::MemberExpr>(S)) {
      const auto *FD = llvm::dyn_cast<clang::FieldDecl>(ME->getMemberDecl());
      if (!FD || !FD->getType()->isPointerType())
        return std::nullopt;
      PointerLocation Out;
      if (!resolveBase(ME->getBase(), Out))
        return std::nullopt;
      Out.Path.push_back(FD);
      return Out;
    }
    return std::nullopt;
  }

  SmartPtrInitializationCheck &Check;
  clang::ASTContext &Context;
  llvm::ArrayRef<StringRef> SharedPointers;
  llvm::ArrayRef<StringRef> UniquePointers;
  std::set<PointerLocation> Wrapped;
};

} // namespace

class SmartPtrInitializationCheckImpl {
public:
  explicit SmartPtrInitializationCheckImpl(SmartPtrInitializationCheck &Check)
      : Check(Check) {}
  virtual ~SmartPtrInitializationCheckImpl() = default;
  virtual void registerMatchers(ast_matchers::MatchFinder *Finder) = 0;
  virtual void check(const ast_matchers::MatchFinder::MatchResult &Result) = 0;
  virtual bool isStrictMode() = 0;

protected:
  SmartPtrInitializationCheck &Check;
};

class SmartPtrInitializationCheckPermissiveMode
    : public SmartPtrInitializationCheckImpl {
public:
  using SmartPtrInitializationCheckImpl::SmartPtrInitializationCheckImpl;

  void registerMatchers(ast_matchers::MatchFinder *Finder) override {
    const auto IsSharedPtr = hasAnyName(Check.SharedPointers);
    const auto IsUniquePtr = hasAnyName(Check.UniquePointers);
    const auto IsSmartPtr = anyOf(IsSharedPtr, IsUniquePtr);
    const auto IsSmartPtrRecord = cxxRecordDecl(IsSmartPtr);

    const auto SmartPtrGetCallMatcher = cxxMemberCallExpr(
        callee(cxxMethodDecl(hasName("get"))),
        on(hasType(hasUnqualifiedDesugaredType(recordType(
            hasDeclaration(classTemplateSpecializationDecl(IsSmartPtr)))))));

    // `std::shared_ptr(this)` / `std::shared_ptr(other_sp.get())`
    Finder->addMatcher(
        cxxConstructExpr(
            hasDeclaration(cxxConstructorDecl(ofClass(IsSmartPtrRecord))),
            hasArgument(0, anyOf(ignoringParenCasts(cxxThisExpr()),
                                  ignoringParenCasts(SmartPtrGetCallMatcher))))
            .bind("dangerous-ctor"),
        &Check);

    // `sp.reset(this)` / `sp.reset(other_sp.get())`
    Finder->addMatcher(
        cxxMemberCallExpr(
            on(hasType(hasUnqualifiedDesugaredType(recordType(
                hasDeclaration(classTemplateSpecializationDecl(IsSmartPtr)))))),
            callee(cxxMethodDecl(ofClass(IsSmartPtrRecord), hasName("reset"))),
            hasArgument(0, anyOf(ignoringParenCasts(cxxThisExpr()),
                                  ignoringParenCasts(SmartPtrGetCallMatcher))))
            .bind("dangerous-reset"),
        &Check);

    // Тело каждой функции/метода/лямбды один раз обходится
    // DoubleWrapVisitor'ом; CFG строить не нужно, поэтому не нужен и
    // предфильтр кандидатов.
    Finder->addMatcher(functionDecl(hasBody(anything())).bind("func"), &Check);
  }

  void check(const ast_matchers::MatchFinder::MatchResult &Result) override {
    if (const auto *Ctor =
            Result.Nodes.getNodeAs<CXXConstructExpr>("dangerous-ctor")) {
      Check.emitDiagnostic(Ctor);
      return;
    }
    if (const auto *Reset =
            Result.Nodes.getNodeAs<CXXMemberCallExpr>("dangerous-reset")) {
      Check.emitDiagnostic(Reset);
      return;
    }
    if (const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func")) {
      DoubleWrapVisitor(Check, *Result.Context, Check.SharedPointers,
                         Check.UniquePointers)
          .TraverseStmt(Func->getBody());
    }
  }

  bool isStrictMode() override { return false; }
};

class SmartPtrInitializationCheckStrictMode
    : public SmartPtrInitializationCheckImpl {
public:
  using SmartPtrInitializationCheckImpl::SmartPtrInitializationCheckImpl;

  static StatementMatcher releaseCallMatcher() {
    return cxxMemberCallExpr(callee(cxxMethodDecl(hasName("release"))));
  }

  void registerMatchers(ast_matchers::MatchFinder *Finder) override {
    const auto IsSharedPtr = hasAnyName(Check.SharedPointers);
    const auto IsUniquePtr = hasAnyName(Check.UniquePointers);
    const auto IsSmartPtr = anyOf(IsSharedPtr, IsUniquePtr);
    const auto IsDefaultDeleter = hasAnyName(Check.DefaultDeleters);

    const auto IsSharedPtrRecord = cxxRecordDecl(IsSharedPtr);
    const auto IsUniquePtrRecord = cxxRecordDecl(IsUniquePtr);
    const auto IsSmartPtrRecord = cxxRecordDecl(IsSmartPtr);

    // Array automatically decays to pointer
    const auto PointerArg =
        expr(anyOf(hasType(pointerType()), hasType(arrayType())));

    // Matcher for unique_ptr types with custom deleters
    auto UniquePtrWithCustomDeleter = classTemplateSpecializationDecl(
        IsUniquePtr, templateArgumentCountIs(2),
        hasTemplateArgument(
            1, refersToType(
                   unless(hasUnqualifiedDesugaredType(recordType(hasDeclaration(
                       classTemplateSpecializationDecl(IsDefaultDeleter))))))));

    // Matcher for shared_ptr with custom deleter in constructor
    // Check if the second argument is NOT std::default_delete
    auto SharedPtrWithCustomDeleter = allOf(
        hasDeclaration(cxxConstructorDecl(ofClass(IsSharedPtrRecord))),
        hasArgument(
            1, ignoringParenCasts(unless(hasType(hasUnqualifiedDesugaredType(
                   recordType(hasDeclaration(classTemplateSpecializationDecl(
                       IsDefaultDeleter)))))))));

    // Matcher for smart pointer constructors
    const auto HasCustomDeleter = anyOf(
        SharedPtrWithCustomDeleter,
        allOf(hasType(hasUnqualifiedDesugaredType(
                  recordType(hasDeclaration(UniquePtrWithCustomDeleter)))),
              hasDeclaration(cxxConstructorDecl(ofClass(IsUniquePtrRecord)))));

    const auto AllowedArguments =
        anyOf(ignoringParenCasts(cxxNewExpr()),
              ignoringParenCasts(releaseCallMatcher()));

    const auto OptionalCondOp =
        optionally(ignoringParenCasts(conditionalOperator().bind("cond-op")));

    const auto SmartPtrConstructorMatcher =
        cxxConstructExpr(
            hasDeclaration(cxxConstructorDecl(ofClass(IsSmartPtrRecord))),
            hasArgument(0, PointerArg), unless(HasCustomDeleter),
            unless(hasArgument(0, AllowedArguments)),
            hasArgument(0, OptionalCondOp))
            .bind("ctor");

    // For reset() - we need to check the type of the smart pointer
    // If it's shared_ptr with custom deleter (2+ args in constructor)
    // or unique_ptr with custom deleter type
    const auto SmartPtrWithCustomDeleterType = anyOf(
        // shared_ptr with custom deleter - check if the type has a second
        // template argument that is NOT std::default_delete
        classTemplateSpecializationDecl(
            IsSharedPtr, templateArgumentCountIs(2),
            hasTemplateArgument(
                1, refersToType(unless(hasUnqualifiedDesugaredType(recordType(
                       hasDeclaration(classTemplateSpecializationDecl(
                           IsDefaultDeleter)))))))),
        UniquePtrWithCustomDeleter);

    const auto HasCustomDeleterInReset =
        anyOf(on(hasType(hasUnqualifiedDesugaredType(
                  recordType(hasDeclaration(SmartPtrWithCustomDeleterType))))),
              // Also check if reset call has 2 arguments (second is deleter)
              // but we can't easily check if it's default_delete without
              // matching the function parameters, so we'll skip this case
              hasArgument(1, anything()));

    // Actually, for simplicity, let's just check if the smart pointer type
    // has a custom deleter. If it does, we skip the warning.
    const auto SmartPtrWithDefaultDeleter = classTemplateSpecializationDecl(
        IsSmartPtr,
        anyOf(
            // shared_ptr with default deleter (1 template arg or 2nd is
            // default_delete)
            allOf(IsSharedPtr,
                  anyOf(templateArgumentCountIs(1),
                        allOf(templateArgumentCountIs(2),
                              hasTemplateArgument(
                                  1, refersToType(hasUnqualifiedDesugaredType(
                                         recordType(hasDeclaration(
                                             classTemplateSpecializationDecl(
                                                 IsDefaultDeleter))))))))),
            // unique_ptr with default deleter
            allOf(IsUniquePtr,
                  anyOf(templateArgumentCountIs(1),
                        allOf(templateArgumentCountIs(2),
                              hasTemplateArgument(
                                  1, refersToType(hasUnqualifiedDesugaredType(
                                         recordType(hasDeclaration(
                                             classTemplateSpecializationDecl(
                                                 IsDefaultDeleter)))))))))));

    const auto ResetCallMatcher =
        cxxMemberCallExpr(
            on(hasType(hasUnqualifiedDesugaredType(
                recordType(hasDeclaration(SmartPtrWithDefaultDeleter))))),
            callee(cxxMethodDecl(ofClass(IsSmartPtrRecord), hasName("reset"))),
            hasArgument(0, PointerArg),
            unless(hasArgument(0, AllowedArguments)),
            hasArgument(0, OptionalCondOp))
            .bind("reset");

    Finder->addMatcher(SmartPtrConstructorMatcher, &Check);
    Finder->addMatcher(ResetCallMatcher, &Check);
  }

  void check(const ast_matchers::MatchFinder::MatchResult &Result) override {
    const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructExpr>("ctor");
    const auto *Reset = Result.Nodes.getNodeAs<CXXMemberCallExpr>("reset");
    const auto *Cond = Result.Nodes.getNodeAs<ConditionalOperator>("cond-op");
    const Expr *ConstructorOrMember = Ctor;
    if (!ConstructorOrMember)
      ConstructorOrMember = Reset;

    if (ConstructorOrMember)
      checkInternal(*Result.Context, ConstructorOrMember, Cond);
  }

  bool isStrictMode() override { return true; }

private:
  void checkInternal(ASTContext &Context, const Expr *ConstructorOrMember,
                     const ConditionalOperator *Cond) {
    if (Cond && validateConditionalOperator(Context, Cond))
      return;
    Check.emitDiagnostic(ConstructorOrMember);
  }

  bool validateConditionalOperator(ASTContext &Context,
                                   const ConditionalOperator *Cond) {
    assert(Cond);

    static const StatementMatcher Matcher =
        anyOf(integerLiteral(equals(0)), cxxNullPtrLiteralExpr(), cxxNewExpr(),
              releaseCallMatcher());

    const auto IsValidExpr = [&](const Expr *E) -> bool {
      if (!E)
        return false;

      E = E->IgnoreParenCasts();

      // If this is a nested ternary operator, we check recursively.
      if (const auto *NestedCond = dyn_cast<ConditionalOperator>(E))
        return validateConditionalOperator(Context, NestedCond);

      // Otherwise, we check through the matcher
      const auto Matches = match(Matcher, *E, Context);
      return !Matches.empty();
    };

    return IsValidExpr(Cond->getTrueExpr()) &&
           IsValidExpr(Cond->getFalseExpr());
  }
};

static std::unique_ptr<SmartPtrInitializationCheckImpl>
makeImpl(bool StrictMode, SmartPtrInitializationCheck &Check) {
  if (StrictMode)
    return std::make_unique<SmartPtrInitializationCheckStrictMode>(Check);
  return std::make_unique<SmartPtrInitializationCheckPermissiveMode>(Check);
}

SmartPtrInitializationCheck::SmartPtrInitializationCheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      SharedPointers(utils::options::parseStringList(
          Options.get("SharedPointers", DefaultSharedPointers))),
      UniquePointers(utils::options::parseStringList(
          Options.get("UniquePointers", DefaultUniquePointers))),
      DefaultDeleters(utils::options::parseStringList(
          Options.get("DefaultDeleters", DefaultDefaultDeleters))),
      Impl(makeImpl(Options.get("StrictMode", false), *this)) {}

SmartPtrInitializationCheck::~SmartPtrInitializationCheck() = default;

void SmartPtrInitializationCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "SharedPointers",
                utils::options::serializeStringList(SharedPointers));
  Options.store(Opts, "UniquePointers",
                utils::options::serializeStringList(UniquePointers));
  Options.store(Opts, "DefaultDeleters",
                utils::options::serializeStringList(DefaultDeleters));
  Options.store(Opts, "StrictMode", Impl->isStrictMode());
}

void SmartPtrInitializationCheck::registerMatchers(MatchFinder *Finder) {
  Impl->registerMatchers(Finder);
}

void SmartPtrInitializationCheck::check(
    const MatchFinder::MatchResult &Result) {
  Impl->check(Result);
}

void SmartPtrInitializationCheck::emitDiagnostic(
    const Expr *ConstructorOrMember) {
  if (const auto *SmartPtrCtor =
          dyn_cast<const CXXConstructExpr>(ConstructorOrMember)) {
    const Expr *PointerArg = stripWrappers(SmartPtrCtor->getArg(0));
    if (!PointerArg)
      return;
    const SourceLocation Loc = PointerArg->getBeginLoc();
    if (Loc.isInvalid())
      return;
    diag(Loc, "passing a raw pointer %0 to %1 constructor may cause "
              "double deletion")
        << PointerArg->getType() << SmartPtrCtor->getType();
  } else if (const auto *ResetCall =
                 dyn_cast<const CXXMemberCallExpr>(ConstructorOrMember)) {
    const Expr *PointerArg = stripWrappers(ResetCall->getArg(0));
    if (!PointerArg)
      return;
    const SourceLocation Loc = PointerArg->getBeginLoc();
    if (Loc.isInvalid())
      return;
    diag(
        Loc,
        "passing a raw pointer %0 to %1 reset method may cause double deletion")
        << PointerArg->getType() << ResetCall->getObjectType();
  }
}

} // namespace clang::tidy::bugprone
