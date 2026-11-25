#include "check.hpp"

#include <math/elementwise.hpp>
#include <math/fraction.hpp>
#include <math/geometry.hpp>
#include <math/scalar.hpp>
#include <math/vector.hpp>
#include <math/vector_view.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <numbers>
#include <ranges>
#include <unordered_set>

using namespace volt::math;

// ---- compile-time checks -------------------------------------------------

static_assert(vector_like<vec3f> && vector_like<const vec3f &> && vector_like<vector_view<float, 3>>);
static_assert(!vector_like<float> && !vector_like<fraction<int>>);
static_assert(std::contiguous_iterator<vec3f::iterator>);
static_assert(std::ranges::contiguous_range<vec3f>);
static_assert(std::ranges::contiguous_range<vector_view<float, 3>>);
static_assert(!scalar_like<bool> && !scalar_like<char> && scalar_like<signed char> && scalar_like<fraction<int>>);
static_assert(ordered_scalar<int> && ordered_scalar<fraction<int>>);

static_assert(std::same_as<decltype(vector{1, 2.0}), vector<double, 2>>);
static_assert(std::same_as<decltype(vec2i{} + vec2f{}), vec2f>);
static_assert(std::same_as<decltype(vector<short, 2>{} + vector<short, 2>{}), vector<short, 2>>);
static_assert(std::same_as<decltype(magnitude(vec2i{})), double>);
static_assert(std::same_as<decltype(magnitude(vec2f{})), float>);

static_assert(vec3i{1, 2, 3} + vec3i{4, 5, 6} == vec3i{5, 7, 9});
static_assert(vec3i{1, 2, 3} * 2 == vec3i{2, 4, 6} && 2 * vec3i{1, 2, 3} == vec3i{2, 4, 6});
static_assert(10 - vec3i{1, 2, 3} == vec3i{9, 8, 7});
static_assert(-vec3i{1, -2, 3} == vec3i{-1, 2, -3});
static_assert(vec3i{1, 2, 3}.swizzle<2, 1, 0>() == vec3i{3, 2, 1});
static_assert(vec4i{1, 2, 3, 4}.zw() == vec2i{3, 4});
static_assert(vec3i::axis<1>() == vec3i{0, 1, 0} && vec3i::filled(7) == vec3i{7, 7, 7});
static_assert(dot(vec3i{1, 2, 3}, vec3i{4, 5, 6}) == 32);
static_assert(cross(vec3i{1, 0, 0}, vec3i{0, 1, 0}) == vec3i{0, 0, 1});
static_assert(sum(vec3i{1, 2, 3}) == 6 && product(vec3i{2, 3, 4}) == 24);
static_assert(min_value(vec3i{3, 1, 2}) == 1 && max_value(vec3i{3, 1, 2}) == 3);
static_assert(magnitude(vec2i{3, 4}) == 5.0);
static_assert(concat(vec2i{1, 2}, 3, vec2i{4, 5}) == vector<int, 5>{1, 2, 3, 4, 5});
static_assert(resize<4>(vec2i{1, 2}, 9) == vec4i{1, 2, 9, 9} && resize<2>(vec4i{1, 2, 3, 4}) == vec2i{1, 2});
static_assert(abs(vec3i{-1, 2, -3}) == vec3i{1, 2, 3});
static_assert(abs(vector<short, 2>{-1, 2}) == vector<short, 2>{1, 2});
static_assert(sign(vec3i{-5, 0, 5}) == vec3i{-1, 0, 1});
static_assert(clamp(vec3i{-5, 0, 5}, -1, 1) == vec3i{-1, 0, 1});
static_assert(min(vec2i{1, 5}, vec2i{3, 2}) == vec2i{1, 2} && max(vec2i{1, 5}, vec2i{3, 2}) == vec2i{3, 5});
static_assert(floor(vec2d{1.5, -1.5}) == vec2d{1.0, -2.0} && ceil(vec2d{1.5, -1.5}) == vec2d{2.0, -1.0});
static_assert(round(vec2d{2.5, -2.5}) == vec2d{3.0, -3.0} && trunc(vec2d{1.9, -1.9}) == vec2d{1.0, -1.0});
static_assert(step(1, vec3i{0, 1, 2}) == vec3i{0, 1, 1});
static_assert(sqrt(vec2i{4, 9}) == vec2d{2.0, 3.0});

