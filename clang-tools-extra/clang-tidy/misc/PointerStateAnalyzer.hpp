// PointerStateAnalyzer.hpp
//
// Анализ переходов состояний указателей в пределах одной функции
// на основе clang::CFG и clang::ConstStmtVisitor.
//
// Состояния:
//   0 - Unknown          (начальное состояние)
//   1 - PlainPointer      (обычный указатель, например int* a = nullptr;)
//   2 - NewPointer        (результат new, например a = new int;)
//   3 - SmartPtrWrapper  (передан в std::shared_ptr)
//
// ==========================================================================
// ИСТОРИЯ РЕДИЗАЙНА (важно для понимания текущей архитектуры)
// ==========================================================================
//
// v1: один линейный проход по блокам CFG (RPO) с ОДНИМ общим "текущим
//     состоянием" на переменную для всей функции. Ломался на ветвлениях:
//     состояние одной ветки if/else "утекало" в другую.
//
// v2: честный per-block forward dataflow (IN/OUT состояния, join на
//     слияниях путей). Решил проблему ветвлений, НО ключом состояния
//     по-прежнему был голый `const clang::VarDecl*` — то есть можно было
//     отслеживать только ЦЕЛЫЕ переменные-указатели, но НЕ поля структур:
//
//         struct A { int* val; };
//         void f(A& a) {
//             a.val = new int(42);              // LHS - MemberExpr, не DeclRefExpr!
//             std::shared_ptr<int> p(a.val);     // аргумент - тоже MemberExpr!
//         }
//
//     `a.val = ...` - это clang::BinaryOperator, чей LHS - clang::MemberExpr,
//     а не DeclRefExpr. Старый asTrackedVar() распознавал только
//     DeclRefExpr->VarDecl и на MemberExpr молча возвращал nullptr - в
//     результате присваивание ВООБЩЕ не замечалось, ни как транзакция, ни
//     как источник заражения для shared_ptr-обёртки.
//
// v3 (текущая): ключ состояния обобщён до PointerLocation - "корневая
//     переменная" + (возможно пустая) цепочка clang::FieldDecl. Пустая
//     цепочка = обычная переменная-указатель (как раньше). Непустая =
//     доступ к полю через `.` или `->` (a.val, a->val, a.b.val, ...).
//
//     Симметрично появился НОВЫЙ входной параметр PtrFields - множество
//     указательных clang::FieldDecl, которые нужно отслеживать (по
//     аналогии с тем, как PtrVars задаёт множество отслеживаемых
//     переменных). Без этого узнать "какие поля структур - указатели и
//     нас интересуют" неоткуда: сама структура (`A a;`) не указатель и
//     никогда не попадёт в PtrVars.
//
//     ЭТО МЕНЯЕТ ПУБЛИЧНУЮ СИГНАТУРУ analyzeTransitions() и тип ключа в
//     возвращаемой map (теперь PointerLocation, а не VarDecl*) - если у
//     вас есть код, читающий Result[someVarDecl], его придётся обновить
//     на Result[PointerLocation{someVarDecl, {}}] (для простых переменных
//     путь пустой, так что это прямая замена).
//
// ==========================================================================
// ЧТО ВСЁ ЕЩЁ НЕ ПОДДЕРЖИВАЕТСЯ (сознательные ограничения v3)
// ==========================================================================
//
// - Никакого alias-анализа: `A* p = &a; p->val` и `a.val` считаются
//   РАЗНЫМИ локациями (у них разные "корневые" VarDecl - p и a
//   соответственно), хотя физически это одна и та же память. Это
//   классическое ограничение field-sensitive-но-не-alias-sensitive
//   анализа; полноценный points-to анализ - совершенно другая по
//   трудозатратам задача.
// - Базой пути должен быть DeclRefExpr (переменная) - индексация массива
//   (arr[i].val), результат вызова функции (getStruct().val) не
//   отслеживаются: resolveBase() на них вернёт nullopt, обращение
//   молча игнорируется (без транзакции), а не падает и не выдаёт мусор.
// - `this->val` внутри методов класса пока не поддержан (CXXThisExpr не
//   обрабатывается resolveBase()) - при необходимости расширяется прямым
//   добавлением ветки для CXXThisExpr с отдельным "корнем", отличным от
//   VarDecl* (например, sentinel-значением).
// - Инициализация полей через списки инициализации в конструкторах
//   (member-initializer list) и агрегатную инициализацию (`A a{ptr};`)
//   не разбирается отдельно - только явные присваивания вида `a.val = ...`
//   и передача `a.val` аргументом в конструктор shared_ptr.

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallPtrSet.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

