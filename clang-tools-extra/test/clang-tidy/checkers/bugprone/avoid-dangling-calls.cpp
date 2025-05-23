// RUN: %check_clang_tidy %s bugprone-avoid-dangling-calls %t

namespace std {
    template<class T>
    const T& min(const T& a, const T& b)
    {
        return (b < a) ? b : a;
    }

    // got from use-after-move test
    template <typename>
    struct remove_reference;

    template <typename _Tp>
    struct remove_reference {
        typedef _Tp type;
    };

    template <typename _Tp>
    struct remove_reference<_Tp &> {
        typedef _Tp type;
    };

    template <typename _Tp>
    struct remove_reference<_Tp &&> {
        typedef _Tp type;
    };

    template <typename _Tp>
    constexpr typename std::remove_reference<_Tp>::type &&move(_Tp &&__t) noexcept {
        return static_cast<typename remove_reference<_Tp>::type &&>(__t);
    }
}

template<typename Dummy>
using const_ref_int = const int&;

template<typename Dummy=void>
using const_int = const int;

using const_ref_int_ = const int&;
using const_int_ = const int;


// TODO: don't spoil performance (check for triviable copyable)
// TODO: don't spoil exception safety (check for noexcept copyable)

// TODO: what we should do when no copy constructor but cast operator available?
// TODO: constructor exists and fits but it's not a copy constructor??
// TODO: test for min in template functions

// TODO: there is also safe version of min in std library
// TODO: test with implicit cast of min's arguments

// TODO: (std::min)(a,b)

// TODO: test for not a single decl
//       is that `int a = std::min(b, c), d = std::max(c, b);` ??

// TODO: test for varDecl without declStmt
//       is that `for (int a = std::max(b, c); a!=0; --a);` ??

// TODO: помимо implicit cast еще есть скобки? Вроде ignoringParenImpCasts нужен - auto a = (std::min(b,c));

// TODO: test for min in return - `const int& get() { return std::min(1, 2); }`

// TODO: test for `S{}.val`

const int& ignore(const int& value) {
    static const int st = 0;
    return st;
}


int normal()
{
    int a = 0;
    int b = 0;
    const auto c = std::min(a, b);
    const auto& d = std::min(a, b);
    const auto e = std::min(1, 2);
    const auto f = std::min(a, normal());
    const auto g = std::min(1, a*2);
    const auto& i = ignore(std::min(1, 2));
    const auto& j = ignore(ignore(std::min(1, 2)));
    const int k = std::min(1, a*2);
    const auto& l = std::min(1, 2) + 5; // TODO: make sure this should be in "normal"
    // TODO: const auto& m = std::min(a, std::move(b));
    return 0;
}

void normal_with_rvalues()
{
    int a = 0;
    int b = 0;
    const int ca = 0;
    auto&& c = std::move(a);
    // TODO: const auto&& d = std::move(b);
    // TODO: more cases like ^this??
    const auto&& e = std::move(ca);
}

void dangling()
{
    const auto& c = std::min(1, 2);
// CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'c' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
// CHECK-FIXES: const auto c = std::min(1, 2);
    int a = 2;
    const auto& d = std::min(a, normal());
// CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'd' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
// CHECK-FIXES: const auto d = std::min(a, normal());
    const auto& e = std::min(1, a*2);
// CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'e' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
// CHECK-FIXES: const auto e = std::min(1, a*2);
    const int& f = std::min(1, a*2);
// CHECK-NOTES: :[[@LINE-1]]:16: warning: dangling reference 'f' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
// CHECK-FIXES: const int f = std::min(1, a*2);
    const int& g = std::min(std::min(1, 2), std::min(a, normal()));
// CHECK-NOTES: :[[@LINE-1]]:16: warning: dangling reference 'g' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
// CHECK-FIXES: const int g = std::min(std::min(1, 2), std::min(a, normal()));
}

// TODO: also it's possible to hide via macro
void dangling_with_typedefs() {
    const_ref_int_ a = std::min(1, 2);
// CHECK-NOTES: :[[@LINE-1]]:20: warning: dangling reference 'a' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:5: note: consider changing reference to value
// CHECK-NOT-FIXES:
    const_int_& b = std::min(1, 2);
// CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'b' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:5: note: consider changing reference to value
// CHECK-FIXES: const_int_ b = std::min(1, 2);
    const_ref_int<char&> c = std::min(1, 2);
// CHECK-NOTES: :[[@LINE-1]]:26: warning: dangling reference 'c' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:5: note: consider changing reference to value
// CHECK-NOT-FIXES:
    const_int<char&>& d = std::min(1, 2);
// CHECK-NOTES: :[[@LINE-1]]:23: warning: dangling reference 'd' due to bad assignment of result of call to 'std::min' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:5: note: consider changing reference to value
// CHECK-FIXES: const_int<char&> d = std::min(1, 2);
}

void dangling_with_move()
{
    auto&& c = std::move(1);
// CHECK-NOTES: :[[@LINE-1]]:12: warning: dangling reference 'c' due to bad assignment of result of call to 'std::move' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:5: note: consider changing reference to value
// CHECK-FIXES: auto c = std::move(1);
    const auto&& d = std::move(2);
// CHECK-NOTES: :[[@LINE-1]]:18: warning: dangling reference 'd' due to bad assignment of result of call to 'std::move' with a prvalue passed
// CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
// CHECK-FIXES: const auto d = std::move(2);
    // TODO: const auto& m = std::min(a, std::move(2));
}