// The integer cases that used to overflow or truncate.
static_assert(magnitude(vec2i{100'000'000, 0}) == 1e8);
static_assert(distance(vector<unsigned, 2>{0u, 0u}, vector<unsigned, 2>{3u, 4u}) == 5.0);
static_assert(distance(vec2i{std::numeric_limits<int>::min(), 0}, vec2i{std::numeric_limits<int>::max(), 0}) ==
              4294967295.0);
static_assert(distance_squared<long long>(vec2i{-1'000'000'000, 0}, vec2i{1'000'000'000, 0}) ==
              4'000'000'000'000'000'000ll);
static_assert(distance_squared(vec2i{1, 1}, vec2i{4, 5}) == 25);
static_assert(project(vec2i{3, 4}, vec2i{2, 0}) == vec2d{3.0, 0.0});
static_assert(reject(vec2i{3, 4}, vec2i{2, 0}) == vec2d{0.0, 4.0});
static_assert(reflect(vec2i{1, -1}, vec2i{0, 1}) == vec2i{1, 1});

// Exact elements.
static_assert(dot(vector{fraction<int>{1, 2}, fraction<int>{1, 3}}, vector{fraction<int>{2}, fraction<int>{3}}) ==
              fraction<int>{2});
static_assert(project(vector{fraction<int>{1}, fraction<int>{1}}, vector{fraction<int>{3}, fraction<int>{0}}) ==
              vector{fraction<int>{1}, fraction<int>{0}});

// Structured bindings.
constexpr int bindings_sum()
{
    vec3i v{1, 2, 3};
    auto &[x, y, z] = v;
    x = 10;
    const auto [a, b, c] = v;
    return a + b + c + y + z;
}
static_assert(bindings_sum() == 20);

// Views write through and never own.
constexpr vec3i through_view()
{
    int raw[3]{1, 2, 3};
    vector_view view(raw);
    view += vec3i{10, 10, 10};
    view *= 2;
    view.z() = -1;
    auto [x, y, z] = view;
    y = 0;
    return view.to_vector();
}
static_assert(through_view() == vec3i{22, 0, -1});

