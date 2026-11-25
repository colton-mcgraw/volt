#include "check.hpp"

#include <math/complex.hpp>
#include <math/elementwise.hpp>
#include <math/fraction.hpp>
#include <math/linear_algebra.hpp>
#include <math/matrix.hpp>
#include <math/matrix_view.hpp>

#include <cmath>
#include <complex>
#include <format>
#include <iterator>
#include <random>
#include <ranges>
#include <unordered_set>

using namespace volt::math;

template <typename T, std::size_t R, std::size_t C>
using cmatrix = matrix<T, R, C, matrix_order::column_major>;

// ---- compile-time checks -------------------------------------------------

static_assert(matrix_like<mat3f> && matrix_like<const mat3f &> && matrix_like<matrix_view<float, 3, 3>>);
static_assert(!matrix_like<vec3f> && !vector_like<mat3f>);
static_assert(std::random_access_iterator<mat3f::iterator>);
static_assert(std::ranges::random_access_range<mat3f>);
static_assert(sizeof(mat4f) == 16 * sizeof(float) && std::is_trivially_copyable_v<mat4f>);
static_assert(scalar_like<std::complex<double>> && !ordered_scalar<std::complex<double>>);

constexpr auto a23 = matrix<int, 2, 3>::from_rows({{1, 2, 3}, {4, 5, 6}});
constexpr auto a23c = cmatrix<int, 2, 3>::from_rows({{1, 2, 3}, {4, 5, 6}});

static_assert(a23(1, 2) == 6 && a23c(1, 2) == 6);
static_assert(a23 == a23c && a23c == a23);
static_assert(a23.row(1) == vec3i{4, 5, 6} && a23c.column(2) == vec2i{3, 6});
static_assert(a23[1] == vec3i{4, 5, 6});  // stored line of a row-major matrix: a row
static_assert(a23c[1] == vec2i{2, 5});    // of a column-major one: a column
static_assert(a23c.data()[1] == 4 && a23.data()[1] == 2);
static_assert(a23 == (matrix<int, 2, 3>::from_columns({{1, 4}, {2, 5}, {3, 6}})));
static_assert(a23 == (matrix<int, 2, 3>::from_rows(vec3i{1, 2, 3}, vec3i{4, 5, 6})));
static_assert(transpose(a23) == (matrix<int, 3, 2>::from_rows({{1, 4}, {2, 5}, {3, 6}})));
static_assert(a23.diagonal() == vec2i{1, 5});
static_assert(a23.cast<double>()(1, 1) == 5.0);
static_assert(a23 + a23c == a23 * 2 && a23 - a23 == (matrix<int, 2, 3>::zero()));
static_assert(-a23 == a23 * -1 && 12 / (a23c + 0) == (matrix<int, 2, 3>::from_rows({{12, 6, 4}, {3, 2, 2}})));
static_assert(hadamard(a23, a23)(1, 2) == 36);
static_assert(a23 * transpose(a23) == mat2i::from_rows({{14, 32}, {32, 77}}));
static_assert(a23c * transpose(a23) == mat2i::from_rows({{14, 32}, {32, 77}}));
static_assert(a23 * vec3i{1, 1, 1} == vec2i{6, 15} && vec2i{1, 1} * a23 == vec3i{5, 7, 9});
static_assert(outer(vec2i{1, 2}, vec3i{1, 2, 3}) == (matrix<int, 2, 3>::from_rows({{1, 2, 3}, {2, 4, 6}})));
static_assert(trace(mat2i::from_rows({{1, 2}, {3, 4}})) == 5);
static_assert(submatrix<0, 1, 2, 2>(a23) == mat2i::from_rows({{2, 3}, {5, 6}}));
static_assert(minor_matrix(mat3i::from_rows({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}), 1, 1) ==
              mat2i::from_rows({{1, 3}, {7, 9}}));
