#include "check.hpp"

#include <math/electrical.hpp>
#include <math/elementwise.hpp>
#include <math/linear_algebra.hpp>

#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>

using namespace volt::math;
using namespace volt::math::electrical;
using namespace std::string_view_literals;

using cd = std::complex<double>;
constexpr double pi = std::numbers::pi;

// Relative comparison, for values spread over many decades.
constexpr bool close(double a, double b, double relative = 1e-9)
{
    const double scale = (a < 0 ? -a : a) > (b < 0 ? -b : b) ? (a < 0 ? -a : a) : (b < 0 ? -b : b);
    return approx_equal(a, b, relative * scale);
}

constexpr bool close(cd a, cd b, double relative = 1e-9)
{
    const double scale = magnitude(a) > magnitude(b) ? magnitude(a) : magnitude(b);
    return approx_equal(a, b, relative * scale);
}

// ---- networks ------------------------------------------------------------

static_assert(series(100.0, 220.0, 330.0) == 650.0);
static_assert(parallel(10.0, 10.0) == 5.0 && close(parallel(6.0, 3.0, 2.0), 1.0));
static_assert(parallel(100.0, 0.0) == 0.0 && parallel(cd{0, 0}, cd{5, 5}) == cd{});
static_assert(close(parallel(cd{0, 10}, cd{0, -20}), cd{0, 20})); // L and C partly cancelling
static_assert(series(1.0f, cd{0, 2}) == cd{1, 2});
static_assert(voltage_divider(10.0, 1e3, 1e3) == 5.0 && close(voltage_divider(3.3, 10e3, 4.7e3), 3.3 * 4.7 / 14.7));
static_assert(close(current_divider(3.0, 10.0, 20.0), 2.0));
static_assert(to_wye(delta{3.0, 3.0, 3.0}) == wye{1.0, 1.0, 1.0});
static_assert(to_delta(wye{1.0, 1.0, 1.0}) == delta{3.0, 3.0, 3.0});
static_assert(close(to_wye(delta{10.0, 20.0, 30.0}).a, 5.0) && close(to_wye(delta{10.0, 20.0, 30.0}).b, 10.0 / 3.0));
static_assert(close(to_delta(to_wye(delta{10.0, 20.0, 30.0})).ca, 30.0));

// ---- AC ------------------------------------------------------------------

static_assert(close(angular_frequency(50.0), 100.0 * pi));
static_assert(close(inductive_reactance(1e-3, 1e3), 2.0 * pi));
static_assert(close(capacitive_reactance(1e-6, 1e3), -159.15494309189535));
static_assert(inductor_impedance(1e-3, 1e3).real() == 0.0 && capacitor_impedance(1e-6, 1e3).imag() < 0.0);
static_assert(close(admittance(cd{3, 4}), cd{0.12, -0.16}));
static_assert(close(rc_cutoff_frequency(1e3, 1e-6), 159.15494309189535));
static_assert(close(rl_cutoff_frequency(100.0, 1e-3), 15915.494309189535));
static_assert(close(resonant_frequency(1e-3, 1e-6), 5032.921210448704));
static_assert(close(characteristic_impedance(1e-3, 1e-6), 31.622776601683793));
static_assert(close(series_rlc_q(10.0, 1e-3, 1e-6) * parallel_rlc_q(10.0, 1e-3, 1e-6), 1.0));
static_assert(close(rc_time_constant(1e3, 1e-6), 1e-3) && close(rl_time_constant(100.0, 1e-3), 1e-5));
static_assert(close(peak_to_rms(rms_to_peak(230.0)), 230.0) && close(rms_to_peak(230.0), 325.26911934581187));
static_assert(close(phasor(2.0, pi / 2), cd{0, 2}, 1e-12) && close(magnitude(cd{3, 4}), 5.0));
// 120 V across a load drawing 10 A lagging by 30 degrees.
constexpr cd load_power = complex_power(phasor(120.0, 0.0), phasor(10.0, -pi / 6));
static_assert(close(load_power, cd{1039.2304845413264, 600.0}));
static_assert(close(power_factor(load_power), 0.8660254037844387) && power_factor(cd{}) == 1.0);

// A series RLC at resonance is purely resistive.
constexpr double f0 = resonant_frequency(1e-3, 1e-6);
static_assert(close(series(resistor_impedance(10.0), inductor_impedance(1e-3, f0), capacitor_impedance(1e-6, f0)),
                    cd{10, 0}, 1e-12));

// ---- preferred values ----------------------------------------------------

