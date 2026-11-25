#pragma once

#include <math/detail/scalar_math.hpp>
#include <math/electrical/ac.hpp>
#include <math/electrical/constants.hpp>
#include <math/matrix.hpp>

#include <cmath>
#include <complex>

namespace volt::math::electrical
{

    // ---------------------------------------------------------------------
    // Two-port networks
    // ---------------------------------------------------------------------
    //
    // A two-port is a 2 x 2 complex matrix, so it is an ordinary
    // math::matrix and everything in matrix.hpp and linear_algebra.hpp
    // applies to it. The working form is the ABCD (transmission) matrix,
    //
    //   [ V1 ]   [ A  B ] [ V2 ]
    //   [ I1 ] = [ C  D ] [ I2 ]
    //
    // with I2 flowing out of port 2 into the load. Its point is that a
    // cascade is a product, left to right from source to load:
    //
    //   auto filter = series_element(inductor_impedance(l, f)) *
    //                 shunt_element(admittance(capacitor_impedance(c, f)));
    //   auto z_in = input_impedance(filter, load);
    //
    // The conversions give the same network as S (scattering), Z
    // (impedance) or Y (admittance) parameters; Z and Y are each other's
    // inverse, so math::inverse converts between those two.

    template <floating_numeric F>
    using two_port = matrix<std::complex<F>, 2, 2>;

    // ---- building blocks -------------------------------------------------

    // An impedance in series with the signal path.
    template <quantity Z>
    constexpr two_port<math::detail::real_t<Z>> series_element(Z impedance) noexcept
    {
        using C = complex_of<Z>;
        return two_port<math::detail::real_t<Z>>::from_rows({{C(1), detail::as<C>(impedance)}, {C(0), C(1)}});
    }

    // An admittance from the signal path to ground.
    template <quantity Y>
    constexpr two_port<math::detail::real_t<Y>> shunt_element(Y admittance) noexcept
    {
        using C = complex_of<Y>;
        return two_port<math::detail::real_t<Y>>::from_rows({{C(1), C(0)}, {detail::as<C>(admittance), C(1)}});
    }

    // An ideal transformer with turns ratio n = N1 / N2: V1 = n V2.
    template <floating_numeric F>
    constexpr two_port<F> ideal_transformer(F turns_ratio) noexcept
    {
        using C = std::complex<F>;
        return two_port<F>::from_rows({{C(turns_ratio), C(0)}, {C(0), C(F(1) / turns_ratio)}});
    }

    // A lossless transmission line of characteristic impedance z0 and
    // electrical length beta * l in radians (2 pi per wavelength).
    template <floating_numeric F>
    constexpr two_port<F> lossless_line(F z0, F electrical_length) noexcept
    {
        using C = std::complex<F>;
        const F c = math::detail::cos_value(electrical_length);
        const F s = math::detail::sin_value(electrical_length);
        return two_port<F>::from_rows({{C(c), C(F(0), z0 * s)}, {C(F(0), s / z0), C(c)}});
    }

    // A general line: complex characteristic impedance, and gamma * l with
    // gamma = alpha + j beta the propagation constant. Not constexpr.
    template <floating_numeric F>
    two_port<F> transmission_line(std::complex<F> z0, std::complex<F> gamma_length) noexcept
    {
        const std::complex<F> c = std::cosh(gamma_length);
        const std::complex<F> s = std::sinh(gamma_length);
        return two_port<F>::from_rows({{c, z0 * s}, {s / z0, c}});
    }

    // ---- terminated behaviour --------------------------------------------

    // Impedance seen at port 1 with `load` across port 2.
    template <floating_numeric F, quantity Z>
    constexpr std::complex<F> input_impedance(const two_port<F> &abcd, Z load) noexcept
    {
        const std::complex<F> zl = detail::as<std::complex<F>>(load);
        return (abcd(0, 0) * zl + abcd(0, 1)) / (abcd(1, 0) * zl + abcd(1, 1));
    }

    // V2 / V1 with `load` across port 2.
    template <floating_numeric F, quantity Z>
    constexpr std::complex<F> voltage_gain(const two_port<F> &abcd, Z load) noexcept
    {
        const std::complex<F> zl = detail::as<std::complex<F>>(load);
        return zl / (abcd(0, 0) * zl + abcd(0, 1));
    }

    // ---- parameter conversions -------------------------------------------
    //
    // S parameters are referred to a real reference impedance z0, the same
    // at both ports. Each conversion divides by one parameter of its
    // source, and yields infinities or NaN when the target form does not
    // exist for that network: a lone series element has no Z parameters.

