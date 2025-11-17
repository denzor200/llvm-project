// RUN: %check_clang_tidy %s misc-include-correctness %t -- -header-filter=.* -- -I%S/Inputs/include-correctness

// Should warn: system header with quotes
#include "vector"
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: system header 'vector' should be included with angle brackets <>
// CHECK-FIXES: #include <vector>

#include "stdio.h"
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: system header 'stdio.h' should be included with angle brackets <>
// CHECK-FIXES: #include <stdio.h>

// Should warn: user header with angle brackets  
#include <my_header.h>
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: user header 'my_header.h' should be included with quotes ""
// CHECK-FIXES: #include "my_header.h"

// Correct usage - no warnings
#include <algorithm>
#include "project_header.h"
#include "local/file.h"