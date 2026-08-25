// RUN: %check_clang_tidy %s misc-double-shared-ptr %t -- -- -I %S/../Inputs/Headers/std

#include <memory>
#include <utility>

// ============================================================================
// TEST 1: Базовый случай двойного владения
// ============================================================================
void test_basic_double_ownership() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  // Это должно вызвать предупреждение
  std::shared_ptr<int> p2(a);
  // CHECK-MESSAGES: :[[@LINE-1]]:24: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
  // CHECK-MESSAGES: :[[@LINE-2]]:25: note: consider using std::shared_ptr::reset() or reassigning the raw pointer to nullptr before second use
}

// ============================================================================
// TEST 2: Объявление и инициализация в одной строке
// ============================================================================
void test_declaration_with_initialization() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  std::shared_ptr<int> p2(a);  // ОШИБКА
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 3: Переприсваивание указателя (валидный случай)
// ============================================================================
void test_reassignment_valid() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  
  a = new int(43);  // Переприсваиваем
  std::shared_ptr<int> p2(a);  // OK - новая память
  
  // Теперь a снова указывает на новую память, но мы не создаем еще один shared_ptr
  // Это OK
  int* b = new int(44);
  std::shared_ptr<int> p3(b);  // OK
}

// ============================================================================
// TEST 4: Переприсваивание с последующим двойным владением
// ============================================================================
void test_reassignment_double_ownership() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  
  a = new int(43);  // Переприсваиваем
  std::shared_ptr<int> p2(a);  // OK - новая память
  std::shared_ptr<int> p3(a);  // ОШИБКА - двойное владение новой памятью
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 5: Обнуление указателя перед повторным использованием (валидный)
// ============================================================================
void test_reset_to_nullptr() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  a = nullptr;  // Освобождаем владение
  
  int* b = new int(43);
  std::shared_ptr<int> p2(b);  // OK - новая память
  // Нет предупреждений
}

// ============================================================================
// TEST 6: Ветвления (условные)
// ============================================================================
void test_branch(bool cond) {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  
  if (cond) {
    std::shared_ptr<int> p2(a);  // ОШИБКА на одном пути
    // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
  }
}

// ============================================================================
// TEST 7: Циклы
// ============================================================================
void test_loop() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  
  for (int i = 0; i < 10; ++i) {
    std::shared_ptr<int> p2(a);  // ОШИБКА в цикле
    // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
  }
}

// ============================================================================
// TEST 8: Несколько переменных
// ============================================================================
void test_multiple_variables() {
  int* a = new int(42);
  int* b = new int(43);
  
  std::shared_ptr<int> p1(a);  // OK - первое использование a
  std::shared_ptr<int> p2(b);  // OK - первое использование b
  std::shared_ptr<int> p3(a);  // ОШИБКА - второе использование a
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 9: Копирование указателей
// ============================================================================
void test_copy_pointer() {
  int* a = new int(42);
  int* b = a;  // b указывает на ту же память
  
  std::shared_ptr<int> p1(a);  // OK
  std::shared_ptr<int> p2(b);  // ОШИБКА - b указывает на ту же память
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 10: Параметры функций (не должны вызывать предупреждений)
// ============================================================================
void test_function_parameter(int* a) {
  std::shared_ptr<int> p1(a);  // OK - память пришла извне
  std::shared_ptr<int> p2(a);  // OK - мы не знаем, откуда память
  // Нет предупреждений
}

// ============================================================================
// TEST 11: Глобальные указатели (не должны вызывать предупреждений)
// ============================================================================
int* global_ptr = new int(42);

void test_global_pointer() {
  std::shared_ptr<int> p1(global_ptr);  // OK - глобальный указатель
  std::shared_ptr<int> p2(global_ptr);  // OK - мы не знаем, откуда память
  // Нет предупреждений
}

// ============================================================================
// TEST 12: unique_ptr (не должны вызывать предупреждений)
// ============================================================================
void test_unique_ptr() {
  int* a = new int(42);
  std::unique_ptr<int> u1(a);  // OK
  std::unique_ptr<int> u2(a);  // Это тоже ошибка, но не наш случай
  // Мы проверяем только shared_ptr
}

// ============================================================================
// TEST 14: Вложенные области видимости
// ============================================================================
void test_nested_scope() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  
  {
    std::shared_ptr<int> p2(a);  // ОШИБКА внутри блока
    // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
  }
  
  // После выхода из блока p2 уничтожен, но a все еще указывает на память
  // Это опасно, но наш анализ этого не ловит (пока)
}

// ============================================================================
// TEST 15: Перемещение shared_ptr (валидный случай)
// ============================================================================
void test_move_shared_ptr() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  std::shared_ptr<int> p2(std::move(p1));  // OK - перемещение
  // Нет предупреждений
}

