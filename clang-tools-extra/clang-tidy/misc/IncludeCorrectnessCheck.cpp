//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "IncludeCorrectnessCheck.h"
#include "../utils/OptionsUtils.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/Path.h"

using namespace clang;
using namespace clang::tidy;

namespace {

template<typename T>
class IncludeCorrectnessPPCallbacks : public PPCallbacks {
public:
  explicit IncludeCorrectnessPPCallbacks(T &Check,
                                         const SourceManager &SM,
                                         bool StrictMode,
                                         const std::vector<StringRef> &AdditionalSystemIncludes)
      : Check(Check), SM(SM), StrictMode(StrictMode),
        AdditionalSystemIncludes(AdditionalSystemIncludes) {}

  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange, const OptionalFileEntryRef File,
                          StringRef SearchPath, StringRef RelativePath,
                          const Module *SuggestedModule,
                          bool ModuleImported,
                          SrcMgr::CharacteristicKind FileType) override {
    
    if (FileName.empty()) return;

    // Skip macros and other non-physical includes
    if (!File) return;

    // Get the actual file path
    StringRef FilePath = File->getName();

    // Determine if this is a system header
    bool IsSystemHeader = isSystemHeader(FilePath, SearchPath, FileType);
    
    // Check for correctness
    if (IsSystemHeader && !IsAngled) {
      // System header included with quotes - should use angle brackets
      Check.diag(FilenameRange.getBegin(),
                 "system header %0 should be included with angle brackets <>")
          << FileName
          << FixItHint::CreateReplacement(FilenameRange,
                                         "<" + FileName.str() + ">");
    } else if (!IsSystemHeader && IsAngled) {
      // User header included with angle brackets - should use quotes
      Check.diag(FilenameRange.getBegin(),
                 "user header %0 should be included with quotes \"\"")
          << FileName
          << FixItHint::CreateReplacement(FilenameRange,
                                         "\"" + FileName.str() + "\"");
    }
  }

private:
  bool isSystemHeader(StringRef FilePath, StringRef SearchPath,
                      SrcMgr::CharacteristicKind FileType) {
    // Use clang's built-in classification
    if (FileType == SrcMgr::C_System || FileType == SrcMgr::C_ExternCSystem) {
      return true;
    }
    
    // Check common system directories
    if (isInSystemDirectory(FilePath, SearchPath)) {
      return true;
    }
    
    // Check additional user-provided system include directories
    for (const auto &SystemPath : AdditionalSystemIncludes) {
      if (FilePath.starts_with(SystemPath)) {
        return true;
      }
    }
    
    // In strict mode, also check by file name patterns
    if (StrictMode) {
      return isKnownSystemHeader(FilePath);
    }
    
    return false;
  }
  
  bool isInSystemDirectory(StringRef FilePath, StringRef SearchPath) {
    // Common system include directories
    static const llvm::StringRef SystemDirs[] = {
        "/usr/include",
        "/usr/local/include", 
        "/System/Library",
        "/Library",
        "/Applications/Xcode.app",
        "/opt/local/include",
        "/mingw",
        "/msys",
        "/Windows Kits",
        "/Microsoft Visual Studio",
        "/clang/",
        "/gcc/",
        "/lib/gcc/"
    };
    
    // Check if file is in system directory
    for (StringRef Dir : SystemDirs) {
      if (FilePath.contains(Dir) || SearchPath.contains(Dir)) {
        return true;
      }
    }
    
    // Check if search path looks like system path
    if (!SearchPath.empty()) {
      if (SearchPath.starts_with("/usr") || 
          SearchPath.starts_with("/System") ||
          SearchPath.starts_with("/Library") ||
          SearchPath.contains("MSVC") ||
          SearchPath.contains("Windows Kits")) {
        return true;
      }
    }
    
    return false;
  }
  
  bool isKnownSystemHeader(StringRef FileName) {
    // Standard C headers
    static const llvm::StringRef CHeaders[] = {
        "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
        "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h", "setjmp.h",
        "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h", "stdbool.h",
        "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h",
        "string.h", "tgmath.h", "threads.h", "time.h", "uchar.h", "wchar.h",
        "wctype.h"
    };
    
    // Standard C++ headers
    static const llvm::StringRef CppHeaders[] = {
        "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset",
        "charconv", "chrono", "codecvt", "compare", "complex", "concepts",
        "condition_variable", "coroutine", "deque", "exception", "execution",
        "filesystem", "format", "forward_list", "fstream", "functional",
        "future", "initializer_list", "iomanip", "ios", "iosfwd", "iostream",
        "istream", "iterator", "latch", "limits", "list", "locale", "map",
        "memory", "memory_resource", "mutex", "new", "numbers", "numeric",
        "optional", "ostream", "queue", "random", "ranges", "ratio", "regex",
        "scoped_allocator", "semaphore", "set", "shared_mutex", "source_location",
        "span", "sstream", "stack", "stdexcept", "streambuf", "string",
        "string_view", "strstream", "syncstream", "system_error", "thread",
        "tuple", "type_traits", "typeindex", "typeinfo", "unordered_map",
        "unordered_set", "utility", "valarray", "variant", "vector", "version"
    };
    
    // Check if it's a standard header
    if (llvm::is_contained(CHeaders, FileName) ||
        llvm::is_contained(CppHeaders, FileName)) {
      return true;
    }
    
    // Check common patterns
    if (FileName.starts_with("c") && FileName.ends_with(".h") && 
        llvm::is_contained(CHeaders, FileName.drop_front(1))) {
      return true; // C++ wrapper headers like cstdio, cstdlib
    }
    
    return false;
  }

  T &Check;
  const SourceManager &SM;
  const bool StrictMode;
  const std::vector<StringRef> &AdditionalSystemIncludes;
};

} // namespace

namespace clang {
namespace tidy {
namespace misc {

IncludeCorrectnessCheck::IncludeCorrectnessCheck(StringRef Name, 
                                               ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      StrictMode(Options.getLocalOrGlobal("StrictMode", false)),
      AdditionalSystemIncludes(utils::options::parseStringList(Options.getLocalOrGlobal("AdditionalSystemIncludes", "")))
    {}

void IncludeCorrectnessCheck::registerPPCallbacks(
    const SourceManager &SM, Preprocessor *PP, Preprocessor *ModuleExpanderPP) {
  PP->addPPCallbacks(std::make_unique<IncludeCorrectnessPPCallbacks<IncludeCorrectnessCheck>>(
      *this, SM, StrictMode, AdditionalSystemIncludes));
}

void IncludeCorrectnessCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "StrictMode", StrictMode);
  Options.store(Opts, "AdditionalSystemIncludes", utils::options::serializeStringList(AdditionalSystemIncludes));
}

} // namespace misc
} // namespace tidy
} // namespace clang