// ---------------------------------------------------------------------
// PointerLocation — обобщённый ключ отслеживаемой "ячейки" (переменная
// целиком, либо доступ к полю/цепочке полей от неё).
// ---------------------------------------------------------------------

struct PointerLocation {
    // Root == nullptr && IsThis == true  => доступ через (неявный/явный) this
    //                                        (a внутри метода == this->a)
    // Root != nullptr                    => обычная переменная/параметр
    const clang::VarDecl* Root = nullptr;
    std::vector<const clang::FieldDecl*> Path;                // [] => сама переменная Root (для Root!=nullptr);
                                                                // [f] => Root.f / Root->f / this->f;
                                                                // [f1,f2] => Root.f1.f2 и т.п.
    bool IsThis = false;                                       // поле стоит ПОСЛЕДНИМ:
                                                                // старый двухаргументный агрегатный
                                                                // инициализатор PointerLocation{Root, Path}
                                                                // остаётся корректным (IsThis=false по умолчанию).

    bool operator==(const PointerLocation& O) const {
        return Root == O.Root && IsThis == O.IsThis && Path == O.Path;
    }
    bool operator<(const PointerLocation& O) const {
        if (Root != O.Root)
            return Root < O.Root;
        if (IsThis != O.IsThis)
            return IsThis < O.IsThis;
        return Path < O.Path; // лексикографически по указателям - достаточно для строгого порядка в std::map
    }
};

// Человекочитаемое имя локации - для логов/отладки/тестов (не используется
// самим анализом).
inline std::string describeLocation(const PointerLocation& Loc) {
    std::string S = Loc.IsThis ? "this" : (Loc.Root ? Loc.Root->getName().str() : "<null>");
    for (size_t i = 0; i < Loc.Path.size(); ++i) {
        S += (Loc.IsThis && i == 0) ? "->" : ".";
        S += Loc.Path[i]->getName().str();
    }
    return S;
}

// ---------------------------------------------------------------------
// Публичный интерфейс
// ---------------------------------------------------------------------

enum PointerState : unsigned {
    PS_Unknown           = 0,
    PS_PlainPointer      = 1,
    PS_NewPointer        = 2,
    PS_SmartPtrWrapper   = 3
};

struct Transition {
    unsigned fromState;        // 0-3
    unsigned toState;          // 0-3
    const clang::Stmt* stmt;   // инструкция, вызвавшая переход
};

// ВНИМАНИЕ: сигнатура изменилась относительно предыдущих версий -
// добавлен PtrFields, ключ результата теперь PointerLocation.
std::map<PointerLocation, std::vector<Transition>> analyzeTransitions(
    const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars,
    const llvm::SmallPtrSet<const clang::FieldDecl*, 32>& PtrFields,
    const clang::CFG& cfg);

// ---------------------------------------------------------------------
// Внутренние детали реализации
// ---------------------------------------------------------------------

