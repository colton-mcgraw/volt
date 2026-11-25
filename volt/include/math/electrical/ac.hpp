#pragma once

#include <math/detail/scalar_math.hpp>
#include <math/electrical/constants.hpp>

#include <cmath>
#include <complex>
#include <numbers>

namespace volt::math::electrical
{

    // ---------------------------------------------------------------------
    // AC circuits
    // ---------------------------------------------------------------------
    //
    // Reactance, impedance, phasors and power, and the first- and
    // second-order time and frequency constants. Frequencies are hertz;
    // see constants.hpp for the rest of the conventions.
    //
    // Everything here is constexpr except step_response, which needs exp.

    // w = 2 pi f, rad/s.
    template <floating_numeric F>
    constexpr F angular_frequency(F frequency) noexcept
    {
        return F(2) * std::numbers::pi_v<F> * frequency;
    }

    // ---- reactance and impedance -----------------------------------------
    //
    // Reactances are signed, so that Z = R + jX holds for both elements:
    // positive for an inductor, negative for a capacitor.

    template <floating_numeric F>
    constexpr F inductive_reactance(F inductance, F frequency) noexcept
    {
        return angular_frequency(frequency) * inductance;
    }

    // -1 / (w C). At DC this is -infinity: a capacitor blocks.
    template <floating_numeric F>
    constexpr F capacitive_reactance(F capacitance, F frequency) noexcept
    {
        return F(-1) / (angular_frequency(frequency) * capacitance);
    }

    template <floating_numeric F>
    constexpr std::complex<F> resistor_impedance(F resistance) noexcept
    {
        return {resistance, F(0)};
    }

    template <floating_numeric F>
    constexpr std::complex<F> inductor_impedance(F inductance, F frequency) noexcept
    {
        return {F(0), inductive_reactance(inductance, frequency)};
    }

    template <floating_numeric F>
    constexpr std::complex<F> capacitor_impedance(F capacitance, F frequency) noexcept
    {
        return {F(0), capacitive_reactance(capacitance, frequency)};
    }

    // Y = 1 / Z, siemens. Also converts an admittance back.
    template <quantity Z>
    constexpr Z admittance(Z impedance) noexcept
    {
        return Z(1) / impedance;
    }

    // ---- phasors ---------------------------------------------------------

    // magnitude at angle `phase`, as std::polar but usable in constant
    // evaluation.
    template <floating_numeric F>
    constexpr std::complex<F> phasor(F magnitude, F phase) noexcept
    {
        return {magnitude * math::detail::cos_value(phase), magnitude * math::detail::sin_value(phase)};
    }

    // |z|, as std::abs but usable in constant evaluation. std::abs guards
    // against overflow of the intermediate squares; this does not.
    template <floating_numeric F>
    constexpr F magnitude(std::complex<F> z) noexcept
    {
        return math::detail::sqrt_value(z.real() * z.real() + z.imag() * z.imag());
    }

    // Sine-wave amplitude conversions.
    template <floating_numeric F>
    constexpr F peak_to_rms(F peak) noexcept
    {
        return peak / std::numbers::sqrt2_v<F>;
    }

    template <floating_numeric F>
    constexpr F rms_to_peak(F rms) noexcept
    {
        return rms * std::numbers::sqrt2_v<F>;
    }

    // ---- power -----------------------------------------------------------

    // Complex power S = V I* of RMS phasors: real() is the active power P in
    // watts, imag() the reactive power Q in var (positive for an inductive
    // load), and its magnitude the apparent power in VA.
    template <floating_numeric F>
    constexpr std::complex<F> complex_power(std::complex<F> voltage, std::complex<F> current) noexcept
    {
        return voltage * std::complex<F>{current.real(), -current.imag()};
    }

    // P / |S|, in [-1, 1]; 1 for a load drawing no power at all.
    template <floating_numeric F>
    constexpr F power_factor(std::complex<F> power) noexcept
    {
        const F apparent = magnitude(power);
        return apparent == F(0) ? F(1) : power.real() / apparent;
    }

    // ---- first order: RC and RL ------------------------------------------

    template <floating_numeric F>
    constexpr F rc_time_constant(F resistance, F capacitance) noexcept
    {
        return resistance * capacitance;
    }

    template <floating_numeric F>
    constexpr F rl_time_constant(F resistance, F inductance) noexcept
    {
        return inductance / resistance;
    }

    // The -3 dB corner 1 / (2 pi tau) of a first-order filter.
    template <floating_numeric F>
    constexpr F rc_cutoff_frequency(F resistance, F capacitance) noexcept
    {
        return F(1) / (F(2) * std::numbers::pi_v<F> * resistance * capacitance);
    }

    template <floating_numeric F>
    constexpr F rl_cutoff_frequency(F resistance, F inductance) noexcept
    {
        return resistance / (F(2) * std::numbers::pi_v<F> * inductance);
    }

    // Value at `time` of a first-order quantity that starts at `initial`
    // and settles toward `final_value` with the given time constant: a
    // capacitor charging through a resistor, an inductor current building.
    template <floating_numeric F>
    F step_response(F initial, F final_value, F time, F time_constant) noexcept
    {
        return final_value + (initial - final_value) * std::exp(-time / time_constant);
    }

    // ---- second order: LC and RLC ----------------------------------------

    // 1 / (2 pi sqrt(L C)), hertz.
    template <floating_numeric F>
    constexpr F resonant_frequency(F inductance, F capacitance) noexcept
    {
        return F(1) / (F(2) * std::numbers::pi_v<F> * math::detail::sqrt_value(inductance * capacitance));
    }

    // sqrt(L / C), ohms: the reactance of either element at resonance.
    template <floating_numeric F>
    constexpr F characteristic_impedance(F inductance, F capacitance) noexcept
    {
        return math::detail::sqrt_value(inductance / capacitance);
    }

    // Quality factor of a series RLC, sqrt(L / C) / R, and of a parallel
    // one, R / sqrt(L / C).
    template <floating_numeric F>
    constexpr F series_rlc_q(F resistance, F inductance, F capacitance) noexcept
    {
        return characteristic_impedance(inductance, capacitance) / resistance;
    }

    template <floating_numeric F>
    constexpr F parallel_rlc_q(F resistance, F inductance, F capacitance) noexcept
    {
        return resistance / characteristic_impedance(inductance, capacitance);
    }

} // namespace volt::math::electrical
