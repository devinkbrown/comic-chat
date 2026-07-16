#pragma once

// This header is force-included by chat.mak for every C++ translation unit.
// It turns the requested language mode into a build invariant instead of a
// best-effort command-line setting that can silently be dropped by an older
// compiler or an incorrectly initialized developer prompt.

#if !defined(__cplusplus)
#error Comic Chat C++ sources must be compiled as C++.
#endif

#if !defined(_MSC_VER) || !defined(_MSVC_LANG)
#error Comic Chat's legacy Windows client requires Microsoft C++ Build Tools.
#endif

// MSVC 14.51 is the current production toolset in Visual Studio 2026.  It is
// the first production lane this port supports for the post-C++23 working
// draft.  Microsoft exposes that draft through /std:c++latest; there is no
// /std:c++26 switch yet.
#if _MSC_VER < 1951
#error MSVC 14.51 or newer is required for the Comic Chat C++26 migration.
#endif

#if _MSVC_LANG <= 202302L
#error /std:c++latest is required; a C++23-or-older mode is not supported.
#endif

// /Zc:__cplusplus makes the portable macro agree with MSVC's mode macro.
static_assert(__cplusplus == _MSVC_LANG,
              "/Zc:__cplusplus must be enabled for every translation unit");

// _MSVC_TRADITIONAL is zero only when the conforming preprocessor is active.
#if !defined(_MSVC_TRADITIONAL) || _MSVC_TRADITIONAL
#error /Zc:preprocessor is required for the Comic Chat C++26 migration.
#endif

namespace comic_chat::build {

inline constexpr long cpp_language_revision = __cplusplus;
inline constexpr int msvc_toolset_revision = _MSC_VER;

static_assert(cpp_language_revision > 202302L);
static_assert(msvc_toolset_revision >= 1951);

} // namespace comic_chat::build
