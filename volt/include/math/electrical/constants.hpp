#pragma once

#include <math/complex.hpp>
#include <math/detail/concepts.hpp>

#include <complex>
#include <numbers>
#include <type_traits>

// -------------------------------------------------------------------------
// Electrical math: conventions, the quantity concept, physical constants
// -------------------------------------------------------------------------
//
// Everything under math/electrical/ lives in volt::math::electrical and
// follows the same conventions:
//
//   * Plain SI units, unscaled: ohms, siemens, farads, henries, volts,
//     amperes, watts, hertz, seconds, metres, kelvin. A 4.7 k resistor is
//     4.7e3 and a 100 nF capacitor is 100e-9; parse_engineering and
//     format_engineering (engineering.hpp) convert at the boundary to what
//     a schematic shows. The two exceptions are spelled out in the name:
//     decibels, and temperatures taken in degrees Celsius.
//   * Frequencies are in hertz, never rad/s; angles and phases are radians.
//   * Impedances are std::complex<F> with the engineering sign convention
//     e^{+j w t}: an inductor is +jwL, a capacitor is -j/(wC).
//   * Phasors are RMS unless a function says otherwise, so P = Re(V I*).
//
// The functions are templates over the floating type, and those that make
// sense for both take either a real or a complex argument: series() adds
// resistances as readily as impedances.

namespace volt::math::electrical
{

    // A real or complex value of an electrical quantity.
    template <typename T>
    concept quantity = floating_numeric<std::remove_cvref_t<T>> || complex_like<T>;

    // The complex type a computation on the given quantities yields:
    // complex_of<float, std::complex<double>> is std::complex<double>.
    template <quantity... T>
    using complex_of = std::complex<math::detail::real_t<std::common_type_t<std::remove_cvref_t<T>...>>>;

    namespace detail
    {
        // value as an R, where R is the common type of several quantities.
        // Spelled out because the direct conversion of a float to a
        // complex<double> is an implicit promotion the warnings object to.
        template <quantity R, quantity T>
        constexpr R as(T value) noexcept
        {
            if constexpr (complex_like<T>)
                return R(value);
            else
                return R(static_cast<math::detail::real_t<R>>(value));
        }
    } // namespace detail

    // ---- physical constants ----------------------------------------------
    //
    // The first three are exact by the 2019 SI definitions; the vacuum
    // constants are the CODATA 2018 values.

    // Speed of light in vacuum, m/s.
    template <floating_numeric F>
    inline constexpr F speed_of_light_v = F(299'792'458.0L);

    // Elementary charge, C.
    template <floating_numeric F>
    inline constexpr F elementary_charge_v = F(1.602176634e-19L);

    // Boltzmann constant, J/K.
    template <floating_numeric F>
    inline constexpr F boltzmann_v = F(1.380649e-23L);

    // Vacuum permeability mu_0, H/m.
    template <floating_numeric F>
    inline constexpr F vacuum_permeability_v = F(1.25663706212e-6L);

    // Vacuum permittivity epsilon_0, F/m.
    template <floating_numeric F>
    inline constexpr F vacuum_permittivity_v = F(8.8541878128e-12L);

    // Impedance of free space sqrt(mu_0 / epsilon_0), ohms.
    template <floating_numeric F>
    inline constexpr F free_space_impedance_v = F(376.730313668L);

    // Resistivity of annealed copper at 20 C (the IACS standard), ohm metres,
    // and its temperature coefficient at 20 C, 1/K.
    template <floating_numeric F>
    inline constexpr F copper_resistivity_v = F(1.7241e-8L);

    template <floating_numeric F>
    inline constexpr F copper_temperature_coefficient_v = F(0.00393L);

    inline constexpr double speed_of_light = speed_of_light_v<double>;
    inline constexpr double elementary_charge = elementary_charge_v<double>;
    inline constexpr double boltzmann = boltzmann_v<double>;
    inline constexpr double vacuum_permeability = vacuum_permeability_v<double>;
    inline constexpr double vacuum_permittivity = vacuum_permittivity_v<double>;
    inline constexpr double free_space_impedance = free_space_impedance_v<double>;
    inline constexpr double copper_resistivity = copper_resistivity_v<double>;
    inline constexpr double copper_temperature_coefficient = copper_temperature_coefficient_v<double>;

} // namespace volt::math::electrical