namespace ptr_state_detail {

using StateMap = std::map<PointerLocation, unsigned>;

inline unsigned getState(const StateMap& M, const PointerLocation& Loc) {
    auto It = M.find(Loc);
    return It == M.end() ? PS_Unknown : It->second;
}

// TODO: boost::shared_ptr
// Проверка, что тип (после раскрытия typedef/using) — специализация std::shared_ptr
inline bool isStdSharedPtrType(clang::QualType QT) {
    QT = QT.getCanonicalType();
    const clang::CXXRecordDecl* RD = QT->getAsCXXRecordDecl();
    if (!RD)
        return false;
    if (RD->getName() != "shared_ptr")
        return false;
    return RD->isInStdNamespace();
}

// Проверка, что тип (после раскрытия typedef/using) — специализация std::unique_ptr
inline bool isStdUniquePtrType(clang::QualType QT) {
    QT = QT.getCanonicalType();
    const clang::CXXRecordDecl* RD = QT->getAsCXXRecordDecl();
    if (!RD)
        return false;
    if (RD->getName() != "unique_ptr")
        return false;
    return RD->isInStdNamespace();
}

inline bool isSmartPtrType(clang::QualType QT) {
    return isStdSharedPtrType(QT) || isStdUniquePtrType(QT);
}

// Снимаем обёртки, не несущие семантической нагрузки для классификации
// значения: скобки, неявные приведения, временные объекты, cleanup-узлы.
inline const clang::Expr* stripWrappers(const clang::Expr* E) {
    while (E) {
        const clang::Expr* Prev = E;
        E = E->IgnoreParens();
        if (const auto* ICE = llvm::dyn_cast<clang::ImplicitCastExpr>(E))
            E = ICE->getSubExpr();
        else if (const auto* EWC = llvm::dyn_cast<clang::ExprWithCleanups>(E))
            E = EWC->getSubExpr();
        else if (const auto* MTE = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(E))
            E = MTE->getSubExpr();
        else if (const auto* BTE = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(E))
            E = BTE->getSubExpr();
        else if (const auto* CE = llvm::dyn_cast<clang::ConstantExpr>(E))
            E = CE->getSubExpr();
        if (E == Prev)
            break;
    }
    return E;
}

// Структурное (без проверки PtrVars/PtrFields) распознавание "пути":
// DeclRefExpr(var) -> {var, []}; MemberExpr(base, field) -> resolveBase(base) + [field].
// Используется как для базы MemberExpr (где сама база - структура, а не
// указатель, и НЕ обязана быть в PtrVars), так и внутри resolveLocation.
// Останавливается (возвращает nullopt) на всём, что не DeclRefExpr и не
// MemberExpr - индексация массива, вызов функции, this и т.п. (см.
// ограничения в шапке файла).
inline bool resolveBase(const clang::Expr* E, PointerLocation& Out) {
    E = stripWrappers(E);
    if (!E)
        return false;

    if (const auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(E)) {
        const auto* VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl());
        if (!VD)
            return false;
        Out.Root = VD;
        Out.IsThis = false;
        Out.Path.clear();
        return true;
    }

    // this->val / val (неявный this) внутри метода класса.
    if (llvm::isa<clang::CXXThisExpr>(E)) {
        Out.Root = nullptr;
        Out.IsThis = true;
        Out.Path.clear();
        return true;
    }

    if (const auto* ME = llvm::dyn_cast<clang::MemberExpr>(E)) {
        const auto* FD = llvm::dyn_cast<clang::FieldDecl>(ME->getMemberDecl());
        if (!FD)
            return false; // метод, статическое поле и т.п. - не путь к данным
        if (!resolveBase(ME->getBase(), Out))
            return false;
        Out.Path.push_back(FD);
        return true;
    }

    return false; // arr[i].val, f().val и т.п. - не поддержано
}

} // namespace ptr_state_detail

