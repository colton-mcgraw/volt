#pragma once

#include <math/electrical/constants.hpp>

#include <cmath>

namespace volt::math::electrical
{

    // ---------------------------------------------------------------------
    // Decibels
    // ---------------------------------------------------------------------
    //
    // A decibel is ten times the log of a power ratio. Voltages and currents
    // are amplitudes, whose square is proportional to power, so the same
    // ratio is twenty times their log. None of this is constexpr: it is all
    // log10 and pow.

    // 10 log10(ratio), for a ratio of powers.
    template <floating_numeric F>
    F power_to_db(F ratio) noexcept
    {
        return F(10) * std::log10(ratio);
    }

    template <floating_numeric F>
    F db_to_power(F db) noexcept
    {
        return std::pow(F(10), db / F(10));
    }

    // 20 log10(ratio), for a ratio of voltages or currents.
    template <floating_numeric F>
    F amplitude_to_db(F ratio) noexcept
    {
        return F(20) * std::log10(ratio);
    }

    template <floating_numeric F>
    F db_to_amplitude(F db) noexcept
    {
        return std::pow(F(10), db / F(20));
    }

    // dBm: power relative to one milliwatt.
    template <floating_numeric F>
    F watts_to_dbm(F watts) noexcept
    {
        return F(10) * std::log10(watts * F(1000));
    }

    template <floating_numeric F>
    F dbm_to_watts(F dbm) noexcept
    {
        return std::pow(F(10), dbm / F(10)) / F(1000);
    }

    // dBV: RMS voltage relative to one volt.
    template <floating_numeric F>
    F volts_to_dbv(F volts) noexcept
    {
        return amplitude_to_db(volts);
    }

    template <floating_numeric F>
    F dbv_to_volts(F dbv) noexcept
    {
        return db_to_amplitude(dbv);
    }

} // namespace volt::math::electrical
