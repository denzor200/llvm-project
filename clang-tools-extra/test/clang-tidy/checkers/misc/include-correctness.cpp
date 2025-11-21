// RUN: %check_clang_tidy %s misc-include-correctness %t -- -header-filter=.* --config='{CheckOptions: {misc-include-correctness.AdditionalSystemIncludes: %S/Inputs/include-correctness/system}}' -- -I%S/Inputs/include-correctness/system -I%S/Inputs/include-correctness/user

// Should warn: system header with quotes
#include "vector"
// CHECK-MESSAGES: :[[@LINE-1]]:10: warning: system header 'vector' should be included with angle brackets <> [misc-include-correctness]
// CHECK-FIXES: #include <vector>

#include "stdio.h"
// CHECK-MESSAGES: :[[@LINE-1]]:10: warning: system header 'stdio.h' should be included with angle brackets <> [misc-include-correctness]
// CHECK-FIXES: #include <stdio.h>

// Should warn: user header with angle brackets  
#include <my_header.h>
// CHECK-MESSAGES: :[[@LINE-1]]:10: warning: user header 'my_header.h' should be included with quotes "" [misc-include-correctness]
// CHECK-FIXES: #include "my_header.h"

// Correct usage - no warnings
#include <algorithm>
#include "project_header.h"
#include "local/file.h"