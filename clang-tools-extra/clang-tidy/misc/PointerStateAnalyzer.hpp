// PointerStateAnalyzer.hpp
//
// Анализ переходов состояний указателей в пределах одной функции
// на основе clang::CFG и clang::ConstStmtVisitor.
//
// Состояния:
//   0 - Unknown          (начальное состояние)
//   1 - PlainPointer      (обычный указатель, например int* a = nullptr;)
//   2 - NewPointer        (результат new, например a = new int;)
//   3 - SharedPtrWrapper  (передан в std::shared_ptr)
//
// АРХИТЕКТУРА (важно, т.к. предыдущая версия здесь ошибалась):
//
// Наивный однопроходный обход блоков в RPO с ОДНИМ общим "текущим
// состоянием" на переменную для всей функции даёт неверный результат
// на ветвлениях: состояние, оставшееся после обработки одной ветки
// if/else, "утекает" в соседнюю ветку вместо того, чтобы обе ветки
// стартовали от состояния их общего предка. Пример:
//
//   int* a = new int;
//   if (cond) { std::shared_ptr<int> p1(a); }
//   else      { std::shared_ptr<int> p2(a); }
//
// Наивный обход даёт a: 0->2, 2->3, 3->3 (вторая ветка "видит" состояние
// первой). Правильно: 0->2, 2->3, 2->3 — обе ветки независимо стартуют
// от состояния 2, которое было у "a" на выходе из общего предка.
//
// Поэтому здесь реализован обычный forward dataflow по CFG:
//   IN[B]  = join(OUT[P] для всех предшественников P блока B)
//   OUT[B] = результат применения "передаточной функции" блока B к IN[B]
//   join(x, y) = x, если x == y; иначе Unknown (конфликт путей)
//
// Фаза 1 (fixpoint): вычисляем стабильные IN/OUT для каждого блока
// итеративно (chaotic iteration), НЕ записывая переходы — только чтобы
// получить корректные "входные" состояния для каждого блока. Нужна из-за
// циклов (back edges), где OUT тела цикла влияет на IN условия цикла.
//
// Фаза 2 (эмиссия): проходим блоки ЕЩЁ РАЗ, ровно по одному разу, начиная
// каждый блок с его финального стабильного IN, и уже по-настоящему
// записываем Transition. Это даёт детерминированный список переходов на
// каждую переменную, не зависящий от порядка обхода фазы 1.
//
// Ограничение: join сейчас — "по значению": если два пути на входе в
// блок дают одному вару разные состояния, IN для этого вара становится
// Unknown (мы не трекаем множества возможных состояний / не делаем
// полноценный path-sensitive анализ). Для большинства реальных
// диагностик этого достаточно; при необходимости join можно заменить на
// хранение набора состояний вместо одного значения.

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"

#include <algorithm>
#include <map>
#include <vector>

// ---------------------------------------------------------------------
// Публичный интерфейс (в соответствии с требуемой сигнатурой)
// ---------------------------------------------------------------------

enum PointerState : unsigned {
    PS_Unknown          = 0,
    PS_PlainPointer      = 1,
    PS_NewPointer        = 2,
    PS_SharedPtrWrapper  = 3
};

struct Transition {
    unsigned fromState;        // 0-3
    unsigned toState;          // 0-3
    const clang::Stmt* stmt;   // инструкция, вызвавшая переход
};

std::map<const clang::VarDecl*, std::vector<Transition>> analyzeTransitions(
    const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars,
    const clang::CFG& cfg);

// ---------------------------------------------------------------------
// Внутренние детали реализации
// ---------------------------------------------------------------------

namespace ptr_state_detail {

using StateMap = llvm::DenseMap<const clang::VarDecl*, unsigned>;

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

// Если выражение (после снятия обёрток) — ссылка на отслеживаемую
// переменную-указатель, возвращает её VarDecl, иначе nullptr.
inline const clang::VarDecl* asTrackedVar(
        const clang::Expr* E,
        const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars) {
    E = stripWrappers(E);
    if (!E)
        return nullptr;
    if (const auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(E)) {
        if (const auto* VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl())) {
            if (PtrVars.count(VD))
                return VD;
        }
    }
    return nullptr;
}

// Состояние по умолчанию (Unknown) для всех отслеживаемых переменных.
inline StateMap initialState(const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars) {
    StateMap S;
    for (const clang::VarDecl* VD : PtrVars)
        S[VD] = PS_Unknown;
    return S;
}

// join двух состояний на слиянии путей: совпадают -> берём значение,
// расходятся -> Unknown (честно отражаем потерю точности, а не
// произвольно выбираем одну из веток).
inline StateMap joinStates(const StateMap& A, const StateMap& B,
                            const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars) {
    StateMap Result;
    for (const clang::VarDecl* VD : PtrVars) {
        unsigned a = A.lookup(VD);
        unsigned b = B.lookup(VD);
        Result[VD] = (a == b) ? a : PS_Unknown;
    }
    return Result;
}

