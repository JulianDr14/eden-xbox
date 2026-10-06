// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/logging.h"

// Sometimes we want to try to continue even after hitting an assert.
// However touching this file yields a global recompilation as this header is included almost
// everywhere. So let's just move the handling of the failed assert to a single cpp file.

void AssertFailSoftImpl();
[[noreturn]] void AssertFatalImpl();

// Prevents errors on old GCC... smh...
#ifdef _MSC_VER
#define YUZU_NO_INLINE __declspec(noinline)
#else
#define YUZU_NO_INLINE __attribute__((noinline))
#endif

// Asserts sit on hot paths (the JIT's IR passes and emitters, the shader recompiler), so a passing
// assert must cost a compare and a branch and nothing else:
// - The condition is tested inline. Wrapping it in a lambda, as before, made every passing assert
//   a call, and its [&] capture took the address of each local it named, which forced those
//   locals out of registers into the stack for the whole function.
// - The failure path is a call to an out-of-line, cold function. A plain ASSERT passes one string
//   literal (file, line and condition) to a single shared function; ASSERT_MSG copies its format
//   arguments into a per-site lambda, by value so that naming a local never takes its address.
// - MSVC ignores [[unlikely]] for block layout, so the hint matters only on GCC/Clang, where the
//   cold attribute also moves the failure code out of the hot function's cache lines.
#if defined(__GNUC__) || defined(__clang__)
#define EDEN_ASSERT_COLD __attribute__((noinline, cold))
#define EDEN_ASSERT_COLD_NORETURN __attribute__((noinline, cold, noreturn))
#define EDEN_ASSERT_PASSES(_a_) __builtin_expect(static_cast<bool>(_a_), 1)
#else
#define EDEN_ASSERT_COLD __declspec(noinline)
#define EDEN_ASSERT_COLD_NORETURN __declspec(noinline) __declspec(noreturn)
#define EDEN_ASSERT_PASSES(_a_) (_a_)
#endif
#define EDEN_ASSERT_STRINGIFY_(x) #x
#define EDEN_ASSERT_STRINGIFY(x) EDEN_ASSERT_STRINGIFY_(x)
#define EDEN_ASSERT_WHERE __FILE__ ":" EDEN_ASSERT_STRINGIFY(__LINE__)
// The traditional MSVC preprocessor (no /Zc:preprocessor, as in Dynarmic) passes a forwarded
// __VA_ARGS__ as one argument; rescanning the expansion splits it into the named parameters.
#define EDEN_ASSERT_EXPAND(x) x

/// Logs `what` ("file:line: assert condition") and applies the soft-failure policy.
EDEN_ASSERT_COLD void AssertFailedAt(const char* what);
/// Bug tracker hook: receives each failed ASSERT/UNIMPLEMENTED/UNREACHABLE `what` string (a literal,
/// so its address identifies the site) before it is logged.
using AssertHook = void (*)(const char* what) noexcept;
void SetAssertHook(AssertHook hook) noexcept;
/// Logs `where` ("file:line") as unreachable code and aborts.
EDEN_ASSERT_COLD_NORETURN void UnreachableAt(const char* where);

#define ASSERT_MSG(_a_, _fmt_, ...)                                                                \
    (EDEN_ASSERT_PASSES(_a_) ? void(0)                                                             \
                             : [](auto... assert_args) EDEN_ASSERT_COLD {                          \
                                   LOG_CRITICAL(Debug, __FILE__ ": assert " _fmt_, assert_args...); \
                                   AssertFailSoftImpl();                                           \
                               }(__VA_ARGS__))
#define ASSERT(_a_)                                                                                \
    (EDEN_ASSERT_PASSES(_a_) ? void(0) : AssertFailedAt(EDEN_ASSERT_WHERE ": assert " #_a_))

#define UNREACHABLE_MSG(_fmt_, ...)                                                                \
    [](auto... assert_args) EDEN_ASSERT_COLD_NORETURN {                                            \
        LOG_CRITICAL(Debug, __FILE__ ": unreachable " _fmt_, assert_args...);                     \
        AssertFatalImpl();                                                                         \
    }(__VA_ARGS__)
#define UNREACHABLE() UnreachableAt(EDEN_ASSERT_WHERE)

#ifdef _DEBUG
#define DEBUG_ASSERT(_a_) ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, ...) EDEN_ASSERT_EXPAND(ASSERT_MSG(_a_, __VA_ARGS__))
#else // not debug
#define DEBUG_ASSERT(_a_)                                                                          \
    do {                                                                                           \
    } while (0)
#define DEBUG_ASSERT_MSG(_a_, _desc_, ...)                                                         \
    do {                                                                                           \
    } while (0)
#endif

// As ASSERT_MSG, but logged as "unimplemented" so the bug tracker can tell missing features from
// broken invariants.
#define EDEN_UNIMPLEMENTED_UNLESS_MSG(_a_, _fmt_, ...)                                             \
    (EDEN_ASSERT_PASSES(_a_) ? void(0)                                                             \
                             : [](auto... assert_args) EDEN_ASSERT_COLD {                          \
                                   LOG_CRITICAL(Debug, __FILE__ ": unimplemented " _fmt_,          \
                                                assert_args...);                                   \
                                   AssertFailSoftImpl();                                           \
                               }(__VA_ARGS__))

#define UNIMPLEMENTED() ASSERT(false && "Unimplemented!")
#define UNIMPLEMENTED_MSG(...) EDEN_ASSERT_EXPAND(EDEN_UNIMPLEMENTED_UNLESS_MSG(false, __VA_ARGS__))

#define UNIMPLEMENTED_IF(cond) ASSERT((!(cond)) && "Unimplemented!")
#define UNIMPLEMENTED_IF_MSG(cond, ...)                                                            \
    EDEN_ASSERT_EXPAND(EDEN_UNIMPLEMENTED_UNLESS_MSG(!(cond), __VA_ARGS__))

// If the assert is ignored, execute _b_
#define ASSERT_OR_EXECUTE_MSG(_a_, _b_, ...)                                                       \
    do {                                                                                           \
        if (!EDEN_ASSERT_PASSES(_a_)) {                                                            \
            EDEN_ASSERT_EXPAND(ASSERT_MSG(false, __VA_ARGS__));                                    \
            _b_                                                                                    \
        }                                                                                          \
    } while (0)

// If the assert is ignored, execute _b_
#define ASSERT_OR_EXECUTE(_a_, _b_) ASSERT_OR_EXECUTE_MSG(_a_, _b_, "{}", #_a_)