static_assert(e_series_size(e_series::e96) == 96 && e_series_mantissa(e_series::e24, 23) == 910);
static_assert(e_series_mantissa(e_series::e3, 1) == 220 && e_series_mantissa(e_series::e6, 3) == 330);
static_assert(e_series_mantissa(e_series::e12, 11) == 820 && e_series_mantissa(e_series::e48, 47) == 953);
static_assert(e_series_mantissa(e_series::e96, 95) == 976 && e_series_mantissa(e_series::e192, 185) == 920);
static_assert(preferred_nearest(4500.0, e_series::e12) == 4700.0 && preferred_nearest(4200.0, e_series::e12) == 3900.0);
static_assert(preferred_floor(4500.0, e_series::e96) == 4420.0 && preferred_ceil(4500.0, e_series::e96) == 4530.0);
static_assert(preferred_nearest(5000.0, e_series::e24) == 5100.0 && preferred_nearest(1.0, e_series::e3) == 1.0);
static_assert(preferred_nearest(9900.0, e_series::e12) == 10000.0 && preferred_floor(0.99, e_series::e12) == 0.82);
static_assert(preferred_ceil(8.3, e_series::e12) == 10.0 && preferred_ceil(9.9e-10, e_series::e6) == 1e-9);
static_assert(preferred_nearest(1.23e-7, e_series::e6) == 1e-7 && preferred_nearest(2.6e6, e_series::e6) == 2.2e6);
static_assert(preferred_floor(4.7e-6, e_series::e12) == 4.7e-6 && preferred_ceil(4.7e-6, e_series::e12) == 4.7e-6);
static_assert(preferred_floor(4.7e-6f, e_series::e96) == 4.64e-6f); // 4.70 is not an E96 value
static_assert(preferred_floor(4.7e-6f, e_series::e24) == 4.7e-6f && preferred_ceil(3.3e-9f, e_series::e6) == 3.3e-9f);
static_assert(is_preferred(4700.0, e_series::e12) && !is_preferred(4800.0, e_series::e12));
static_assert(is_preferred(47.0 * 1e-7, e_series::e12) && is_preferred(1e-12, e_series::e3) && !is_preferred(0.0, e_series::e3));
static_assert(preferred_nearest(0.0, e_series::e12) == 0.0 && preferred_nearest(-5.0, e_series::e12) == -5.0);
static_assert(e_series_tolerance(e_series::e24) == 0.05);

// ---- engineering notation ------------------------------------------------

static_assert(to_engineering(4700.0) == engineering_parts{4.7, 3} && to_engineering(1e-7) == engineering_parts{100.0, -9});
static_assert(to_engineering(0.0) == engineering_parts{0.0, 0} && to_engineering(-0.05) == engineering_parts{-50.0, -3});
static_assert(to_engineering(999.0).exponent == 0 && to_engineering(1000.0).exponent == 3 && to_engineering(0.001).exponent == -3);
static_assert(to_engineering(1e30) == engineering_parts{1e6, 24});
static_assert(si_prefix(3) == "k" && si_prefix(-6, true) == "u" && si_prefix(-6) == "\xC2\xB5" && si_prefix(0).empty());

constexpr bool parses_to(std::string_view text, double value, std::string_view unit = {})
{
    const auto parsed = parse_engineering(text);
    return parsed && parsed->value == value && parsed->unit == unit;
}
static_assert(parses_to("4700", 4700.0) && parses_to("4.7k", 4700.0) && parses_to("4k7", 4700.0));
static_assert(parses_to("2M2", 2.2e6) && parses_to("4R7", 4.7) && parses_to("0R1", 0.1) && parses_to("10R", 10.0, "R"));
static_assert(parses_to("100nF", 1e-7, "F") && parses_to(" 100 nF ", 1e-7, "F") && parses_to("10uF", 1e-5, "F"));
static_assert(parses_to("10\xC2\xB5" "F", 1e-5, "F") && parses_to("10\xCE\xBC" "F", 1e-5, "F"));
static_assert(parses_to("-3.3V", -3.3, "V") && parses_to("+5", 5.0) && parses_to(".5", 0.5) && parses_to("5.", 5.0));
static_assert(parses_to("1e-7", 1e-7) && parses_to("1E3", 1000.0) && parses_to("2.5e3Hz", 2500.0, "Hz"));
static_assert(parses_to("1E", 1e18) && parses_to("4.7 k\xCE\xA9", 4700.0, "\xCE\xA9") && parses_to("1MHz", 1e6, "Hz"));
static_assert(parses_to("1mH", 1e-3, "H") && parses_to("1p", 1e-12) && parses_to("22K", 22e3) && parses_to("0", 0.0));
static_assert(parses_to("0.000001", 1e-6) && parses_to("123456789012345678901234567890", 123456789012345678901234567890.0));
static_assert(!parse_engineering("") && !parse_engineering("k") && !parse_engineering("abc") && !parse_engineering("1.2.3"));
static_assert(!parse_engineering("4.7k7") && !parse_engineering("1 2") && !parse_engineering("-") && !parse_engineering("1e5k5"));
static_assert(parse_engineering("10 mm", "m") == 0.01 && parse_engineering("10mil", "mil") == 10.0);
static_assert(parse_engineering("4k7", "\xCE\xA9") == 4700.0 && !parse_engineering("10 V", "A"));

