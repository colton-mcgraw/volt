#pragma once

#include <math/detail/scalar_math.hpp>
#include <math/electrical/constants.hpp>

#include <cmath>
#include <numbers>

namespace volt::math::electrical
{

    // ---------------------------------------------------------------------
    // Printed circuit traces
    // ---------------------------------------------------------------------
    //
    // Resistance, current capacity and characteristic impedance of copper
    // traces. Dimensions are metres like everything else, so a 0.2 mm trace
    // is 0.2e-3; the board-shop units convert through the constants below.
    //
    // The current and impedance functions are the published closed-form
    // approximations, not field solutions: good to a few percent inside the
    // stated ranges, and a starting point rather than a sign-off outside
    // them. They are not constexpr (pow, log).

    // ---- units -----------------------------------------------------------

    // One mil (a thousandth of an inch), metres.
    template <floating_numeric F>
    inline constexpr F mil_v = F(25.4e-6L);

    // Finished thickness of one ounce per square foot of copper foil,
    // metres: the nominal 35 um (1.378 mil) the IPC tables assume.
    template <floating_numeric F>
    inline constexpr F copper_ounce_v = F(35e-6L);

    inline constexpr double mil = mil_v<double>;
    inline constexpr double copper_ounce = copper_ounce_v<double>;

    // ---- resistance ------------------------------------------------------

    // Resistivity of copper at a temperature in degrees Celsius, ohm
    // metres, by the linear model about 20 C.
    template <floating_numeric F>
    constexpr F copper_resistivity_at(F celsius) noexcept
    {
        return copper_resistivity_v<F> * (F(1) + copper_temperature_coefficient_v<F> * (celsius - F(20)));
    }

    // DC resistance rho L / (w t) of a rectangular trace, ohms.
    template <floating_numeric F>
    constexpr F trace_resistance(F length, F width, F thickness, F celsius = F(20)) noexcept
    {
        return copper_resistivity_at(celsius) * length / (width * thickness);
    }

    // Depth at which an AC current density has fallen to 1/e of its
    // surface value, metres: sqrt(rho / (pi f mu)). Once this is small
    // against the trace thickness, the AC resistance rises above the DC one.
    template <floating_numeric F>
    constexpr F skin_depth(F frequency, F resistivity = copper_resistivity_v<F>,
                           F relative_permeability = F(1)) noexcept
    {
        return math::detail::sqrt_value(
            resistivity / (std::numbers::pi_v<F> * frequency * vacuum_permeability_v<F> * relative_permeability));
    }

    // ---- current capacity (IPC-2221) -------------------------------------
    //
    // I = k dT^0.44 A^0.725, with the cross-section A in square mils and
    // k = 0.048 for outer layers, 0.024 for inner ones, which shed heat
    // less well. The standard's charts cover up to 35 A, 10 to 100 C of
    // rise and 0.5 to 3 oz copper.

    enum class trace_layer
    {
        external,
        internal,
    };

    namespace detail
    {
        template <floating_numeric F>
        constexpr F ipc2221_k(trace_layer layer) noexcept
        {
            return layer == trace_layer::external ? F(0.048L) : F(0.024L);
        }
    } // namespace detail

    // Current, amperes, that heats the trace by temperature_rise kelvin.
    template <floating_numeric F>
    F trace_current_capacity(F width, F thickness, F temperature_rise,
                             trace_layer layer = trace_layer::external) noexcept
    {
        const F area_mils = width * thickness / (mil_v<F> * mil_v<F>);
        return detail::ipc2221_k<F>(layer) * std::pow(temperature_rise, F(0.44L)) * std::pow(area_mils, F(0.725L));
    }

    // Width, metres, a trace needs to carry `current` within that rise.
    template <floating_numeric F>
    F trace_width_for_current(F current, F thickness, F temperature_rise,
                              trace_layer layer = trace_layer::external) noexcept
    {
        const F area_mils = std::pow(current / (detail::ipc2221_k<F>(layer) * std::pow(temperature_rise, F(0.44L))),
                                     F(1) / F(0.725L));
        return area_mils * mil_v<F> * mil_v<F> / thickness;
    }

    // ---- characteristic impedance ----------------------------------------

    struct line_impedance
    {
        // Characteristic impedance, ohms.
        double z0{};
        // Effective relative permittivity: what the wave sees, between the
        // substrate's and air's for a microstrip.
        double effective_permittivity{};
    };

    // Surface microstrip: a trace of the given width, `height` above a
    // ground plane, on a substrate of relative permittivity er.
    // Hammerstad's formulas, with Wheeler's width correction when a
    // thickness is given. Within about 1% for 0.05 < width / height < 20
    // and er < 16.
    inline line_impedance microstrip_impedance(double width, double height, double relative_permittivity,
                                               double thickness = 0.0) noexcept
    {
        constexpr double pi = std::numbers::pi;
        double w = width;
        if (thickness > 0.0)
        {
            // A thick trace fringes like a slightly wider thin one.
            const double reach = width / height >= 1.0 / (2.0 * pi) ? 2.0 * height / thickness : 4.0 * pi * width / thickness;
            w += thickness / pi * (1.0 + std::log(reach));
        }
        const double u = w / height;
        const double er = relative_permittivity;
        double effective = (er + 1.0) / 2.0 + (er - 1.0) / 2.0 / std::sqrt(1.0 + 12.0 / u);
        double z0 = 0.0;
        if (u <= 1.0)
        {
            effective += (er - 1.0) / 2.0 * 0.04 * (1.0 - u) * (1.0 - u);
            z0 = 60.0 / std::sqrt(effective) * std::log(8.0 / u + u / 4.0);
        }
        else
            z0 = 120.0 * pi / (std::sqrt(effective) * (u + 1.393 + 0.667 * std::log(u + 1.444)));
        return {z0, effective};
    }

    // Symmetric stripline: a trace centred between two ground planes
    // `plane_spacing` apart, in a dielectric of relative permittivity er.
    // The IPC-2141 formula; valid for width / plane_spacing < 0.35 and
    // thickness / plane_spacing < 0.25.
    inline line_impedance stripline_impedance(double width, double plane_spacing, double relative_permittivity,
                                              double thickness = 0.0) noexcept
    {
        const double z0 = 60.0 / std::sqrt(relative_permittivity) *
                          std::log(1.9 * plane_spacing / (0.8 * width + thickness));
        return {z0, relative_permittivity};
    }

    // ---- propagation -----------------------------------------------------

    // Delay per metre of line, s/m: sqrt(er_eff) / c.
    template <floating_numeric F>
    constexpr F propagation_delay(F effective_permittivity) noexcept
    {
        return math::detail::sqrt_value(effective_permittivity) / speed_of_light_v<F>;
    }

    // Wavelength on the line at a frequency, metres.
    template <floating_numeric F>
    constexpr F wavelength(F frequency, F effective_permittivity = F(1)) noexcept
    {
        return speed_of_light_v<F> / (frequency * math::detail::sqrt_value(effective_permittivity));
    }

} // namespace volt::math::electrical
