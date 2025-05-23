// RUN: %check_clang_tidy %s bugprone-avoid-dangling-calls %t

namespace std {
    template<class T>
    const T& min(const T& a, const T& b)
    {
        return (b < a) ? b : a;
    }
}

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

struct Noncopyable3 : Noncopyable {
    int val;
    explicit Noncopyable3(int val) : Noncopyable(val), val(val) {}
};

struct Noncopyable4 : Noncopyable2 {
    int val;
    explicit Noncopyable4(int val) : Noncopyable2(val), val(val) {}
};

struct Copyable : Noncopyable {
    int val;
    explicit Copyable(int val) : Noncopyable(val), val(val) {}
    Copyable(const Copyable& other) : Noncopyable(other.val), val(other.val) {}
};

struct Copyable2 : Noncopyable2 {
    int val;
    explicit Copyable2(int val) : Noncopyable2(val), val(val) {}
    Copyable2(const Copyable2& other) : Noncopyable2(other.val), val(other.val) {}
};

struct Noncopyable5 {
    Noncopyable _;
    int val;
    explicit Noncopyable5(int val) : _(val), val(val) {}
    friend bool operator< (const Noncopyable5& l, const Noncopyable5& r) {
        return l.val < r.val;
    }
};

struct Noncopyable6 {
    Noncopyable2 _;
    int val;
    explicit Noncopyable6(int val) : _(val), val(val) {}
    friend bool operator< (const Noncopyable6& l, const Noncopyable6& r) {
        return l.val < r.val;
    }
};

void normal() {
    Noncopyable a(1), b(2);
    Noncopyable2 a2(1), b2(2);
    Noncopyable3 a3(1), b3(2);
    Noncopyable4 a4(1), b4(2);
    Noncopyable5 a5(1), b5(2);
    Noncopyable6 a6(1), b6(2);
    const auto& c = std::min(a, b);
    const auto& d = std::min(a2, b2);
    const auto& e = std::min(a3, b3);
    const auto& f = std::min(a4, b4);
    const auto g = std::min(Copyable(1), Copyable(2));
    const auto i = std::min(Copyable2(1), Copyable2(2));
    const auto& j = std::min(a5, b5);
    const auto& k = std::min(a6, b6);
}

void dangling() {
    const auto& a = std::min(Noncopyable(1), Noncopyable(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'a' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& b = std::min(Noncopyable2(1), Noncopyable2(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'b' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& c = std::min(Noncopyable3(1), Noncopyable3(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'c' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& d = std::min(Noncopyable4(1), Noncopyable4(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'd' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& e = std::min(Copyable(1), Copyable(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'e' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto e = std::min(Copyable(1), Copyable(2));
    const auto& f = std::min(Copyable2(1), Copyable2(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'f' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:11: note: consider changing reference to value
    // CHECK-FIXES: const auto f = std::min(Copyable2(1), Copyable2(2));
    const auto& g = std::min(Noncopyable5(1), Noncopyable5(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'g' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    const auto& i = std::min(Noncopyable6(1), Noncopyable6(2));
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'i' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it
    
}

