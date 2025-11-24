//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_INCLUDECORRECTNESSCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_INCLUDECORRECTNESSCHECK_H

#include "../ClangTidyCheck.h"

namespace clang {
namespace tidy {
namespace misc {

/// Checks for correctness of angle brackets (<>) and quotes ("") in #include
/// statements.
///
/// Angle brackets should be used only for system headers (from /usr/include,
/// STL paths, compiler include paths, etc.), while quotes should be used for
/// project-local headers.
///
/// For the user-facing documentation see:
/// https://clang.llvm.org/extra/clang-tidy/checks/misc/include-correctness.html
class IncludeCorrectnessCheck : public ClangTidyCheck {
public:
  IncludeCorrectnessCheck(StringRef Name, ClangTidyContext *Context);
  void registerPPCallbacks(const SourceManager &SM, Preprocessor *PP,
                           Preprocessor *ModuleExpanderPP) override;
  void storeOptions(ClangTidyOptions::OptionMap &Opts) override;

private:
  const bool StrictMode;
  const bool Verbose;
  const std::vector<StringRef> SystemIncludes;
};

} // namespace misc
} // namespace tidy
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_INCLUDECORRECTNESSCHECK_H