// Основной visitor — применяет "передаточную функцию" одного блока к
// переданному состоянию. Создаётся заново на каждый вызов
// runBlockTransfer(), не хранит межблочного состояния сам.
class PointerStateVisitor
    : public clang::ConstStmtVisitor<PointerStateVisitor> {
public:
    // Sink == nullptr -> переходы не записываются, только считается
    // итоговое состояние (используется в фазе 1 / fixpoint, и в фазе
    // предварительного обнаружения локаций).
    PointerStateVisitor(
            const llvm::SmallPtrSet<const clang::VarDecl*, 32>& Vars,
            const llvm::SmallPtrSet<const clang::FieldDecl*, 32>& Fields,
            ptr_state_detail::StateMap& State,
            std::map<PointerLocation, std::vector<Transition>>* Sink)
        : PtrVars(Vars), PtrFields(Fields), CurrentState(State), Sink(Sink) {}

    // int* a = nullptr; / int* a = new int; / int* b = a; ...
    void VisitDeclStmt(const clang::DeclStmt* DS) {
        for (const clang::Decl* D : DS->decls()) {
            const auto* VD = llvm::dyn_cast<clang::VarDecl>(D);
            if (!VD || !PtrVars.count(VD))
                continue;
            if (const clang::Expr* Init = VD->getInit()) {
                unsigned NewState = classify(Init);
                addTransition(PointerLocation{VD, {}}, NewState, DS);
            }
        }
        // std::shared_ptr<int> sp(a); / std::shared_ptr<int> sp(a.val); ->
        // "a" / "a.val" тоже заражается SmartPtrWrapper
        scanForSmartPtrWrap(DS);
    }

    // a = ...; / a.val = ...; (в т.ч. = new int; / = b; / = nullptr;)
    void VisitBinaryOperator(const clang::BinaryOperator* BO) {
        if (BO->getOpcode() != clang::BO_Assign) {
            scanForSmartPtrWrap(BO);
            return;
        }
        PointerLocation Loc;
        if (resolveLocation(BO->getLHS(), Loc)) {
            unsigned NewState = classify(BO->getRHS());
            addTransition(Loc, NewState, BO);
        }
        scanForSmartPtrWrap(BO);
    }

    // Прямая встреча конструктора смартпоинтера как отдельного элемента CFG
    void VisitCXXConstructExpr(const clang::CXXConstructExpr* CE) {
        handleSmartPtrConstruct(CE, CE);
    }

    // Любая другая инструкция: просто ищем внутри неё "заражение" через
    // передачу указателя/поля в конструктор смартпоинтера.
    void VisitStmt(const clang::Stmt* S) {
        scanForSmartPtrWrap(S);
    }

private:
    const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars;
    const llvm::SmallPtrSet<const clang::FieldDecl*, 32>& PtrFields;
    ptr_state_detail::StateMap& CurrentState; // состояние конкретного блока (снаружи: IN -> по итогу OUT)
    std::map<PointerLocation, std::vector<Transition>>* Sink;
    llvm::SmallPtrSet<const clang::Stmt*, 32> ProcessedConstructs; // дедуп в рамках ОДНОГО вызова обработки блока

    // Пытается распознать E как отслеживаемую локацию:
    //  - DeclRefExpr(var), где var - в PtrVars (обычная указатель-переменная)
    //  - MemberExpr(base, field), где field - в PtrFields (a.val / a->val),
    //    а base распознаётся СТРУКТУРНО (без проверки PtrVars для base -
    //    ведь base обычно НЕ указатель, а структура/объект).
    bool resolveLocation(const clang::Expr* E, PointerLocation& Out) {
        const clang::Expr* S = ptr_state_detail::stripWrappers(E);
        if (!S)
            return false;

        if (const auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(S)) {
            const auto* VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl());
            if (VD && PtrVars.count(VD)) {
                Out.Root = VD;
                Out.IsThis = false;
                Out.Path.clear();
                return true;
            }
            return false;
        }

        if (const auto* ME = llvm::dyn_cast<clang::MemberExpr>(S)) {
            const auto* FD = llvm::dyn_cast<clang::FieldDecl>(ME->getMemberDecl());
            if (!FD || !PtrFields.count(FD))
                return false;
            if (!ptr_state_detail::resolveBase(ME->getBase(), Out))
                return false;
            Out.Path.push_back(FD);
            return true;
        }

        return false;
    }

    // Определяет состояние, в которое переходит указатель, если ему
    // присваивается/инициализируется значением выражения E.
    unsigned classify(const clang::Expr* E) {
        const clang::Expr* S = ptr_state_detail::stripWrappers(E);
        if (!S)
            return PS_PlainPointer;

        // a = new int;
        if (llvm::isa<clang::CXXNewExpr>(S))
            return PS_NewPointer;

        // a = nullptr; / a = NULL;
        if (llvm::isa<clang::CXXNullPtrLiteralExpr>(S) ||
            llvm::isa<clang::GNUNullExpr>(S))
            return PS_PlainPointer;

        // a = 0;
        if (const auto* IL = llvm::dyn_cast<clang::IntegerLiteral>(S)) {
            if (IL->getValue() == 0)
                return PS_PlainPointer;
        }

        // редкий случай: указателю напрямую присваивается результат
        // конструирования умного указателя
        if (const auto* CE = llvm::dyn_cast<clang::CXXConstructExpr>(S)) {
            if (ptr_state_detail::isSmartPtrType(CE->getType()))
                return PS_SmartPtrWrapper;
        }

        // b = a;  /  b = a.val;  -> заражение ТЕКУЩИМ (для этого блока)
        // состоянием источника
        PointerLocation SrcLoc;
        if (resolveLocation(S, SrcLoc))
            return ptr_state_detail::getState(CurrentState, SrcLoc);

        // Неизвестное выражение указательного типа (вызов функции,
        // приведение типа и т.п.) считаем обычным указателем.
        if (S->getType()->isPointerType())
            return PS_PlainPointer;

        return PS_Unknown;
    }

    void addTransition(const PointerLocation& Loc, unsigned NewState,
                        const clang::Stmt* S) {
        unsigned From = ptr_state_detail::getState(CurrentState, Loc);
        if (Sink)
            (*Sink)[Loc].push_back(Transition{From, NewState, S});
        CurrentState[Loc] = NewState;
    }

    // Ищет CXXConstructExpr типа shared_ptr или unique_ptr внутри поддерева S и
    // помечает как SmartPtrWrapper любые отслеживаемые локации,
    // переданные аргументами конструктора.
    //
    // Обход итеративный (явный стек в куче), а не рекурсивный - рекурсия
    // переполняет стек вызовов на глубоко вложенных выражениях (было
    // подтверждено AddressSanitizer: stack-overflow).
    void scanForSmartPtrWrap(const clang::Stmt* Root) {
        if (!Root)
            return;

        std::vector<const clang::Stmt*> Worklist;
        Worklist.push_back(Root);

        while (!Worklist.empty()) {
            const clang::Stmt* S = Worklist.back();
            Worklist.pop_back();
            if (!S)
                continue;

            if (const auto* CE = llvm::dyn_cast<clang::CXXConstructExpr>(S))
                handleSmartPtrConstruct(CE, S);

            for (const clang::Stmt* Child : S->children())
                Worklist.push_back(Child);
        }
    }

    void handleSmartPtrConstruct(const clang::CXXConstructExpr* CE,
                                   const clang::Stmt* EnclosingStmt) {
        if (!ptr_state_detail::isSmartPtrType(CE->getType()))
            return;
        if (!ProcessedConstructs.insert(CE).second)
            return; // уже обработан в рамках этого вызова

        for (const clang::Expr* Arg : CE->arguments()) {
            PointerLocation Loc;
            if (resolveLocation(Arg, Loc))
                addTransition(Loc, PS_SmartPtrWrapper, EnclosingStmt);
        }
    }
};