    template <floating_numeric F>
    constexpr two_port<F> abcd_to_s(const two_port<F> &abcd, F z0) noexcept
    {
        using C = std::complex<F>;
        const C a = abcd(0, 0), b = abcd(0, 1) / z0, c = abcd(1, 0) * z0, d = abcd(1, 1);
        const C sum = a + b + c + d;
        return two_port<F>::from_rows(
            {{(a + b - c - d) / sum, F(2) * (a * d - b * c) / sum}, {C(2) / sum, (-a + b - c + d) / sum}});
    }

    template <floating_numeric F>
    constexpr two_port<F> s_to_abcd(const two_port<F> &s, F z0) noexcept
    {
        using C = std::complex<F>;
        const C s11 = s(0, 0), s12 = s(0, 1), s21 = s(1, 0), s22 = s(1, 1);
        const C one(1);
        const C cross = s12 * s21;
        const C twice = F(2) * s21;
        return two_port<F>::from_rows(
            {{((one + s11) * (one - s22) + cross) / twice, z0 * ((one + s11) * (one + s22) - cross) / twice},
             {((one - s11) * (one - s22) - cross) / (z0 * twice), ((one - s11) * (one + s22) + cross) / twice}});
    }

    template <floating_numeric F>
    constexpr two_port<F> abcd_to_z(const two_port<F> &abcd) noexcept
    {
        using C = std::complex<F>;
        const C a = abcd(0, 0), b = abcd(0, 1), c = abcd(1, 0), d = abcd(1, 1);
        return two_port<F>::from_rows({{a / c, (a * d - b * c) / c}, {C(1) / c, d / c}});
    }

    template <floating_numeric F>
    constexpr two_port<F> z_to_abcd(const two_port<F> &z) noexcept
    {
        using C = std::complex<F>;
        const C z11 = z(0, 0), z12 = z(0, 1), z21 = z(1, 0), z22 = z(1, 1);
        return two_port<F>::from_rows({{z11 / z21, (z11 * z22 - z12 * z21) / z21}, {C(1) / z21, z22 / z21}});
    }

    template <floating_numeric F>
    constexpr two_port<F> abcd_to_y(const two_port<F> &abcd) noexcept
    {
        using C = std::complex<F>;
        const C a = abcd(0, 0), b = abcd(0, 1), c = abcd(1, 0), d = abcd(1, 1);
        return two_port<F>::from_rows({{d / b, (b * c - a * d) / b}, {C(-1) / b, a / b}});
    }

    template <floating_numeric F>
    constexpr two_port<F> y_to_abcd(const two_port<F> &y) noexcept
    {
        using C = std::complex<F>;
        const C y11 = y(0, 0), y12 = y(0, 1), y21 = y(1, 0), y22 = y(1, 1);
        return two_port<F>::from_rows({{-y22 / y21, C(-1) / y21}, {(y12 * y21 - y11 * y22) / y21, -y11 / y21}});
    }

    // ---- matching --------------------------------------------------------

    // Gamma = (Zl - Z0) / (Zl + Z0): the fraction of an incident voltage
    // wave a load reflects back into a line of impedance z0.
    template <quantity Zl, quantity Z0>
    constexpr complex_of<Zl, Z0> reflection_coefficient(Zl load, Z0 z0) noexcept
    {
        using C = complex_of<Zl, Z0>;
        return (detail::as<C>(load) - detail::as<C>(z0)) / (detail::as<C>(load) + detail::as<C>(z0));
    }

    // Voltage standing wave ratio (1 + |Gamma|) / (1 - |Gamma|): 1 when
    // matched, infinite for total reflection.
    template <floating_numeric F>
    constexpr F vswr(std::complex<F> reflection) noexcept
    {
        const F rho = magnitude(reflection);
        return (F(1) + rho) / (F(1) - rho);
    }

    // -20 log10 |Gamma|, in dB: how far below the incident wave the
    // reflected one is. Larger is better; infinite when matched.
    template <floating_numeric F>
    F return_loss_db(std::complex<F> reflection) noexcept
    {
        return F(-20) * std::log10(std::abs(reflection));
    }

    // -10 log10(1 - |Gamma|^2), in dB: the power lost to the reflection.
    template <floating_numeric F>
    F mismatch_loss_db(std::complex<F> reflection) noexcept
    {
        return F(-10) * std::log10(F(1) - std::norm(reflection));
    }

} // namespace volt::math::electrical
