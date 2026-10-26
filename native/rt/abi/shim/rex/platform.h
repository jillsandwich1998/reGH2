// Platform / architecture / compiler detection for the isolated CPU module (native/rt/abi).
//
// Written for this project, from the needs of the generated code and of the vendored CPU headers (it is NOT the SDK's header of
// the same name, which derives from the Xenia project). Every REX_PLATFORM_*, REX_ARCH_* and REX_COMPILER_* macro is always
// defined, to 1 or 0, so both `#if X` and `#if defined(X)` style checks keep their meaning.
#pragma once

#include <bit>
#include <cstdint>

// ---- operating system
#if defined(_WIN32)
#define REX_PLATFORM_WIN32 1
#else
#define REX_PLATFORM_WIN32 0
#endif
#if defined(__APPLE__)
#define REX_PLATFORM_MAC 1
#else
#define REX_PLATFORM_MAC 0
#endif
#if defined(__ANDROID__)
#define REX_PLATFORM_ANDROID 1
#else
#define REX_PLATFORM_ANDROID 0
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#define REX_PLATFORM_GNU_LINUX 1
#else
#define REX_PLATFORM_GNU_LINUX 0
#endif
#if defined(__linux__)
#define REX_PLATFORM_LINUX 1
#else
#define REX_PLATFORM_LINUX 0
#endif

// ---- compiler
#if defined(__clang__)
#define REX_COMPILER_CLANG 1
#else
#define REX_COMPILER_CLANG 0
#endif
#if defined(__GNUC__) && !defined(__clang__)
#define REX_COMPILER_GNUC 1
#else
#define REX_COMPILER_GNUC 0
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define REX_COMPILER_MSVC 1
#else
#define REX_COMPILER_MSVC 0
#endif

// ---- CPU architecture of the host
#if defined(__x86_64__) || defined(_M_X64)
#define REX_ARCH_AMD64 1
#else
#define REX_ARCH_AMD64 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define REX_ARCH_ARM64 1
#else
#define REX_ARCH_ARM64 0
#endif

// ---- rotate builtins used by the generated code (rlwinm / rldicl ...). Clang has them; GCC does not (before 14).
#if REX_COMPILER_GNUC
#ifndef __builtin_rotateleft32
#define __builtin_rotateleft32(x, n) std::rotl(static_cast<uint32_t>(x), static_cast<int>(n))
#endif
#ifndef __builtin_rotateleft64
#define __builtin_rotateleft64(x, n) std::rotl(static_cast<uint64_t>(x), static_cast<int>(n))
#endif
#endif