static_assert(mat3i::identity() == mat3i::from_diagonal(vec3i{1, 1, 1}));
static_assert(abs(mat2i::from_rows({{-1, 2}, {3, -4}})) == mat2i::from_rows({{1, 2}, {3, 4}}));
static_assert(clamp(a23, 2, 5) == (matrix<int, 2, 3>::from_rows({{2, 2, 3}, {4, 5, 5}})));

// Determinants: closed forms, Bareiss (integer, exact) and pivoting (floating).
static_assert(determinant(mat2i::from_rows({{1, 2}, {3, 4}})) == -2);
static_assert(determinant(mat3i::from_rows({{2, 0, 1}, {1, 3, 2}, {1, 1, 1}})) == 0);
static_assert(determinant(mat4i::from_rows({{1, 2, 3, 4}, {5, 6, 7, 8}, {2, 6, 4, 8}, {3, 1, 1, 2}})) == 72);
static_assert(determinant(mat4i::from_rows({{0, 2, 3, 4}, {5, 0, 7, 8}, {2, 6, 0, 8}, {3, 1, 1, 0}})) == -464);
static_assert(approx_equal(determinant(mat4d::from_rows({{1, 2, 3, 4}, {5, 6, 7, 8}, {2, 6, 4, 8}, {3, 1, 1, 2}})), 72.0, 1e-12));
static_assert(determinant(mat4i::from_rows({{1, 2, 3, 4}, {2, 4, 6, 8}, {2, 6, 4, 8}, {3, 1, 1, 2}})) == 0);
// 5 x 5 and up: Bareiss for exact elements, pivoting for floating ones.
constexpr auto m5 = matrix<int, 5, 5>::from_rows(
    {{0, 2, 0, 1, 3}, {1, 0, 4, 0, 2}, {3, 1, 0, 2, 0}, {0, 5, 1, 0, 1}, {2, 0, 3, 4, 0}});
static_assert(determinant(m5) == 627);
static_assert(approx_equal(determinant(m5.cast<double>()), double{627}, 1e-9));
static_assert(determinant(m5.cast<fraction<long long>>() / fraction<long long>{2}) == fraction<long long>{627, 32});
static_assert(is_identity(inverse(m5) * m5, 1e-12));

// Inverse and solve, exactly, on fractions.
using frac = fraction<int>;
constexpr auto fm = mat3<frac>::from_rows({{2, 1, 0}, {1, 3, 1}, {0, 1, 4}});
static_assert(inverse(fm) * fm == mat3<frac>::identity());
static_assert(determinant(fm) == frac{18});
static_assert(solve(fm, vector<frac, 3>{3, 5, 5}) == (vector<frac, 3>{1, 1, 1}));
static_assert(!try_inverse(mat2i::from_rows({{1, 2}, {2, 4}})).has_value());
static_assert(approx_equal(inverse(mat2i::from_rows({{4, 7}, {2, 6}})), mat2d::from_rows({{0.6, -0.7}, {-0.2, 0.4}})));
static_assert(is_identity(mat3d::identity()) && is_symmetric(mat2i::from_rows({{1, 2}, {2, 1}})));
static_assert(is_diagonal(mat2i::from_rows({{1, 0}, {0, 3}})) && !is_diagonal(a23 + 1) && !is_square(a23));

// Structured bindings and write-through line views.
constexpr mat2i through_lines()
{
    mat2i m{};
    auto &[r0, r1] = m;
    r0.assign(vec2i{1, 2});
    r1.fill(7);
    m[0] += vec2i{10, 10};
    m(1, 1) = 0;
    for (auto line : m)
        line *= 2;
    return m;
}
static_assert(through_lines() == mat2i::from_rows({{22, 24}, {14, 0}}));

constexpr mat2i through_view()
{
    int raw[2][2]{{1, 2}, {3, 4}};
    matrix_view view(raw);
    view += mat2i::identity();
    view.set_column(0, vec2i{9, 9});
    matrix_view<int, 2, 2, matrix_order::column_major> flipped(raw);
    flipped(0, 1) = -1; // row 0, column 1 of the column-major reading is raw[1][0]
    return view.to_matrix();
}
static_assert(through_view() == mat2i::from_rows({{9, 2}, {-1, 5}}));