// Reverse post order обход блоков CFG (итеративный DFS без рекурсии по
// стеку интерпретатора, чтобы не зависеть от глубины CFG).
inline std::vector<const clang::CFGBlock*>
computeReversePostOrder(const clang::CFG& Cfg) {
    std::vector<const clang::CFGBlock*> PostOrder;
    llvm::SmallPtrSet<const clang::CFGBlock*, 32> Visited;

    struct Frame {
        const clang::CFGBlock* Block;
        clang::CFGBlock::const_succ_iterator It;
        clang::CFGBlock::const_succ_iterator End;
    };

    const clang::CFGBlock* Entry = &Cfg.getEntry();
    if (!Entry)
        return PostOrder;

    std::vector<Frame> Stack;
    Visited.insert(Entry);
    Stack.push_back({Entry, Entry->succ_begin(), Entry->succ_end()});

    while (!Stack.empty()) {
        Frame& F = Stack.back();
        if (F.It == F.End) {
            PostOrder.push_back(F.Block);
            Stack.pop_back();
            continue;
        }
        const clang::CFGBlock* Succ = *F.It;
        ++F.It;
        if (!Succ || Visited.count(Succ))
            continue;
        Visited.insert(Succ);
        Stack.push_back({Succ, Succ->succ_begin(), Succ->succ_end()});
    }

    std::reverse(PostOrder.begin(), PostOrder.end());
    return PostOrder;
}

// Прогоняет "передаточную функцию" одного блока: на входе IN-состояние,
// на выходе итоговое (OUT) состояние. Если Sink != nullptr, попутно
// записывает переходы.
inline ptr_state_detail::StateMap runBlockTransfer(
        const clang::CFGBlock& Block,
        const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars,
        const llvm::SmallPtrSet<const clang::FieldDecl*, 32>& PtrFields,
        const ptr_state_detail::StateMap& In,
        std::map<PointerLocation, std::vector<Transition>>* Sink) {
    ptr_state_detail::StateMap Working = In;
    PointerStateVisitor Visitor(PtrVars, PtrFields, Working, Sink);
    for (const clang::CFGElement& Elem : Block) {
        if (auto CS = Elem.getAs<clang::CFGStmt>()) {
            if (const clang::Stmt* S = CS->getStmt())
                Visitor.Visit(S);
        }
    }
    return Working;
}

// Домен анализа заранее НЕ известен целиком (в отличие от v2, где им был
// просто PtrVars): локации вида a.val обнаруживаются только при обходе
// тела функции. Поэтому перед fixpoint-фазой делаем лёгкий
// предварительный проход по всем блокам НЕЗАВИСИМО (без протягивания
// состояния между блоками - оно здесь не нужно), собирая множество всех
// PointerLocation, которые вообще когда-либо являются целью
// присваивания/инициализации/аргументом shared_ptr-обёртки.
inline std::set<PointerLocation> discoverLocations(
        const std::vector<const clang::CFGBlock*>& Order,
        const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars,
        const llvm::SmallPtrSet<const clang::FieldDecl*, 32>& PtrFields) {
    std::set<PointerLocation> Domain;
    for (const clang::VarDecl* VD : PtrVars)
        Domain.insert(PointerLocation{VD, {}});

    for (const clang::CFGBlock* Block : Order) {
        if (!Block)
            continue;
        ptr_state_detail::StateMap Scratch; // одноразовый, пустой на входе каждого блока
        runBlockTransfer(*Block, PtrVars, PtrFields, Scratch, /*Sink=*/nullptr);
        for (const auto& KV : Scratch)
            Domain.insert(KV.first);
    }
    return Domain;
}

