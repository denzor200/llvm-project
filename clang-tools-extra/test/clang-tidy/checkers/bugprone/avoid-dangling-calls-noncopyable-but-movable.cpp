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

class NoncopyableButMovable {
    int m_val;
public:
    explicit NoncopyableButMovable(int val) : m_val(val) {}
    NoncopyableButMovable(const NoncopyableButMovable& other) = delete;
    NoncopyableButMovable& operator= (const NoncopyableButMovable& other) = delete;
    NoncopyableButMovable(NoncopyableButMovable&& other) = default;
    NoncopyableButMovable& operator= (NoncopyableButMovable&& other) = default;
    friend bool operator< (const NoncopyableButMovable& l, const NoncopyableButMovable& r) {
        return l.m_val < r.m_val;
    }
};

class NoncopyableButMovable2 {
    int m_val;
public:
    explicit NoncopyableButMovable2(int val) : m_val(val) {}
    friend bool operator< (const NoncopyableButMovable2& l, const NoncopyableButMovable2& r) {
        return l.m_val < r.m_val;
    }
private:
    NoncopyableButMovable2(const NoncopyableButMovable2& other) = default;
    NoncopyableButMovable2& operator= (const NoncopyableButMovable2& other) = default;
public:
    NoncopyableButMovable2( NoncopyableButMovable2&& other) = default;
    NoncopyableButMovable2& operator= ( NoncopyableButMovable2&& other) = default;
};

struct NoncopyableButMovable3 : NoncopyableButMovable {
    int val;
    explicit NoncopyableButMovable3(int val) : NoncopyableButMovable(val), val(val) {}
};

struct NoncopyableButMovable4 : NoncopyableButMovable2 {
    int val;
    explicit NoncopyableButMovable4(int val) : NoncopyableButMovable2(val), val(val) {}
};

struct Copyable : NoncopyableButMovable {
    int val;
    explicit Copyable(int val) : NoncopyableButMovable(val), val(val) {}
    Copyable(const Copyable& other) : NoncopyableButMovable(other.val), val(other.val) {}
};

struct Copyable2 : NoncopyableButMovable2 {
    int val;
    explicit Copyable2(int val) : NoncopyableButMovable2(val), val(val) {}
    Copyable2(const Copyable2& other) : NoncopyableButMovable2(other.val), val(other.val) {}
};

struct NoncopyableButMovable5 {
    NoncopyableButMovable _;
    int val;
    explicit NoncopyableButMovable5(int val) : _(val), val(val) {}
    friend bool operator< (const NoncopyableButMovable5& l, const NoncopyableButMovable5& r) {
        return l.val < r.val;
    }
};

struct NoncopyableButMovable6 {
    NoncopyableButMovable2 _;
    int val;
    explicit NoncopyableButMovable6(int val) : _(val), val(val) {}
    friend bool operator< (const NoncopyableButMovable6& l, const NoncopyableButMovable6& r) {
        return l.val < r.val;
    }
};

class Noncopyable {
    int m_val;
public:
    explicit Noncopyable(int val) : m_val(val) {}
    Noncopyable(const Noncopyable& other) = delete;
    Noncopyable& operator= (const Noncopyable& other) = delete;
    friend bool operator< (const Noncopyable& l, const Noncopyable& r) {
        return l.m_val < r.m_val;
    }
};

class Noncopyable2 {
    int m_val;
public:
    explicit Noncopyable2(int val) : m_val(val) {}
    friend bool operator< (const Noncopyable2& l, const Noncopyable2& r) {
        return l.m_val < r.m_val;
    }
private:
    Noncopyable2(const Noncopyable2& other) = default;
    Noncopyable2& operator= (const Noncopyable2& other) = default;
};

struct NoncopyableButMovable7 : Noncopyable {
    int val;
    explicit NoncopyableButMovable7(int val) : Noncopyable(val), val(val) {}
    NoncopyableButMovable7(NoncopyableButMovable7&& other) : Noncopyable(other.val), val(other.val) {}
};

