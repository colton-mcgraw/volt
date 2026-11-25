#pragma once

#include <math/detail/concepts.hpp>
#include <math/detail/scalar_math.hpp>

#include <complex>
#include <type_traits>

namespace volt::math
{

    // ---------------------------------------------------------------------
    // Complex numbers
    // ---------------------------------------------------------------------
    //
    // std::complex<F> is the complex type; nothing here wraps it. This header
    // opts it in as a vector and matrix element, so phasors, impedances and
    // the complex matrices of AC analysis go through the same operators and
    // the same linear algebra as real ones:
    //
    //   matrix<std::complex<double>, 2, 2> y = ...;   // nodal admittances
    //   auto v = solve(y, currents);                  // node voltages
    //
    // Everything that needs only field operations works: the arithmetic
    // operators, the matrix products, transpose, trace, determinant,
    // inverse, solve and approx_equal. The functions that rank elements
    // (min, max, clamp, sign, ...) are not available, since complex numbers
    // have no order, and neither are std::format and std::hash, which the
    // standard library does not provide for std::complex.
    //
    // dot() on complex vectors is the plain bilinear sum of products; it
    // does not conjugate either operand.

    template <floating_numeric F>
    inline constexpr bool enable_scalar<std::complex<F>> = true;

    namespace detail
    {
        template <typename X>
        struct is_complex : std::false_type
        {
        };

        template <typename F>
        struct is_complex<std::complex<F>> : std::true_type
        {
        };

        // Tolerances and magnitudes of a complex<F> are plain F.
        template <typename F>
        struct real_of<std::complex<F>>
        {
            using type = F;
        };
    } // namespace detail

    template <typename X>
    concept complex_like = detail::is_complex<detail::bare<X>>::value;

    using complexf = std::complex<float>;
    using complexd = std::complex<double>;

    // ---- approximate comparison ------------------------------------------

    // |a - b| <= tolerance in the complex plane, for two complex values or a
    // complex and a real one. Equal values (including equal infinities) are
    // always approximately equal. Compares squared magnitudes, so it needs
    // no square root and is usable in constant evaluation.
    template <scalar_like L, scalar_like R, floating_numeric S = detail::real_t<std::common_type_t<L, R>>>
        requires(complex_like<L> || complex_like<R>)
    constexpr bool approx_equal(const L &a, const R &b, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        using C = std::common_type_t<L, R>;
        using F = std::common_type_t<detail::real_t<C>, S>;
        const C x(a);
        const C y(b);
        if (x == y)
            return true;
        const F re = static_cast<F>(x.real()) - static_cast<F>(y.real());
        const F im = static_cast<F>(x.imag()) - static_cast<F>(y.imag());
        const F limit = static_cast<F>(tolerance);
        return re * re + im * im <= limit * limit;
    }

} // namespace volt::math