// ============================================================================
// TEST 16: Сложный пример с несколькими new и shared_ptr
// ============================================================================
void test_complex_case() {
  int* a = new int(1);
  std::shared_ptr<int> p1(a);
  
  a = new int(2);  // Переприсваиваем
  std::shared_ptr<int> p2(a);  // OK
  
  a = new int(3);  // Снова переприсваиваем
  std::shared_ptr<int> p3(a);  // OK
  std::shared_ptr<int> p4(a);  // ОШИБКА
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 17: Использование reset (валидный случай)
// ============================================================================
void test_reset_method() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  
  p1.reset();  // Освобождаем память
  a = new int(43);
  std::shared_ptr<int> p2(a);  // OK - новая память
  // Нет предупреждений
}

// ============================================================================
// TEST 18: Адрес переменной (должно игнорироваться)
// ============================================================================
void test_address_of_variable() {
  int x = 42;
  int* a = &x;  // Не new
  std::shared_ptr<int> p1(a);  // OK
  std::shared_ptr<int> p2(a);  // OK - мы не знаем, откуда память
  // Нет предупреждений
}

// ============================================================================
// TEST 19: Массивы
// ============================================================================
void test_arrays() {
  int* a = new int[10];  // Массив
  std::shared_ptr<int> p1(a);  // OK
  std::shared_ptr<int> p2(a);  // ОШИБКА
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 20: Ссылки на указатели
// ============================================================================
void test_pointer_reference() {
  int* a = new int(42);
  int*& ref = a;  // Ссылка на указатель
  
  std::shared_ptr<int> p1(a);   // OK
  std::shared_ptr<int> p2(ref); // ОШИБКА - ref указывает на ту же память
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 21: Указатель на указатель
// ============================================================================
void test_pointer_to_pointer() {
  int** pp = new int*(new int(42));  // Сложный случай
  // Наш анализатор может не обработать этот случай
  std::shared_ptr<int> p1(*pp);
  std::shared_ptr<int> p2(*pp);
  // Возможно, предупреждение, но не обязательно
}

// ============================================================================
// TEST 22: Условное присваивание
// ============================================================================
void test_conditional_assignment(bool cond) {
  int* a = cond ? new int(42) : new int(43);
  std::shared_ptr<int> p1(a);  // OK
  std::shared_ptr<int> p2(a);  // ОШИБКА (независимо от cond)
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 23: make_shared (не должен вызывать предупреждений)
// ============================================================================
void test_make_shared() {
  // make_shared не использует сырые указатели
  auto p1 = std::make_shared<int>(42);
  auto p2 = p1;  // OK - копирование shared_ptr
  // Нет предупреждений
}

// ============================================================================
// TEST 24: Возврат указателя из функции
// ============================================================================
int* getPointer() {
  return new int(42);
}

void test_return_from_function() {
  int* a = getPointer();  // Не можем определить, откуда память
  std::shared_ptr<int> p1(a);  // OK
  std::shared_ptr<int> p2(a);  // OK (мы не знаем, откуда память)
  // Нет предупреждений (или может быть, если доработать анализ)
}

// ============================================================================
// TEST 25: Вложенные функции
// ============================================================================
void test_nested_function() {
  auto lambda = []() {
    int* a = new int(42);
    std::shared_ptr<int> p1(a);
    std::shared_ptr<int> p2(a);  // ОШИБКА внутри лямбды
    // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
  };
  lambda();
}

// ============================================================================
// TEST 26: Указатели на константные данные
// ============================================================================
void test_const_pointer() {
  const int* a = new int(42);
  std::shared_ptr<const int> p1(a);  // OK
  std::shared_ptr<const int> p2(a);  // ОШИБКА
  // CHECK-MESSAGES: :[[@LINE-1]]:31: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

// ============================================================================
// TEST 27: Разыменование указателя
// ============================================================================
void test_dereference() {
  int* a = new int(42);
  std::shared_ptr<int> p1(a);
  *a = 100;  // OK - модификация через указатель
  std::shared_ptr<int> p2(a);  // ОШИБКА
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: raw pointer from 'new' used to initialize multiple std::shared_ptr objects [misc-double-shared-ptr]
}

