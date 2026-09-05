// RUN: %check_clang_tidy %s bugprone-multiple-smart-ptr-owners %t

namespace std {
template <typename T> struct default_delete {};

template <typename T, typename D = default_delete<T>> class unique_ptr {
public:
  unique_ptr() = default;
  explicit unique_ptr(T *p) : ptr(p) {}
  ~unique_ptr() { delete ptr; }
  T *ptr = nullptr;
};

template <typename T> class shared_ptr {
public:
  shared_ptr() = default;
  explicit shared_ptr(T *p) : ptr(p) {}
  ~shared_ptr() { delete ptr; }
  T *ptr = nullptr;
};
} // namespace std

struct A {};
bool cond();

void test_straight_line_fail() {
  A *first = new A();
  A *second = new A();
  std::shared_ptr<A> a(first);
  std::shared_ptr<A> a2(first);
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: passing a raw pointer 'A *' to 'std::shared_ptr<A>' constructor may cause double deletion [bugprone-multiple-smart-ptr-owners]
  std::unique_ptr<A> b(second);
  std::unique_ptr<A> b2(second);
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: passing a raw pointer 'A *' to 'std::unique_ptr<A>' constructor may cause double deletion [bugprone-multiple-smart-ptr-owners]
}

// The two constructions are on mutually exclusive branches, so no single
// execution can double-delete the pointer. A purely lexical (non-CFG) check
// would have to choose between missing this or over-approximating; the CFG
// walk correctly finds no feasible path from one branch to the other.
void test_mutually_exclusive_branches_ok() {
  A *raw = new A();
  if (cond()) {
    std::shared_ptr<A> a(raw);
  } else {
    std::shared_ptr<A> b(raw);
  }
}

// Reassigning the raw pointer between the two constructions clears the
// "danger": the second construction now owns a different object.
void test_reassignment_clears_ok() {
  A *raw = new A();
  std::unique_ptr<A> a(raw);
  raw = new A();
  std::unique_ptr<A> b(raw);
}

// Loop where the same pointer is (re)constructed into an owner every
// iteration without being reset first -- flagged, and recognized as
// spanning loop iterations rather than a single straight-line block.
void test_loop_fail() {
  A *raw = new A();
  std::shared_ptr<A> keep;
  for (int i = 0; i < 3; ++i) {
    std::shared_ptr<A> a(raw);
    // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: passing a raw pointer 'A *' to 'std::shared_ptr<A>' constructor may cause double deletion [bugprone-multiple-smart-ptr-owners]
    // CHECK-MESSAGES: :[[@LINE-2]]:26: note: the second construction happens in a later loop iteration than the first
    keep = a;
  }
}

void test_ok_different_pointers() {
  A *p1 = new A();
  A *p2 = new A();
  std::shared_ptr<A> a(p1);
  std::shared_ptr<A> b(p2);
}