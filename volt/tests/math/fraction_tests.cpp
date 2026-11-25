#include "check.hpp"

#include <math/fraction.hpp>

#include <cmath>
#include <compare>
#include <cstdint>
#include <format>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_set>

using namespace volt::math;

using frac = fraction<int>;
using frac64 = fraction<std::int64_t>;
using ufrac = fraction<unsigned>;

// ---- compile-time checks -------------------------------------------------

static_assert(frac{6, -8} == frac{-3, 4} && frac{0, -5} == frac{} && frac{6, -8}.denominator == 4);
static_assert(frac{1, 2} + frac{1, 3} == frac{5, 6} && frac{1, 2} - frac{1, 3} == frac{1, 6});
static_assert(frac{2, 3} * frac{3, 4} == frac{1, 2} && frac{2, 3} / frac{4, 3} == frac{1, 2});
static_assert(frac{1, 2} + 1 == frac{3, 2} && 2 * frac{1, 4} == frac{1, 2} && 1 - frac{1, 4} == frac{3, 4});
static_assert(frac{1, 3} < frac{1, 2} && frac{-1, 2} < frac{-1, 3} && frac{7, 2} > 3 && frac{4, 2} == 2);
static_assert((frac{1, 3} <=> frac{2, 6}) == std::strong_ordering::equal);
static_assert(frac{7, 2}.floor() == 3 && frac{7, 2}.ceil() == 4 && frac{7, 2}.trunc() == 3 && frac{7, 2}.round() == 4);
static_assert(frac{-7, 2}.floor() == -4 && frac{-7, 2}.ceil() == -3 && frac{-7, 2}.trunc() == -3);
static_assert(frac{-7, 2}.round() == -4 && frac{-5, 3}.round() == -2 && frac{-4, 3}.round() == -1 && frac{5, 2}.round() == 3);
static_assert(frac{2, 3}.pow(3) == frac{8, 27} && frac{2, 3}.pow(-2) == frac{9, 4} && frac{5, 7}.pow(0) == 1);
static_assert(frac{-3, 4}.abs() == frac{3, 4} && frac{-3, 4}.reciprocal() == frac{-4, 3} && -frac{1, 2} == frac{-1, 2});
static_assert(frac{3, 4}.to_floating() == 0.75 && static_cast<double>(frac{1, 8}) == 0.125);
static_assert(frac{4, 2}.is_integer() && !frac{1, 2}.is_integer() && frac{-1, 2}.is_negative());

// Cross-cancellation keeps intermediates in range where a naive product overflows.
constexpr int big = std::numeric_limits<int>::max();
static_assert(frac{big, 3} * frac{3, big} == 1);
static_assert(frac{1, big} + frac{1, big} == frac{2, big});
static_assert(frac{big - 1, big} < frac{big, big - 1} && frac{big - 2, big - 1} < frac{big - 1, big});

// from_floating finds the simplest fraction, not the binary expansion.
static_assert(frac::from_floating(0.75) == frac{3, 4} && frac::from_floating(0.1) == frac{1, 10});
static_assert(frac::from_floating(-2.5) == frac{-5, 2} && frac::from_floating(3.0) == 3 && frac::from_floating(0.0) == 0);
static_assert(frac::from_floating(3.14159265358979, 1000) == frac{355, 113});
static_assert(frac64::from_floating(1.0 / 3.0) == frac64{1, 3});
static_assert(ufrac::from_floating(0.5f) == ufrac{1u, 2u});
static_assert(ufrac{6u, 4u} == ufrac{3u, 2u} && ufrac{1u, 3u} < ufrac{1u, 2u} && ufrac{7u, 2u}.round() == 4u);

template <typename Exception, typename F>
bool throws(F &&f)
{
    try
    {
        f();
    }
    catch (const Exception &)
    {
        return true;
    }
    return false;
}

int main()
{
    // ---- errors ---------------------------------------------------------
    CHECK(throws<std::domain_error>([] { (void)frac{1, 0}; }));
    CHECK(throws<std::domain_error>([] { (void)(frac{1, 2} / frac{0}); }));
    CHECK(throws<std::domain_error>([] { (void)frac{0}.reciprocal(); }));
    CHECK(throws<std::domain_error>([] { (void)frac{0}.pow(-1); }));
    CHECK(throws<std::overflow_error>([] { (void)frac{std::numeric_limits<long long>::max()}; }));
    CHECK(throws<std::overflow_error>([] { (void)frac::from_floating(1e30); }));
    CHECK(throws<std::overflow_error>([] { (void)ufrac::from_floating(-0.5); }));
    CHECK(throws<std::invalid_argument>([] { (void)frac::from_floating(std::numeric_limits<double>::quiet_NaN()); }));
    CHECK(throws<std::invalid_argument>([] { (void)frac::from_floating(std::numeric_limits<double>::infinity()); }));
    CHECK(throws<std::invalid_argument>([] { (void)frac::from_floating(0.5, 0); }));

    // ---- differential: arithmetic and ordering against wide integers ----
    std::mt19937 rng(99);
    std::uniform_int_distribution<int> num(-2000, 2000);
    std::uniform_int_distribution<int> den(1, 2000);
    for (int round = 0; round < 20000; ++round)
    {
        const int a = num(rng), b = den(rng), c = num(rng), d = den(rng);
        const frac x{a, b}, y{c, d};
        const long long lhs = static_cast<long long>(a) * d;
        const long long rhs = static_cast<long long>(c) * b;

        CHECK((x <=> y) == (lhs <=> rhs));
        CHECK((x == y) == (lhs == rhs));
        CHECK(x.is_reduced() && (x + y).is_reduced() && (x * y).is_reduced());

        // x + y == (ad + cb) / bd, checked by cross-multiplying in 64 bits.
        const frac s = x + y;
        CHECK(static_cast<long long>(s.numerator) * b * d == (lhs + rhs) * s.denominator);
        const frac p = x * y;
        CHECK(static_cast<long long>(p.numerator) * b * d == static_cast<long long>(a) * c * p.denominator);
        CHECK(x - y + y == x);
        if (c != 0)
            CHECK(x / y * y == x);

        const double value = static_cast<double>(a) / b;
        CHECK(x.floor() == static_cast<int>(std::floor(value)));
        CHECK(x.ceil() == static_cast<int>(std::ceil(value)));
        CHECK(x.trunc() == static_cast<int>(std::trunc(value)));
        // round: compare exactly, 2a/b against odd integers, instead of via double.
        const int r = x.round();
        CHECK(std::abs(2LL * a - 2LL * r * b) <= b);
        CHECK(frac::from_floating(value) == x);
        CHECK(std::hash<frac>{}(x) == std::hash<frac>{}(frac{a * 3, b * 3}));
    }

    // ---- std integration ------------------------------------------------
    CHECK(std::format("{}", frac{3, 4}) == "3/4" && std::format("{}", frac{8, 4}) == "2" &&
          std::format("{}", frac{-1, 2}) == "-1/2");
    std::unordered_set<frac> set{frac{1, 2}, frac{2, 4}, frac{1, 3}};
    CHECK(set.size() == 2);

    return volt::tests::result("fraction_tests");
}