// ---- two-ports -----------------------------------------------------------

// A quarter-wave line transforms a load to z0^2 / load.
static_assert(close(input_impedance(lossless_line(50.0, pi / 2), 100.0), cd{25, 0}, 1e-12));
// Half a wavelength repeats the load; a matched line is invisible.
static_assert(close(input_impedance(lossless_line(50.0, pi), cd{30, 40}), cd{30, 40}, 1e-12));
static_assert(close(input_impedance(lossless_line(75.0, 1.234), 75.0), cd{75, 0}, 1e-12));
static_assert(close(input_impedance(series_element(10.0) * shunt_element(0.01), 100.0), cd{60, 0}));
static_assert(close(voltage_gain(series_element(1e3), 1e3), cd{0.5, 0}));
static_assert(close(input_impedance(ideal_transformer(2.0), 50.0), cd{200, 0}));
// A series 50 ohm resistor in a 50 ohm system: S11 = 1/3, S21 = 2/3.
constexpr auto s_series = abcd_to_s(series_element(50.0), 50.0);
static_assert(close(s_series(0, 0), cd{1.0 / 3, 0}) && close(s_series(1, 0), cd{2.0 / 3, 0}));
static_assert(s_series(0, 1) == s_series(1, 0) && s_series(0, 0) == s_series(1, 1));
static_assert(close(reflection_coefficient(100.0, 50.0), cd{1.0 / 3, 0}) && close(vswr(cd{1.0 / 3, 0}), 2.0));
static_assert(reflection_coefficient(cd{50, 0}, 50.0) == cd{} && vswr(cd{}) == 1.0);

