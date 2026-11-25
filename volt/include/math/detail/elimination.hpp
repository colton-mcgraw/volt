#pragma once

#include <math/complex.hpp>
#include <math/detail/concepts.hpp>
#include <math/detail/scalar_math.hpp>
#include <math/matrix.hpp>

#include <cstddef>
#include <limits>

// -------------------------------------------------------------------------
// Gaussian elimination
// -------------------------------------------------------------------------
//
// The elimination kernels linear_algebra.hpp drives: row swapping, partial
// pivot selection, Gauss-Jordan reduction of [a | b], and the two
// determinant strategies (partial-pivot elimination for floating and complex
// elements, Bareiss fraction-free elimination for exact ones). They all take and
// return owning matrices, and none of them throws: gauss_jordan reports a
// singular input by returning false.

namespace volt::math
{
    namespace detail
    {
        // Swaps rows i and j of an owning matrix in place.
        template <scalar_like T, std::size_t R, std::size_t C, matrix_order O>
        constexpr void swap_rows(matrix<T, R, C, O> &m, std::size_t i, std::size_t j) noexcept
        {
            if (i == j)
                return;
            for (std::size_t k = 0; k < C; ++k)
            {
                const T tmp = m(i, k);
                m(i, k) = m(j, k);
                m(j, k) = tmp;
            }
        }

        // How good a pivot v is: |v|, or |v|^2 for a complex element, which
        // ranks the same and needs no square root.
        template <scalar_like T>
        constexpr auto pivot_weight(const T &v) noexcept
        {
            if constexpr (complex_like<T>)
                return v.real() * v.real() + v.imag() * v.imag();
            else
                return abs_value(v);
        }

        // Row index in [from, N) with the largest |m(row, col)|.
        template <scalar_like T, std::size_t R, std::size_t C, matrix_order O>
        constexpr std::size_t pivot_row(const matrix<T, R, C, O> &m, std::size_t col, std::size_t from) noexcept
        {
            std::size_t best = from;
            auto best_weight = pivot_weight(m(from, col));
            for (std::size_t r = from + 1; r < R; ++r)
            {
                const auto weight = pivot_weight(m(r, col));
                if (weight > best_weight)
                {
                    best = r;
                    best_weight = weight;
                }
            }
            return best;
        }

        // Reduces the augmented system [a | b] to [I | a^-1 b] in place.
        // Returns false, leaving both operands partially reduced, when a
        // zero pivot shows a to be singular.
        template <scalar_like T, std::size_t N, std::size_t K, matrix_order OA, matrix_order OB>
        constexpr bool gauss_jordan(matrix<T, N, N, OA> &a, matrix<T, N, K, OB> &b) noexcept
        {
            for (std::size_t k = 0; k < N; ++k)
            {
                const std::size_t p = pivot_row(a, k, k);
                if (a(p, k) == T{})
                    return false;
                swap_rows(a, k, p);
                swap_rows(b, k, p);

                // Columns left of k are already those of the identity, so
                // row k is zero there and both loops over a start at k.
                const T inv = T(1) / a(k, k);
                for (std::size_t j = k; j < N; ++j)
                    a(k, j) = a(k, j) * inv;
                for (std::size_t j = 0; j < K; ++j)
                    b(k, j) = b(k, j) * inv;

                for (std::size_t i = 0; i < N; ++i)
                {
                    if (i == k)
                        continue;
                    const T f = a(i, k);
                    if (f == T{})
                        continue;
                    for (std::size_t j = k; j < N; ++j)
                        a(i, j) = a(i, j) - f * a(k, j);
                    for (std::size_t j = 0; j < K; ++j)
                        b(i, j) = b(i, j) - f * b(k, j);
                }
            }
            return true;
        }

        // Determinant by partial-pivot elimination; for floating and complex
        // elements.
        template <scalar_like T, std::size_t N, matrix_order O>
        constexpr T determinant_pivoting(matrix<T, N, N, O> m) noexcept
        {
            T result{1};
            for (std::size_t k = 0; k < N; ++k)
            {
                const std::size_t p = pivot_row(m, k, k);
                if (m(p, k) == T{})
                    return T{};
                if (p != k)
                {
                    swap_rows(m, k, p);
                    result = -result;
                }
                result = result * m(k, k);
                for (std::size_t i = k + 1; i < N; ++i)
                {
                    const T f = m(i, k) / m(k, k);
                    for (std::size_t j = k; j < N; ++j)
                        m(i, j) = m(i, j) - f * m(k, j);
                }
            }
            return result;
        }