// ---------------------------------------------------------------------
// Реализация публичной функции
// ---------------------------------------------------------------------

inline std::map<PointerLocation, std::vector<Transition>>
analyzeTransitions(const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars,
                    const llvm::SmallPtrSet<const clang::FieldDecl*, 32>& PtrFields,
                    const clang::CFG& cfg) {
    using ptr_state_detail::StateMap;

    std::map<PointerLocation, std::vector<Transition>> Result;

    std::vector<const clang::CFGBlock*> Order = computeReversePostOrder(cfg);
    if (Order.empty()) {
        for (const clang::VarDecl* VD : PtrVars)
            Result[PointerLocation{VD, {}}]; // гарантируем ключ даже без CFG-блоков
        return Result;
    }

    std::set<PointerLocation> Domain = discoverLocations(Order, PtrVars, PtrFields);
    for (const PointerLocation& Loc : Domain)
        Result[Loc]; // гарантируем наличие ключа даже без переходов

    auto initialState = [&]() {
        StateMap S;
        for (const PointerLocation& Loc : Domain)
            S[Loc] = PS_Unknown;
        return S;
    };
    auto joinStates = [&](const StateMap& A, const StateMap& B) {
        StateMap R;
        for (const PointerLocation& Loc : Domain) {
            unsigned a = ptr_state_detail::getState(A, Loc);
            unsigned b = ptr_state_detail::getState(B, Loc);
            R[Loc] = (a == b) ? a : PS_Unknown;
        }
        return R;
    };
    auto statesEqual = [&](const StateMap& A, const StateMap& B) {
        for (const PointerLocation& Loc : Domain)
            if (ptr_state_detail::getState(A, Loc) != ptr_state_detail::getState(B, Loc))
                return false;
        return true;
    };

    std::map<const clang::CFGBlock*, StateMap> InState;
    std::map<const clang::CFGBlock*, StateMap> OutState;

    // ---- Фаза 1: fixpoint по IN/OUT состояниям, без записи переходов ----
    const int MaxIters = static_cast<int>(Order.size()) * 4 + 16;
    bool Changed = true;
    int Iter = 0;
    while (Changed && Iter < MaxIters) {
        Changed = false;
        ++Iter;

        for (const clang::CFGBlock* Block : Order) {
            StateMap NewIn;
            bool HaveAny = false;
            for (auto PredIt = Block->pred_begin(); PredIt != Block->pred_end(); ++PredIt) {
                const clang::CFGBlock* Pred = *PredIt;
                if (!Pred)
                    continue; // недостижимое ребро (AdjacentBlock == nullptr)
                auto It = OutState.find(Pred);
                if (It == OutState.end())
                    continue; // предшественник ещё не обработан в этом проходе
                if (!HaveAny) {
                    NewIn = It->second;
                    HaveAny = true;
                } else {
                    NewIn = joinStates(NewIn, It->second);
                }
            }
            if (!HaveAny)
                NewIn = initialState();

            auto InIt = InState.find(Block);
            bool InChanged = (InIt == InState.end()) || !statesEqual(InIt->second, NewIn);
            if (InChanged) {
                InState[Block] = NewIn;
                Changed = true;
            }

            StateMap NewOut =
                runBlockTransfer(*Block, PtrVars, PtrFields, InState[Block], /*Sink=*/nullptr);

            auto OutIt = OutState.find(Block);
            bool OutChanged = (OutIt == OutState.end()) || !statesEqual(OutIt->second, NewOut);
            if (OutChanged) {
                OutState[Block] = NewOut;
                Changed = true;
            }
        }
    }

    // ---- Фаза 2: эмиссия — каждый блок ровно один раз, с финальным IN ----
    for (const clang::CFGBlock* Block : Order) {
        StateMap In = InState.count(Block) ? InState[Block] : initialState();
        runBlockTransfer(*Block, PtrVars, PtrFields, In, &Result);
    }

    return Result;
}