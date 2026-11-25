#pragma once

#include <cstdio>

// Minimal checking for the math tests: CHECK records a failure and keeps
// going, so one run reports everything that is wrong, and main returns
// volt::tests::result(). Anything that can be verified at compile time is a
// static_assert in the test source instead.

namespace volt::tests
{
    inline int failures = 0;

    inline void check(bool ok, const char *expression, const char *file, int line)
    {
        if (ok)
            return;
        ++failures;
        std::fprintf(stderr, "%s:%d: FAIL: %s\n", file, line, expression);
    }

    inline int result(const char *suite)
    {
        if (failures == 0)
            std::printf("%s: all checks passed\n", suite);
        else
            std::fprintf(stderr, "%s: %d check(s) failed\n", suite, failures);
        return failures == 0 ? 0 : 1;
    }
} // namespace volt::tests

#define CHECK(...) ::volt::tests::check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