struct NoncopyableButMovable8 : Noncopyable2 {
    int val;
    explicit NoncopyableButMovable8(int val) : Noncopyable2(val), val(val) {}
    NoncopyableButMovable8(NoncopyableButMovable8&& other) : Noncopyable2(other.val), val(other.val) {}
};

void normal() {
    const auto a = std::move(NoncopyableButMovable(1));
    const auto b = std::move(NoncopyableButMovable2(2));
    const auto c = std::move(NoncopyableButMovable3(3));
    const auto d = std::move(NoncopyableButMovable4(4));
    const auto g = std::move(NoncopyableButMovable5(1));
    const auto i = std::move(NoncopyableButMovable6(2));
    const auto j = std::move(NoncopyableButMovable7(3));
    const auto k = std::move(NoncopyableButMovable8(4));
}

void normal_no_rvalue() {
    NoncopyableButMovable a(1), b(2);
    NoncopyableButMovable2 a2(1), b2(2);
    NoncopyableButMovable3 a3(1), b3(2);
    NoncopyableButMovable4 a4(1), b4(2);
    NoncopyableButMovable5 a5(1), b5(2);
    NoncopyableButMovable6 a6(1), b6(2);
    NoncopyableButMovable7 a7(1), b7(2);
    NoncopyableButMovable8 a8(1), b8(2);
    const auto& c = std::min(a, b);
    const auto& d = std::min(a2, b2);
    const auto& e = std::min(a3, b3);
    const auto& f = std::min(a4, b4);
    const auto g = std::min(Copyable(1), Copyable(2));
    const auto i = std::min(Copyable2(1), Copyable2(2));
    const auto& j = std::min(a5, b5);
    const auto& k = std::min(a6, b6);
    const auto& l = std::min(a7, b7);
    const auto& m = std::min(a8, b8);
}

void dangling() {
    const auto& a = std::move(NoncopyableButMovable(1));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'a' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto a = std::move(NoncopyableButMovable(1));
    const auto& b = std::move(NoncopyableButMovable2(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'b' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto b = std::move(NoncopyableButMovable2(2));
    const auto& c = std::move(NoncopyableButMovable3(3));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'c' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto c = std::move(NoncopyableButMovable3(3));
    const auto& d = std::move(NoncopyableButMovable4(4));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'd' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto d = std::move(NoncopyableButMovable4(4));
    const auto& e = std::min(Copyable(1), Copyable(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'e' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto e = std::min(Copyable(1), Copyable(2));
    const auto& f = std::min(Copyable2(1), Copyable2(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'f' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto f = std::min(Copyable2(1), Copyable2(2));
    const auto& g = std::move(NoncopyableButMovable5(1));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'g' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto g = std::move(NoncopyableButMovable5(1));
    const auto& i = std::move(NoncopyableButMovable6(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'i' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto i = std::move(NoncopyableButMovable6(2));
    const auto& j = std::move(NoncopyableButMovable7(3));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'j' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto j = std::move(NoncopyableButMovable7(3));
    const auto& k = std::move(NoncopyableButMovable8(4));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'k' due to bad assignment of result of call to 'std::move' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto k = std::move(NoncopyableButMovable8(4));
}

void dangling_no_rvalue() {
    const auto& a = std::min(NoncopyableButMovable(1), NoncopyableButMovable(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'a' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& b = std::min(NoncopyableButMovable2(1), NoncopyableButMovable2(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'b' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& c = std::min(NoncopyableButMovable3(1), NoncopyableButMovable3(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'c' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& d = std::min(NoncopyableButMovable4(1), NoncopyableButMovable4(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'd' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& e = std::min(NoncopyableButMovable5(1), NoncopyableButMovable5(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'e' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& f = std::min(NoncopyableButMovable6(1), NoncopyableButMovable6(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'f' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& i = std::min(NoncopyableButMovable7(1), NoncopyableButMovable7(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'i' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& j = std::min(NoncopyableButMovable8(1), NoncopyableButMovable8(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'j' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
}