        // Determinant by Bareiss elimination; exact for integer and fraction
        // elements.
        // Each step divides by the previous pivot, and that division is
        // exact because the entries are determinants of leading minors.
        template <scalar_like T, std::size_t N, matrix_order O>
        constexpr T determinant_bareiss(matrix<T, N, N, O> m) noexcept
        {
            T sign{1};
            T previous{1};
            for (std::size_t k = 0; k + 1 < N; ++k)
            {
                if (m(k, k) == T{})
                {
                    std::size_t p = k + 1;
                    while (p < N && m(p, k) == T{})
                        ++p;
                    if (p == N)
                        return T{};
                    swap_rows(m, k, p);
                    sign = static_cast<T>(-sign);
                }
                for (std::size_t i = k + 1; i < N; ++i)
                    for (std::size_t j = k + 1; j < N; ++j)
                        m(i, j) = static_cast<T>((m(i, j) * m(k, k) - m(i, k) * m(k, j)) / previous);
                previous = m(k, k);
            }
            return static_cast<T>(sign * m(N - 1, N - 1));
        }

        // -----------------------------------------------------------------
        // Closed forms for small matrices
        // -----------------------------------------------------------------
        //
        // The 2 x 2 minors a 4 x 4 determinant and adjugate are built from:
        // s[] from rows 0-1, c[] from rows 2-3, each over the six column
        // pairs (01, 02, 03, 12, 13, 23).
        template <typename T>
        struct minors4
        {
            T s[6];
            T c[6];

            template <typename M>
            constexpr explicit minors4(const M &m) noexcept
                : s{static_cast<T>(m(0, 0) * m(1, 1) - m(1, 0) * m(0, 1)),
                    static_cast<T>(m(0, 0) * m(1, 2) - m(1, 0) * m(0, 2)),
                    static_cast<T>(m(0, 0) * m(1, 3) - m(1, 0) * m(0, 3)),
                    static_cast<T>(m(0, 1) * m(1, 2) - m(1, 1) * m(0, 2)),
                    static_cast<T>(m(0, 1) * m(1, 3) - m(1, 1) * m(0, 3)),
                    static_cast<T>(m(0, 2) * m(1, 3) - m(1, 2) * m(0, 3))},
                  c{static_cast<T>(m(2, 0) * m(3, 1) - m(3, 0) * m(2, 1)),
                    static_cast<T>(m(2, 0) * m(3, 2) - m(3, 0) * m(2, 2)),
                    static_cast<T>(m(2, 0) * m(3, 3) - m(3, 0) * m(2, 3)),
                    static_cast<T>(m(2, 1) * m(3, 2) - m(3, 1) * m(2, 2)),
                    static_cast<T>(m(2, 1) * m(3, 3) - m(3, 1) * m(2, 3)),
                    static_cast<T>(m(2, 2) * m(3, 3) - m(3, 2) * m(2, 3))}
            {
            }

            constexpr T determinant() const noexcept
            {
                return static_cast<T>(s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] +
                                      s[5] * c[0]);
            }
        };

        // Whether 1 / det is safe to scale an adjugate by: det is finite,
        // non-zero, and far enough from underflow that the products it was
        // summed from kept their precision. Anything else (which includes
        // every singular matrix) is left to Gauss-Jordan.
        template <floating_numeric F>
        constexpr bool usable_determinant(F det) noexcept
        {
            const F magnitude = abs_value(det);
            return magnitude >= std::numeric_limits<F>::min() / std::numeric_limits<F>::epsilon() &&
                   magnitude <= std::numeric_limits<F>::max();
        }