inline bool statesEqual(const StateMap& A, const StateMap& B,
                         const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars) {
    for (const clang::VarDecl* VD : PtrVars)
        if (A.lookup(VD) != B.lookup(VD))
            return false;
    return true;
}

} // namespace ptr_state_detail

// Основной visitor — применяет "передаточную функцию" одного блока к
// переданному состоянию. Создаётся заново на каждый вызов
// processBlock(), поэтому не хранит межблочного состояния сам по себе:
// всё входное/выходное состояние передаётся явно снаружи (см.
// analyzeTransitions). Это то, что делает возможным честный per-block
// dataflow вместо одного общего состояния на весь обход.
class PointerStateVisitor
    : public clang::ConstStmtVisitor<PointerStateVisitor> {
public:
    // Sink == nullptr -> переходы не записываются, только считается
    // итоговое состояние (используется в фазе 1 / fixpoint).
    // Sink != nullptr -> переходы записываются (фаза 2 / эмиссия).
    PointerStateVisitor(
            const llvm::SmallPtrSet<const clang::VarDecl*, 32>& Vars,
            ptr_state_detail::StateMap& State,
            std::map<const clang::VarDecl*, std::vector<Transition>>* Sink)
        : PtrVars(Vars), CurrentState(State), Sink(Sink) {}

    // int* a = nullptr; / int* a = new int; / int* b = a; ...
    void VisitDeclStmt(const clang::DeclStmt* DS) {
        for (const clang::Decl* D : DS->decls()) {
            const auto* VD = llvm::dyn_cast<clang::VarDecl>(D);
            if (!VD || !PtrVars.count(VD))
                continue;
            if (const clang::Expr* Init = VD->getInit()) {
                unsigned NewState = classify(Init);
                addTransition(VD, NewState, DS);
            }
        }
        // std::shared_ptr<int> sp(a); -> "a" тоже заражается SharedPtrWrapper
        scanForSharedPtrWrap(DS);
    }

    // a = ...; (в т.ч. a = new int; / a = b; / a = nullptr;)
    void VisitBinaryOperator(const clang::BinaryOperator* BO) {
        if (BO->getOpcode() != clang::BO_Assign) {
            scanForSharedPtrWrap(BO);
            return;
        }
        if (const clang::VarDecl* VD =
                ptr_state_detail::asTrackedVar(BO->getLHS(), PtrVars)) {
            unsigned NewState = classify(BO->getRHS());
            addTransition(VD, NewState, BO);
        }
        scanForSharedPtrWrap(BO);
    }

    // Прямая встреча конструктора shared_ptr как отдельного элемента CFG
    void VisitCXXConstructExpr(const clang::CXXConstructExpr* CE) {
        handleSharedPtrConstruct(CE, CE);
    }

    // Любая другая инструкция: просто ищем внутри неё "заражение" через
    // передачу указателя в конструктор std::shared_ptr.
    void VisitStmt(const clang::Stmt* S) {
        scanForSharedPtrWrap(S);
    }

private:
    const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars;
    ptr_state_detail::StateMap& CurrentState; // состояние конкретного блока (снаружи: IN -> по итогу OUT)
    std::map<const clang::VarDecl*, std::vector<Transition>>* Sink;
    llvm::SmallPtrSet<const clang::Stmt*, 32> ProcessedConstructs; // дедуп в рамках ОДНОГО вызова обработки блока

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
        // конструирования shared_ptr (типы обычно не совпадут, но
        // оставляем для общности API)
        if (const auto* CE = llvm::dyn_cast<clang::CXXConstructExpr>(S)) {
            if (ptr_state_detail::isStdSharedPtrType(CE->getType()))
                return PS_SharedPtrWrapper;
        }

        // b = a;  -> заражение ТЕКУЩИМ (для этого блока) состоянием источника
        if (const clang::VarDecl* Src =
                ptr_state_detail::asTrackedVar(S, PtrVars))
            return CurrentState.lookup(Src);

        // Неизвестное выражение указательного типа (вызов функции,
        // приведение типа и т.п.) считаем обычным указателем.
        if (S->getType()->isPointerType())
            return PS_PlainPointer;

        return PS_Unknown;
    }

    void addTransition(const clang::VarDecl* VD, unsigned NewState,
                        const clang::Stmt* S) {
        unsigned From = CurrentState[VD];
        if (Sink)
            (*Sink)[VD].push_back(Transition{From, NewState, S});
        // Состояние обновляем всегда (даже в фазе 1 без записи), иначе
        // не сможем вычислить корректный OUT[Block].
        CurrentState[VD] = NewState;
    }

    // Ищет CXXConstructExpr типа std::shared_ptr внутри поддерева S и
    // помечает как SharedPtrWrapper любые отслеживаемые переменные-указатели,
    // переданные аргументами конструктора.
    //
    // Обход итеративный (явный стек в куче), а не рекурсивный — рекурсия
    // переполняет стек вызовов на глубоко вложенных выражениях (было
    // подтверждено AddressSanitizer: stack-overflow).
    void scanForSharedPtrWrap(const clang::Stmt* Root) {
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
                handleSharedPtrConstruct(CE, S);

            for (const clang::Stmt* Child : S->children())
                Worklist.push_back(Child);
        }
    }

    void handleSharedPtrConstruct(const clang::CXXConstructExpr* CE,
                                   const clang::Stmt* EnclosingStmt) {
        if (!ptr_state_detail::isStdSharedPtrType(CE->getType()))
            return;
        // Защита от повторной обработки одного и того же узла в рамках
        // ОДНОГО вызова обработки блока (может встретиться и как
        // отдельный CFGStmt, и через рекурсию от родительской
        // инструкции).
        if (!ProcessedConstructs.insert(CE).second)
            return;

        for (const clang::Expr* Arg : CE->arguments()) {
            if (const clang::VarDecl* VD =
                    ptr_state_detail::asTrackedVar(Arg, PtrVars)) {
                addTransition(VD, PS_SharedPtrWrapper, EnclosingStmt);
            }
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
// записывает переходы (для фазы эмиссии).
inline ptr_state_detail::StateMap runBlockTransfer(
        const clang::CFGBlock& Block,
        const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars,
        const ptr_state_detail::StateMap& In,
        std::map<const clang::VarDecl*, std::vector<Transition>>* Sink) {
    ptr_state_detail::StateMap Working = In;
    PointerStateVisitor Visitor(PtrVars, Working, Sink);
    for (const clang::CFGElement& Elem : Block) {
        if (auto CS = Elem.getAs<clang::CFGStmt>()) {
            if (const clang::Stmt* S = CS->getStmt())
                Visitor.Visit(S);
        }
    }
    return Working;
}

// ---------------------------------------------------------------------
// Реализация публичной функции
// ---------------------------------------------------------------------

inline std::map<const clang::VarDecl*, std::vector<Transition>>
analyzeTransitions(const llvm::SmallPtrSet<const clang::VarDecl*, 32>& PtrVars,
                    const clang::CFG& cfg) {
    using ptr_state_detail::StateMap;

    std::map<const clang::VarDecl*, std::vector<Transition>> Result;
    for (const clang::VarDecl* VD : PtrVars)
        Result[VD]; // гарантируем наличие ключа даже без переходов

    std::vector<const clang::CFGBlock*> Order = computeReversePostOrder(cfg);
    if (Order.empty())
        return Result;

    llvm::DenseMap<const clang::CFGBlock*, StateMap> InState;
    llvm::DenseMap<const clang::CFGBlock*, StateMap> OutState;

    // ---- Фаза 1: fixpoint по IN/OUT состояниям, без записи переходов ----
    //
    // Обычная "chaotic iteration": пересчитываем IN/OUT для всех блоков
    // в RPO-порядке, пока что-то меняется. Для ациклического CFG сходится
    // за один проход; циклы (back edges) могут потребовать нескольких —
    // отсюда ограничение MaxIters как защита от неожиданного незавершения
    // на патологических случаях (само состояние — не строго монотонная
    // решётка, т.к. присваивания могут "откатывать" состояние назад).
    const int MaxIters = static_cast<int>(Order.size()) * 4 + 16;
    bool Changed = true;
    int Iter = 0;
    while (Changed && Iter < MaxIters) {
        Changed = false;
        ++Iter;

        for (const clang::CFGBlock* Block : Order) {
            // IN[Block] = join(OUT[P]) по всем предшественникам, чьи OUT
            // уже известны хотя бы приближённо. Если предшественников с
            // известным OUT нет (первый проход, либо блок без входящих
            // рёбер) — считаем Unknown для всех переменных.
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
                    NewIn = ptr_state_detail::joinStates(NewIn, It->second, PtrVars);
                }
            }
            if (!HaveAny)
                NewIn = ptr_state_detail::initialState(PtrVars);

            auto InIt = InState.find(Block);
            bool InChanged = (InIt == InState.end()) ||
                              !ptr_state_detail::statesEqual(InIt->second, NewIn, PtrVars);
            if (InChanged) {
                InState[Block] = NewIn;
                Changed = true;
            }

            StateMap NewOut =
                runBlockTransfer(*Block, PtrVars, InState[Block], /*Sink=*/nullptr);

            auto OutIt = OutState.find(Block);
            bool OutChanged = (OutIt == OutState.end()) ||
                               !ptr_state_detail::statesEqual(OutIt->second, NewOut, PtrVars);
            if (OutChanged) {
                OutState[Block] = NewOut;
                Changed = true;
            }
        }
    }

    // ---- Фаза 2: эмиссия — каждый блок ровно один раз, с финальным IN ----
    for (const clang::CFGBlock* Block : Order) {
        StateMap In = InState.count(Block) ? InState[Block]
                                            : ptr_state_detail::initialState(PtrVars);
        runBlockTransfer(*Block, PtrVars, In, &Result);
    }

    return Result;
}