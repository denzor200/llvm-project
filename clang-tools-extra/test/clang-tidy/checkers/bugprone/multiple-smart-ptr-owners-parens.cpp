// RUN: %check_clang_tidy %s bugprone-multiple-smart-ptr-owners %t

#include <memory>

void test1() {
  int *i = new int;
  std::shared_ptr<int> p1((i));
  std::shared_ptr<int> p2((i));
  // CHECK-MESSAGES: :[[@LINE-1]]:28: warning: passing a raw pointer 'int *' to 'std::shared_ptr<int>' constructor may cause double deletion
}

void test2() {
  std::shared_ptr<int> src;
  std::shared_ptr<int> p1((src.get()));
  // CHECK-MESSAGES: :[[@LINE-1]]:28: warning: passing a raw pointer 'int *' to 'std::shared_ptr<int>' constructor may cause double deletion
  std::shared_ptr<int> p2((src.get()));
  // CHECK-MESSAGES: :[[@LINE-1]]:28: warning: passing a raw pointer 'int *' to 'std::shared_ptr<int>' constructor may cause double deletion
}

struct test3 {
  void operator() () {
  std::shared_ptr<int> p1((reinterpret_cast<int*>(this)));
  // CHECK-MESSAGES: :[[@LINE-1]]:28: warning: passing a raw pointer 'int *' to 'std::shared_ptr<int>' constructor may cause double deletion
  std::shared_ptr<int> p2((reinterpret_cast<int*>(this)));
  // CHECK-MESSAGES: :[[@LINE-1]]:28: warning: passing a raw pointer 'int *' to 'std::shared_ptr<int>' constructor may cause double deletion
  }
};

