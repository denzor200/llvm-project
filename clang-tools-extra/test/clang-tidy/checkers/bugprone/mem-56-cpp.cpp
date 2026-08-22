// test/clang-tidy/bugprone-mem-56-cpp.cpp
// RUN: %check_clang_tidy %s bugprone-mem-56-cpp %t

#include <memory>

// Неправильный вариант - один сырой указатель для двух shared_ptr
void test_invalid() {
  int *i = new int;
  std::shared_ptr<int> p1(i);
  std::shared_ptr<int> p2(i); // warning здесь
  // CHECK-MESSAGES: :[[@LINE-1]]:24: warning: raw pointer 'i' used to initialize multiple std::shared_ptr objects
}

// Правильный вариант - один shared_ptr
void test_valid() {
  int *i = new int;
  std::shared_ptr<int> p(i); // OK
}

// Другой правильный вариант - использование make_shared
void test_valid_make_shared() {
  auto p = std::make_shared<int>(42); // OK
}

// Использование разных сырых указателей
void test_valid_different_ptrs() {
  int *i1 = new int(10);
  int *i2 = new int(20);
  std::shared_ptr<int> p1(i1);
  std::shared_ptr<int> p2(i2); // OK - разные указатели
}

// Использование shared_ptr в разных областях видимости
void test_valid_scope() {
  int *i = new int;
  {
    std::shared_ptr<int> p1(i);
  }
  {
    std::shared_ptr<int> p2(i); // Это тоже проблема, даже в разных блоках
    // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: raw pointer 'i' used to initialize multiple std::shared_ptr objects
  }
}