int main()
{
    // ---- non-constexpr AC and decibels ----------------------------------
    CHECK(close(step_response(0.0, 5.0, 1e-3, 1e-3), 5.0 * (1.0 - std::exp(-1.0))));
    CHECK(step_response(3.0, 5.0, 0.0, 1e-3) == 3.0);
    CHECK(close(power_to_db(2.0), 3.010299956639812) && close(db_to_power(30.0), 1000.0));
    CHECK(close(amplitude_to_db(10.0), 20.0) && close(db_to_amplitude(-6.0), 0.5011872336272722));
    CHECK(close(watts_to_dbm(1.0), 30.0) && close(dbm_to_watts(0.0), 1e-3) && close(watts_to_dbm(dbm_to_watts(-17.5)), -17.5));
    CHECK(close(volts_to_dbv(0.1), -20.0) && close(dbv_to_volts(6.0), 1.9952623149688795));
    CHECK(approx_equal(power_to_db(2.0f), 3.0103f, 1e-4f));

    // ---- every preferred value survives every query, in every decade ----
    for (const e_series series : {e_series::e3, e_series::e6, e_series::e12, e_series::e24, e_series::e48,
                                  e_series::e96, e_series::e192})
    {
        const std::size_t count = e_series_size(series);
        double previous = 0.0;
        for (int decade = -15; decade <= 15; ++decade)
            for (std::size_t i = 0; i < count; ++i)
            {
                // The literal a person would write, parsed by the C++ compiler's
                // own rules: mantissa digits and a decimal exponent.
                const std::string literal = std::to_string(e_series_mantissa(series, i)) + "e" + std::to_string(decade - 2);
                const double value = std::stod(literal);
                CHECK(value > previous);
                CHECK(is_preferred(value, series));
                CHECK(preferred_floor(value, series) == value);
                CHECK(preferred_ceil(value, series) == value);
                CHECK(preferred_nearest(value, series) == value);
                CHECK(preferred_nearest(static_cast<float>(value), series) == static_cast<float>(value));
                if (previous > 0.0)
                {
                    // Strictly between two neighbours: floor and ceil bracket,
                    // nearest picks the closer.
                    const double below_mid = previous + (value - previous) * 0.49;
                    const double above_mid = previous + (value - previous) * 0.51;
                    CHECK(preferred_floor(below_mid, series) == previous && preferred_ceil(below_mid, series) == value);
                    CHECK(preferred_nearest(below_mid, series) == previous);
                    CHECK(preferred_nearest(above_mid, series) == value);
                    CHECK(!is_preferred(below_mid, series));
                }
                previous = value;
            }
    }

    // ---- formatting -----------------------------------------------------
    CHECK(format_engineering(4700.0) == "4.7k" && format_engineering(0.0) == "0" && format_engineering(1.0) == "1");
    CHECK(format_engineering(1e-7, {.unit = "F"}) == "100nF" && format_engineering(-3.3, {.unit = "V"}) == "-3.3V");
    CHECK(format_engineering(4.7e-6, {.unit = "F"}) == "4.7\xC2\xB5" "F");
    CHECK(format_engineering(4.7e-6, {.unit = "F", .ascii = true}) == "4.7uF");
    CHECK(format_engineering(12345.678) == "12.35k" && format_engineering(12345.678, {.significant_digits = 6}) == "12.3457k");
    CHECK(format_engineering(999.96) == "1k" && format_engineering(999.94) == "999.9");
    CHECK(format_engineering(999999.9) == "1M" && format_engineering(0.00099999) == "1m");
    CHECK(format_engineering(4750.0, {.significant_digits = 2}) == "4.8k" || format_engineering(4750.0, {.significant_digits = 2}) == "4.7k");
    CHECK(format_engineering(123456.0, {.significant_digits = 1}) == "100k");
    CHECK(format_engineering(1e24) == "1Y" && format_engineering(1e27) == "1000Y" && format_engineering(2.5e-24) == "2.5y");
    CHECK(format_engineering(1e9, {.unit = "Hz"}) == "1GHz" && format_engineering(50.0, {.unit = "\xCE\xA9"}) == "50\xCE\xA9");
    CHECK(format_engineering(std::numeric_limits<double>::infinity()) == "inf");
    CHECK(format_engineering(std::numeric_limits<double>::quiet_NaN(), {.unit = "V"}) == "nanV");

    // Every E24 value in every prefixed decade round-trips through text.
    for (int decade = -24; decade <= 26; ++decade)
        for (std::size_t i = 0; i < 24; ++i)
        {
            const double value = std::stod(std::to_string(e_series_mantissa(e_series::e24, i)) + "e" + std::to_string(decade - 2));
            for (const bool ascii : {false, true})
            {
                const std::string text = format_engineering(value, {.unit = "F", .ascii = ascii});
                const auto parsed = parse_engineering(text);
                // Exact from zepto to zetta; an ulp or two beyond.
                const double tolerance = decade >= -21 && decade <= 23 ? 0.0 : 1e-15;
                CHECK(parsed && close(parsed->value, value, tolerance) && parsed->unit == "F");
                CHECK(close(parse_engineering(text, "F").value_or(0.0), value, tolerance));
            }
        }

    // ---- two-port conversions round-trip --------------------------------
    {
        const double f = 1e6;
        const two_port<double> filter = series_element(cd{5.0, 0.0} + inductor_impedance(10e-6, f)) *
                                        shunt_element(admittance(capacitor_impedance(2.2e-9, f))) *
                                        series_element(inductor_impedance(4.7e-6, f)) * shunt_element(cd{0.002, 0.0});
        // A passive reciprocal network has AD - BC = 1.
        CHECK(close(determinant(filter), cd{1, 0}));
        CHECK(approx_equal(s_to_abcd(abcd_to_s(filter, 50.0), 50.0), filter, 1e-9));
        CHECK(approx_equal(z_to_abcd(abcd_to_z(filter)), filter, 1e-9));
        CHECK(approx_equal(y_to_abcd(abcd_to_y(filter)), filter, 1e-9));
        CHECK(is_identity(abcd_to_z(filter) * abcd_to_y(filter), 1e-9));
        CHECK(approx_equal(inverse(abcd_to_z(filter)), abcd_to_y(filter), 1e-12));
        // Reciprocity shows up as S12 == S21, and S11 is the reflection
        // coefficient of the input impedance with the output matched.
        const auto s = abcd_to_s(filter, 50.0);
        CHECK(close(s(0, 1), s(1, 0)));
        CHECK(close(s(0, 0), reflection_coefficient(input_impedance(filter, 50.0), 50.0)));
        // Z11 is the input impedance with the output open.
        CHECK(close(abcd_to_z(filter)(0, 0), input_impedance(filter, 1e30), 1e-6));

        // The general line reduces to the lossless one for gamma = j beta.
        CHECK(approx_equal(transmission_line(cd{50, 0}, cd{0, 0.7}), lossless_line(50.0, 0.7), 1e-12));
        // A lossy matched line attenuates by e^(-alpha l).
        CHECK(close(std::abs(voltage_gain(transmission_line(cd{50, 0}, cd{0.1, 2.0}), 50.0)), std::exp(-0.1)));
        CHECK(close(return_loss_db(cd{1.0 / 3, 0}), 9.542425094393248) && close(mismatch_loss_db(cd{1.0 / 3, 0}), 0.5115252244738129));
    }

    // ---- nodal analysis through the complex solver ----------------------
    {
        // Two nodes: a 1 A source into node 1, R1 to ground, C between the
        // nodes, R2 from node 2 to ground. Compare with the divider formula.
        const double f = 1e3;
        const cd g1 = 1.0 / 1e3, g2 = 1.0 / 2.2e3, yc = admittance(capacitor_impedance(100e-9, f));
        const auto y = matrix<cd, 2, 2>::from_rows({{g1 + yc, -yc}, {-yc, g2 + yc}});
        const auto v = solve(y, vector<cd, 2>{cd{1, 0}, cd{}});
        const cd z_branch = series(capacitor_impedance(100e-9, f), cd{2.2e3, 0});
        const cd v1 = parallel(cd{1e3, 0}, z_branch);
        CHECK(close(v[0], v1));
        CHECK(close(v[1], voltage_divider(v1, capacitor_impedance(100e-9, f), 2.2e3)));
    }

    // ---- PCB ------------------------------------------------------------
    {
        CHECK(close(trace_resistance(10e-3, 0.2e-3, copper_ounce), 1.7241e-8 * 10e-3 / (0.2e-3 * 35e-6)));
        CHECK(close(trace_resistance(10e-3, 0.2e-3, copper_ounce, 70.0) / trace_resistance(10e-3, 0.2e-3, copper_ounce), 1.1965));
        CHECK(close(skin_depth(1e6), 66.08e-6, 1e-3));
        // 10 mil of 1 oz outer copper at a 10 C rise: 0.048 * 10^0.44 * 13.78^0.725.
        const double capacity = trace_current_capacity(10 * mil, copper_ounce, 10.0);
        CHECK(close(capacity, 0.048 * std::pow(10.0, 0.44) * std::pow(10 * 35e-6 / 25.4e-6, 0.725)));
        CHECK(capacity > 0.85 && capacity < 0.95);
        CHECK(close(trace_width_for_current(capacity, copper_ounce, 10.0), 10 * mil));
        CHECK(close(trace_current_capacity(10 * mil, copper_ounce, 10.0, trace_layer::internal), capacity / 2));
        CHECK(approx_equal(trace_width_for_current(2.0f, copper_ounce_v<float>, 20.0f),
                           static_cast<float>(trace_width_for_current(2.0, copper_ounce, 20.0)), 1e-7f));

        // The textbook 50 ohm microstrip on FR-4: width about 1.9 heights.
        const line_impedance fr4 = microstrip_impedance(1.91e-3, 1e-3, 4.4);
        CHECK(fr4.z0 > 49.5 && fr4.z0 < 50.5 && fr4.effective_permittivity > 3.2 && fr4.effective_permittivity < 3.4);
        // Narrow and wide branches meet at width == height.
        CHECK(close(microstrip_impedance(0.999999e-3, 1e-3, 4.4).z0, microstrip_impedance(1.000001e-3, 1e-3, 4.4).z0, 5e-3));
        // Wider, or thicker, means lower impedance; air means sqrt(er) faster.
        CHECK(microstrip_impedance(3e-3, 1e-3, 4.4).z0 < fr4.z0);
        CHECK(microstrip_impedance(1.91e-3, 1e-3, 4.4, 35e-6).z0 < fr4.z0);
        CHECK(close(microstrip_impedance(1e-3, 1e-3, 1.0).effective_permittivity, 1.0));
        const line_impedance strip = stripline_impedance(0.15e-3, 0.5e-3, 4.4, 35e-6);
        CHECK(close(strip.z0, 60.0 / std::sqrt(4.4) * std::log(1.9 * 0.5 / (0.8 * 0.15 + 0.035))));
        CHECK(strip.z0 > 45.0 && strip.z0 < 60.0 && strip.effective_permittivity == 4.4);
        CHECK(close(propagation_delay(4.0), 2.0 / 299792458.0) && close(wavelength(1e9), 0.299792458));
        CHECK(close(wavelength(1e9, 4.0), 0.149896229));
    }

    return volt::tests::result("electrical_tests");
}
