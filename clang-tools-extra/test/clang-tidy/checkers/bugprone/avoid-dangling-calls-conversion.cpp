// RUN: %check_clang_tidy %s bugprone-avoid-dangling-calls %t

namespace std {
template<class T>
const T& min(const T& a, const T& b)
{
    return (b < a) ? b : a;
}
}

// TODO: must be copyable. change test for that
struct Foo2 {
    Foo2() {  }
    Foo2(const Foo2& other) = delete;
    Foo2& operator=(const Foo2& other) = delete;
    ~Foo2() {}
    friend bool operator< (const Foo2& l, const Foo2& r) {
        return true;
    }
};

struct Foo {
    Foo2 f2;
    Foo() {  }
    Foo(const Foo& other) = delete;
    Foo& operator=(const Foo& other) = delete;
    operator const Foo2& ( ) const {
        return f2;
    }
    ~Foo() {  }
    friend bool operator< (const Foo& l, const Foo& r) {
        return true;
    }
};

void normal() {
    // TODO const long& b = std::min(1, 2);
}

void dangling() {
    const Foo2& a = std::min(Foo(), Foo());
    // CHECK-NOTES: :[[@LINE-1]]:17: warning: dangling reference 'a' due to bad assignment of result of call to 'std::min' with a prvalue passed
    // CHECK-NOTES: :[[@LINE-2]]:21: note: consider storing the temporary object in a variable before passing it

    // TODO: same with static_cast
}
