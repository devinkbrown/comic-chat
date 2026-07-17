# Toolchain and language gates

## Repository invariants

| Surface | Source of truth | Required mode | Proof |
| --- | --- | --- | --- |
| Portable core | portable/meson.build, portable/include/comicchat/cpp26.hpp | Clang 21+ with cpp_std=c++26, -std=c++2c, and -pedantic-errors | Meson release compile and tests |
| Primary Linux CI | .github/workflows/build-modern.yml | Pinned Clang 22.1.8 | portable-clang job on the exact head |
| OpenBSD | .github/workflows/build-modern.yml | Clang 21 | openbsd-clang job |
| FreeBSD | .github/workflows/build-modern.yml | Clang 22 | freebsd-clang job |
| Modern Windows | v2.5-beta-1-modern/chat.mak, cpp26mode.h | MSVC 14.51+, /std:c++latest, /permissive-, conforming preprocessor | native Windows build/package/smoke job |
| Shared MFC adapter API | portable/meson.build, v2.5-beta-1-modern/tests/transport_adapter_api_compile.cpp | Same public transport header under Clang and MSVC | Clang adapter compile plus native build |

Do not apply strict C++ mode to generated MIDL glue or third-party C and then describe the result as a clean migration. Preserve the deliberate C/C++ boundary in chat.mak and portable/meson.build.

## Feature selection

1. Check the language facility in Clang's C++ status.
2. Check the library facility in libc++'s C++26 status.
3. Check MSVC conformance and the current /std mode.
4. Prefer an already-supported C++20/23 facility when a draft C++26 facility is incomplete in either required lane.
5. Guard a genuine platform gap in one focused compatibility header, such as portable/include/comicchat/thread_compat.hpp. Do not scatter compiler-version conditionals.
6. Add a compile assertion or causal test when dropping the required mode would otherwise fail silently.

## Primary sources

- Clang C++ implementation status: https://clang.llvm.org/cxx_status
- libc++ C++26 status: https://libcxx.llvm.org/Status/Cxx26.html
- MSVC /std language modes: https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version
- MSVC standards conformance mode: https://learn.microsoft.com/en-us/cpp/build/reference/permissive-standards-conformance
- Meson built-in options, including cpp_std and b_sanitize: https://mesonbuild.com/Builtin-options.html
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- Clang AddressSanitizer: https://clang.llvm.org/docs/AddressSanitizer.html
- Clang UndefinedBehaviorSanitizer: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
- Clang ThreadSanitizer: https://clang.llvm.org/docs/ThreadSanitizer.html

Treat C++26 as a working-draft toolchain mode. Verify the exact compiler and standard-library implementation instead of assuming every adopted facility exists.