        // Inverse of a 1 x 1 to 4 x 4 floating matrix as adjugate / det,
        // written to `out`. Several times faster than elimination, which
        // matters for the 3 x 3 and 4 x 4 transforms rendering inverts every
        // frame. Returns false, leaving `out` untouched, when the
        // determinant is not usable; the caller then falls back to
        // gauss_jordan, so singular and badly scaled inputs behave exactly
        // as they do for larger matrices.
        //
        // The source is taken by value: the results are written while it is
        // still being read, and a reference could alias `out`, which would
        // be wrong if it did and costs the optimizer every reload if it
        // merely might.
        template <floating_numeric F, std::size_t N, matrix_order O>
            requires(N <= 4)
        constexpr bool inverse_closed_form(const matrix<F, N, N, O> &m, matrix<F, N, N, O> &out) noexcept
        {
            if constexpr (N == 1)
            {
                if (!usable_determinant(m(0, 0)))
                    return false;
                out(0, 0) = F(1) / m(0, 0);
            }
            else if constexpr (N == 2)
            {
                const F det = m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0);
                if (!usable_determinant(det))
                    return false;
                const F inv = F(1) / det;
                out(0, 0) = m(1, 1) * inv;
                out(0, 1) = -m(0, 1) * inv;
                out(1, 0) = -m(1, 0) * inv;
                out(1, 1) = m(0, 0) * inv;
            }
            else if constexpr (N == 3)
            {
                const F c00 = m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1);
                const F c01 = m(1, 2) * m(2, 0) - m(1, 0) * m(2, 2);
                const F c02 = m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0);
                const F det = m(0, 0) * c00 + m(0, 1) * c01 + m(0, 2) * c02;
                if (!usable_determinant(det))
                    return false;
                const F inv = F(1) / det;
                out(0, 0) = c00 * inv;
                out(0, 1) = (m(0, 2) * m(2, 1) - m(0, 1) * m(2, 2)) * inv;
                out(0, 2) = (m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1)) * inv;
                out(1, 0) = c01 * inv;
                out(1, 1) = (m(0, 0) * m(2, 2) - m(0, 2) * m(2, 0)) * inv;
                out(1, 2) = (m(0, 2) * m(1, 0) - m(0, 0) * m(1, 2)) * inv;
                out(2, 0) = c02 * inv;
                out(2, 1) = (m(0, 1) * m(2, 0) - m(0, 0) * m(2, 1)) * inv;
                out(2, 2) = (m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)) * inv;
            }
            else
            {
                // Every element is loaded into a local before anything is
                // written, which is what lets the sixteen results vectorize.
                const F a00 = m(0, 0), a01 = m(0, 1), a02 = m(0, 2), a03 = m(0, 3);
                const F a10 = m(1, 0), a11 = m(1, 1), a12 = m(1, 2), a13 = m(1, 3);
                const F a20 = m(2, 0), a21 = m(2, 1), a22 = m(2, 2), a23 = m(2, 3);
                const F a30 = m(3, 0), a31 = m(3, 1), a32 = m(3, 2), a33 = m(3, 3);
                const F s0 = a00 * a11 - a10 * a01, s1 = a00 * a12 - a10 * a02, s2 = a00 * a13 - a10 * a03;
                const F s3 = a01 * a12 - a11 * a02, s4 = a01 * a13 - a11 * a03, s5 = a02 * a13 - a12 * a03;
                const F c0 = a20 * a31 - a30 * a21, c1 = a20 * a32 - a30 * a22, c2 = a20 * a33 - a30 * a23;
                const F c3 = a21 * a32 - a31 * a22, c4 = a21 * a33 - a31 * a23, c5 = a22 * a33 - a32 * a23;
                const F det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
                if (!usable_determinant(det))
                    return false;
                const F inv = F(1) / det;
                out(0, 0) = (a11 * c5 - a12 * c4 + a13 * c3) * inv;
                out(0, 1) = (-a01 * c5 + a02 * c4 - a03 * c3) * inv;
                out(0, 2) = (a31 * s5 - a32 * s4 + a33 * s3) * inv;
                out(0, 3) = (-a21 * s5 + a22 * s4 - a23 * s3) * inv;
                out(1, 0) = (-a10 * c5 + a12 * c2 - a13 * c1) * inv;
                out(1, 1) = (a00 * c5 - a02 * c2 + a03 * c1) * inv;
                out(1, 2) = (-a30 * s5 + a32 * s2 - a33 * s1) * inv;
                out(1, 3) = (a20 * s5 - a22 * s2 + a23 * s1) * inv;
                out(2, 0) = (a10 * c4 - a11 * c2 + a13 * c0) * inv;
                out(2, 1) = (-a00 * c4 + a01 * c2 - a03 * c0) * inv;
                out(2, 2) = (a30 * s4 - a31 * s2 + a33 * s0) * inv;
                out(2, 3) = (-a20 * s4 + a21 * s2 - a23 * s0) * inv;
                out(3, 0) = (-a10 * c3 + a11 * c1 - a12 * c0) * inv;
                out(3, 1) = (a00 * c3 - a01 * c1 + a02 * c0) * inv;
                out(3, 2) = (-a30 * s3 + a31 * s1 - a32 * s0) * inv;
                out(3, 3) = (a20 * s3 - a21 * s1 + a22 * s0) * inv;
            }
            return true;
        }
    } // namespace detail

} // namespace volt::math