int main()
{
    // ---- views in every function that takes a vector_like ---------------
    {
        float raw_a[3]{1.0f, 0.0f, 0.0f};
        float raw_b[3]{0.0f, 1.0f, 0.0f};
        const vector_view a(raw_a);
        const vector_view b(raw_b);
        CHECK(cross(a, b) == vec3f{0.0f, 0.0f, 1.0f});
        CHECK(a == vec3f{1.0f, 0.0f, 0.0f} && vec3f{1.0f, 0.0f, 0.0f} == a && a != b);
        CHECK(approx_equal(refract(a, b, 1.0f), vec3f{1.0f, 0.0f, 0.0f}));
        CHECK(a.cast<double>() == vec3d{1.0, 0.0, 0.0});
        CHECK(approx_equal(angle_between(a, b), std::numbers::pi_v<float> / 2.0f, 1e-6f));
        const vector_view<const float, 3> ca = a;
        CHECK(ca == a && lerp(ca, b, 0.5f) == vec3f{0.5f, 0.5f, 0.0f});
    }

    // ---- geometry at run time -------------------------------------------
    {
        const vec3d v{1.0, 2.0, 2.0};
        CHECK(magnitude(v) == 3.0);
        CHECK(approx_equal(magnitude(normalized(v)), 1.0));
        CHECK(approx_equal(distance(vec3d{1.0, 1.0, 1.0}, vec3d{2.0, 3.0, 3.0}), 3.0));
        CHECK(approx_equal(reflect(vec3d{1.0, -1.0, 0.0}, vec3d{0.0, 1.0, 0.0}), vec3d{1.0, 1.0, 0.0}));
        CHECK(approx_equal(angle_between(vec2i{1, 0}, vec2i{0, 5}), std::numbers::pi / 2));
        CHECK(approx_equal(angle_between(vec2d{1.0, 0.0}, vec2d{-2.0, 0.0}), std::numbers::pi));
        // Snell: 45 degrees into a medium with eta = 1 / 1.5.
        const vec2d incident = normalized(vec2d{1.0, -1.0});
        const vec2d out = refract(incident, vec2d{0.0, 1.0}, 1.0 / 1.5);
        CHECK(approx_equal(magnitude(out), 1.0, 1e-12));
        CHECK(approx_equal(out.x(), std::sin(std::numbers::pi / 4) / 1.5, 1e-12));
        // Total internal reflection.
        CHECK(refract(incident, vec2d{0.0, 1.0}, 1.5) == vec2d{});
        CHECK(approx_equal(pow(vec2d{2.0, 3.0}, 2), vec2d{4.0, 9.0}));
        CHECK(approx_equal(exp(vec2d{0.0, 1.0}), vec2d{1.0, std::numbers::e}));
        CHECK(approx_equal(smoothstep(0.0, 1.0, vec3d{-1.0, 0.5, 2.0}), vec3d{0.0, 0.5, 1.0}));
        CHECK(saturate(vec3d{-1.0, 0.5, 2.0}) == vec3d{0.0, 0.5, 1.0});
        CHECK(is_zero(vec3d{0.0, 1e-15, 0.0}) && !is_zero(vec3d{0.0, 1e-3, 0.0}));
    }

    // ---- compile-time and run-time kernels agree ------------------------
    {
        static constexpr double values[]{0.0, 1e-9, 0.5, 1.0, 2.0, 3.0, 10.0, 12345.678, 1e10, 1e-300, 1e300};
        static constexpr auto roots = []
        {
            vector<double, std::size(values)> r{};
            for (std::size_t i = 0; i < std::size(values); ++i)
                r[i] = detail::sqrt_value(values[i]);
            return r;
        }();
        for (std::size_t i = 0; i < std::size(values); ++i)
            CHECK(std::abs(roots[i] - std::sqrt(values[i])) <= 2 * std::numeric_limits<double>::epsilon() * roots[i]);

        static constexpr double angles[]{-7.0, -3.14159, -1.0, -1e-8, 0.0, 0.3, 1.0, 1.5707963, 2.0, 3.0, 6.0, 100.0};
        static constexpr auto sines = []
        {
            vector<double, std::size(angles)> r{};
            for (std::size_t i = 0; i < std::size(angles); ++i)
                r[i] = detail::sin_value(angles[i]);
            return r;
        }();
        static constexpr auto cosines = []
        {
            vector<double, std::size(angles)> r{};
            for (std::size_t i = 0; i < std::size(angles); ++i)
                r[i] = detail::cos_value(angles[i]);
            return r;
        }();
        for (std::size_t i = 0; i < std::size(angles); ++i)
        {
            CHECK(std::abs(sines[i] - std::sin(angles[i])) < 1e-14);
            CHECK(std::abs(cosines[i] - std::cos(angles[i])) < 1e-14);
        }

        static constexpr double rounding[]{-2.5, -2.0, -1.5, -0.5, -0.0, 0.0, 0.49999999999999994, 0.5, 1.5, 2.5, 1e15 + 0.5, -1e18};
        static constexpr auto rounded = []
        {
            vector<double, 4 * std::size(rounding)> r{};
            for (std::size_t i = 0; i < std::size(rounding); ++i)
            {
                r[4 * i + 0] = detail::floor_value(rounding[i]);
                r[4 * i + 1] = detail::ceil_value(rounding[i]);
                r[4 * i + 2] = detail::trunc_value(rounding[i]);
                r[4 * i + 3] = detail::round_value(rounding[i]);
            }
            return r;
        }();
        for (std::size_t i = 0; i < std::size(rounding); ++i)
        {
            CHECK(rounded[4 * i + 0] == std::floor(rounding[i]));
            CHECK(rounded[4 * i + 1] == std::ceil(rounding[i]));
            CHECK(rounded[4 * i + 2] == std::trunc(rounding[i]));
            CHECK(rounded[4 * i + 3] == std::round(rounding[i]));
        }
    }

    // ---- std integration ------------------------------------------------
    {
        CHECK(std::format("{}", vec3i{1, 2, 3}) == "(1, 2, 3)");
        CHECK(std::format("{:.1f}", vec2d{1.25, 2.0}) == "(1.2, 2.0)" || std::format("{:.1f}", vec2d{1.25, 2.0}) == "(1.3, 2.0)");
        int raw[2]{4, 5};
        CHECK(std::format("{}", vector_view(raw)) == "(4, 5)");
        std::unordered_set<vec2i> set{vec2i{1, 2}, vec2i{2, 1}, vec2i{1, 2}};
        CHECK(set.size() == 2);
        vec4i v{4, 3, 2, 1};
        std::ranges::sort(v);
        CHECK(v == vec4i{1, 2, 3, 4});
        CHECK(*std::ranges::max_element(v) == 4 && v.rbegin()[0] == 4 && v.at(3) == 4);
        bool threw = false;
        try
        {
            (void)v.at(4);
        }
        catch (const std::out_of_range &)
        {
            threw = true;
        }
        CHECK(threw);
    }

    return volt::tests::result("vector_tests");
}
