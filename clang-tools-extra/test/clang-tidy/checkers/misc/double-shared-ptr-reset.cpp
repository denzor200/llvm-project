// RUN: %check_clang_tidy %s misc-double-shared-ptr %t -- -- -I %S/../Inputs/Headers/std

#include <memory>
#include <utility>


void test_basic_double_ownership(std::shared_ptr<int> p1, std::shared_ptr<int> p2) {
  int* a = new int(42);
  p1.reset(a);
  p2.reset(a);
  // CHECK-MESSAGES: :[[@LINE-1]]:12: warning: passing a raw pointer 'int*' to 'std::shared_ptr<int>' reset method may cause double deletion
}

void test_basic_double_two_conditions_ok(bool cond, std::shared_ptr<int> p1, std::shared_ptr<int> p2) {
  int* a = new int(42);
  if (cond) {
     p1.reset(a);
  } else {
     p2.reset(a);
  }
}

void test_basic_no_double_ownership(std::shared_ptr<int> p1, std::shared_ptr<int> p2) {
  int* a = new int(42);
  p1.reset(a);
  a = nullptr;
  p2.reset(a);
}