int main()
{
    std::mt19937 rng(12345);

    // ---- inverse / determinant / solve against random matrices ----------
    {
        auto run = [&]<typename M>(typename M::value_type tolerance)
        {
            using T = typename M::value_type;
            std::uniform_real_distribution<double> dist(-1.0, 1.0);
            for (int round = 0; round < 500; ++round)
            {
                M m{};
                for (std::size_t i = 0; i < M::rows; ++i)
                    for (std::size_t j = 0; j < M::cols; ++j)
                        m(i, j) = static_cast<T>(dist(rng)) + (i == j ? T(M::rows) : T(0));
                vector<T, M::rows> b{};
                for (auto &e : b)
                    e = static_cast<T>(dist(rng));
                const auto inv = inverse(m);
                CHECK(is_identity(inv * m, tolerance));
                CHECK(is_identity(m * inv, tolerance));
                CHECK(approx_equal(determinant(m) * determinant(inv), T(1), tolerance));
                CHECK(approx_equal(m * solve(m, b), b, tolerance));
                CHECK(approx_equal(determinant(transpose(m)), determinant(m), tolerance * determinant(m)));
            }
        };
        run.operator()<mat2d>(1e-12);
        run.operator()<mat3d>(1e-12);
        run.operator()<mat4d>(1e-12);
        run.operator()<matrix<double, 5, 5>>(1e-12);
        run.operator()<matrix<double, 6, 6, matrix_order::column_major>>(1e-12);
        run.operator()<mat2f>(1e-4f);
        run.operator()<mat3f>(1e-4f);
        run.operator()<mat4f>(1e-4f);
        run.operator()<cmatrix<float, 4, 4>>(1e-4f);
    }

    // ---- pivoting: a zero on the diagonal is not singular ---------------
    {
        const auto m = mat3d::from_rows({{0, 1, 0}, {0, 0, 1}, {1, 0, 0}});
        CHECK(determinant(m) == 1.0);
        CHECK(inverse(m) == transpose(m));
        const auto m4 = mat4d::from_rows({{0, 0, 0, 2}, {0, 0, 3, 0}, {0, 4, 0, 0}, {5, 0, 0, 0}});
        CHECK(determinant(m4) == 120.0);
        CHECK(is_identity(inverse(m4) * m4));
        bool threw = false;
        try
        {
            (void)inverse(mat4d::from_rows({{1, 2, 3, 4}, {2, 4, 6, 8}, {0, 1, 0, 1}, {1, 0, 1, 0}}));
        }
        catch (const singular_matrix &)
        {
            threw = true;
        }
        CHECK(threw);
        CHECK(!try_solve(mat2d::zero(), vec2d{1.0, 1.0}).has_value());
        CHECK(is_orthogonal(m) && !is_orthogonal(m4));
    }

    // ---- closed-form inverse agrees with elimination, and falls back ---
    {
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        for (int round = 0; round < 2000; ++round)
        {
            mat4d m{};
            for (std::size_t i = 0; i < 4; ++i)
                for (std::size_t j = 0; j < 4; ++j)
                    m(i, j) = dist(rng);
            if (std::abs(determinant(m)) < 1e-3)
                continue;
            const mat4d by_elimination = solve(m, mat4d::identity());
            CHECK(approx_equal(inverse(m), by_elimination, 1e-9));
            CHECK(approx_equal(inverse(submatrix<0, 0, 3, 3>(m)), solve(submatrix<0, 0, 3, 3>(m), mat3d::identity()), 1e-6));
        }
        // Entries this small put the float determinant (1e-48) below the
        // normal range; the closed form must hand over to elimination.
        const mat4f tiny = mat4f::identity() * 1e-12f;
        CHECK(approx_equal(inverse(tiny) * tiny, mat4f::identity(), 1e-5f));
        const mat4f huge = mat4f::identity() * 1e12f;
        CHECK(approx_equal(inverse(huge) * huge, mat4f::identity(), 1e-5f));
        const mat2d small = mat2d::from_rows({{1e-200, 0}, {0, 1e-200}});
        CHECK(approx_equal(inverse(small)(0, 0), 1e200, 1e188));
        CHECK(inverse(matrix<double, 1, 1>::filled(4.0))(0, 0) == 0.25);
        CHECK(!try_inverse(matrix<double, 1, 1>::zero()).has_value());
        CHECK(!try_inverse(mat3d::from_rows({{1, 2, 3}, {2, 4, 6}, {1, 0, 1}})).has_value());
    }

    // ---- several right-hand sides, mixed orders -------------------------
    {
        const auto a = cmatrix<double, 2, 2>::from_rows({{2, 1}, {1, 3}});
        const auto b = matrix<double, 2, 3>::from_rows({{3, 5, 2}, {4, 5, 1}});
        const auto x = solve(a, b);
        CHECK(approx_equal(a * x, b, 1e-12));
    }

    // ---- complex elements ----------------------------------------------
    {
        using c = std::complex<double>;
        using cm2 = matrix<c, 2, 2>;
        const cm2 z = cm2::from_rows({{c{1, 1}, c{0, 2}}, {c{3, 0}, c{1, -1}}});
        CHECK(approx_equal(determinant(z), c{2, -6}, 1e-12));
        CHECK(is_identity(inverse(z) * z, 1e-12));
        const vector<c, 2> rhs{c{1, 0}, c{0, 1}};
        CHECK(approx_equal(z * solve(z, rhs), rhs, 1e-12));
        CHECK(approx_equal(z * 2.0, z + z, 1e-12));
        CHECK(approx_equal(trace(z), c{2, 0}));
        CHECK(!try_inverse(cm2::from_rows({{c{1, 1}, c{2, 2}}, {c{2, 2}, c{4, 4}}})).has_value());

        // 5 x 5 goes through the pivoting determinant; compare with the
        // product of two determinants.
        using cm4 = matrix<c, 5, 5>;
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        for (int round = 0; round < 200; ++round)
        {
            cm4 p{}, q{};
            for (std::size_t i = 0; i < 5; ++i)
                for (std::size_t j = 0; j < 5; ++j)
                {
                    p(i, j) = c{dist(rng), dist(rng)} + (i == j ? c{4.0, 0.0} : c{});
                    q(i, j) = c{dist(rng), dist(rng)} + (i == j ? c{0.0, 4.0} : c{});
                }
            CHECK(approx_equal(determinant(p * q), determinant(p) * determinant(q), 1e-9 * std::abs(determinant(p * q))));
            CHECK(is_identity(inverse(p) * p, 1e-12));
        }
    }

    // ---- std integration ------------------------------------------------
    {
        CHECK(std::format("{}", a23) == "[1, 2, 3; 4, 5, 6]");
        CHECK(std::format("{}", a23c) == "[1, 2, 3; 4, 5, 6]");
        int raw[2][2]{{1, 2}, {3, 4}};
        CHECK(std::format("{:02}", matrix_view(raw)) == "[01, 02; 03, 04]");
        CHECK(std::hash<matrix<int, 2, 3>>{}(a23) == std::hash<cmatrix<int, 2, 3>>{}(a23c));
        std::unordered_set<mat2i> set{mat2i::identity(), mat2i::zero(), mat2i::identity()};
        CHECK(set.size() == 2);
        mat2i m = mat2i::from_rows({{1, 2}, {3, 4}});
        CHECK(std::ranges::distance(m) == 2 && (*std::next(m.begin()))[0] == 3);
        const auto copy = std::move(m)[1];
        static_assert(std::same_as<decltype(copy), const vec2i>);
        CHECK(copy == vec2i{3, 4});
    }

    return volt::tests::result("matrix_tests");
}
