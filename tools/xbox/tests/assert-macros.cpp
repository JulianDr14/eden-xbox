// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// Behaviour of the assert macros (common/assert.h): single evaluation, messages, expression use,
// constant evaluation and the forwarding macros. Build it twice, with and without
// /Zc:preprocessor (Dynarmic uses the traditional preprocessor), from a desktop x64 prompt:
//   cl /O2 /MD /std:c++20 /EHsc /utf-8 /W4 [/Zc:preprocessor] /I src /I <fmt include>
//      /I build-uwp\src assert-macros.cpp fmt.lib

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/assert.h"

static std::string last;
static int logs = 0, soft = 0, evals = 0;
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*, fmt::string_view f,
                       const fmt::format_args& a) { last = fmt::vformat(f, a); ++logs; }
void Stop() {}
}
void AssertFailSoftImpl() { ++soft; }
void AssertFatalImpl() { std::exit(0); }
void AssertFailedAt(const char* what) { LOG_CRITICAL(Debug, "{}", what); AssertFailSoftImpl(); }
void UnreachableAt(const char* where) { LOG_CRITICAL(Debug, "{}: unreachable", where); AssertFatalImpl(); }

static bool Once(bool v) { ++evals; return v; }
constexpr int Checked(int x) { ASSERT(x >= 0); return x * 2; }
static_assert(Checked(4) == 8);
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s (last=\"%s\")\n", __LINE__, #c, last.c_str()); return 1; } } while (0)

int main() {
    ASSERT(Once(true));
    CHECK(evals == 1 && logs == 0 && soft == 0);
    ASSERT(Once(false));
    CHECK(evals == 2 && logs == 1 && soft == 1);
    CHECK(last.find("assert-macros.cpp:") != std::string::npos && last.ends_with(": assert Once(false)"));
    int x = 7;
    std::string name = "tex";
    ASSERT_MSG(x == 1, "x={} name={}", x, name);
    CHECK(logs == 2 && last.ends_with("assert x=7 name=tex"));
    ASSERT_MSG(x == 1, "no args");
    CHECK(logs == 3 && last.ends_with("assert no args"));
    std::vector<int> v{1};
    ASSERT(v == std::vector<int>{}); // braces in the condition are not format fields
    CHECK(logs == 4 && last.ends_with("assert v == std::vector<int>{}"));
    int ran = 0;
    evals = 0;
    ASSERT_OR_EXECUTE(Once(false), { ++ran; });
    CHECK(evals == 1 && ran == 1 && logs == 5);
    ASSERT_OR_EXECUTE_MSG(Once(true), { ++ran; }, "n={}", x);
    CHECK(evals == 2 && ran == 1 && logs == 5);
    UNIMPLEMENTED_MSG("unimpl {} {}", x, 3);
    CHECK(logs == 6 && last.ends_with("assert unimpl 7 3"));
    UNIMPLEMENTED_IF_MSG(x == 7, "if {}", x);
    CHECK(logs == 7 && last.ends_with("assert if 7"));
    UNIMPLEMENTED();
    CHECK(logs == 8);
    const int r = x > 0 ? (ASSERT(x < 100), x) : 0; // usable as an expression
    CHECK(r == 7 && logs == 8);
    std::printf("PASS (last: %s)\n", last.c_str());
    UNREACHABLE_MSG("bye {}", x); // exits 0 through AssertFatalImpl
}
