#pragma once

// chat.mak force-includes this header into every C++ translation unit.  A
// stale compiler or a dropped conformance flag therefore fails the build
// instead of silently producing a mixed-language-mode executable.

#if !defined(__cplusplus)
#error Comic Chat v1 C++ sources must be compiled as C++.
#endif

#if !defined(_MSC_VER) || !defined(_MSVC_LANG)
#error Comic Chat v1 requires Microsoft C++ Build Tools.
#endif

// Visual Studio 2026's production MSVC 14.51 toolset is the first supported
// native lane for this C++26 migration.  MSVC exposes the post-C++23 working
// draft through /std:c++latest rather than a /std:c++26 switch.
#if _MSC_VER < 1951
#error MSVC 14.51 or newer is required for the Comic Chat v1 C++26 migration.
#endif

#if _MSVC_LANG <= 202302L
#error /std:c++latest is required; C++23-or-older mode is unsupported.
#endif

static_assert(__cplusplus == _MSVC_LANG,
              "/Zc:__cplusplus must be enabled for every translation unit");

#if !defined(_MSVC_TRADITIONAL) || _MSVC_TRADITIONAL
#error /Zc:preprocessor is required for the Comic Chat v1 C++26 migration.
#endif

namespace comic_chat::v1::build {

inline constexpr long cpp_language_revision = __cplusplus;
inline constexpr int msvc_toolset_revision = _MSC_VER;

static_assert(cpp_language_revision > 202302L);
static_assert(msvc_toolset_revision >= 1951);

} // namespace comic_chat::v1::build
