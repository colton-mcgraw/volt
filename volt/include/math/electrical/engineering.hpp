#pragma once

#include <math/electrical/constants.hpp>
#include <math/electrical/detail/decimal.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace volt::math::electrical
{

    // ---------------------------------------------------------------------
    // Engineering notation
    // ---------------------------------------------------------------------
    //
    // The conversion between the plain SI numbers the rest of the library
    // computes with and the way component values are written: a mantissa
    // and an SI prefix for a power of 1000.
    //
    //   format_engineering(4700.0)                       "4.7k"
    //   format_engineering(1e-7, {.unit = "F"})          "100nF"
    //   parse_engineering("4k7")->value                  4700.0
    //   parse_engineering("100 nF")                      {1e-7, "F"}
    //
    // The prefixes are y z a f p n u m (none) k M G T P E Z Y, 10^-24 to
    // 10^24. Micro is written as the micro sign U+00B5 in UTF-8, or as 'u'
    // when ASCII output is asked for; parsing accepts both, and the Greek
    // letter mu as well.

    // ---- decomposition ---------------------------------------------------

    // value == mantissa * 10^exponent, with exponent a multiple of 3.
    struct engineering_parts
    {
        double mantissa{};
        int exponent{};

        constexpr bool operator==(const engineering_parts &) const noexcept = default;
    };

    // Splits a finite value so that 1 <= |mantissa| < 1000. The exponent is
    // kept within [-24, 24], the range that has prefixes, so beyond it the
    // mantissa leaves that interval instead. Zero is {0, 0}.
    constexpr engineering_parts to_engineering(double value) noexcept
    {
        const double magnitude = value < 0.0 ? -value : value;
        if (!(magnitude > 0.0) || magnitude > std::numeric_limits<double>::max())
            return {value, 0};
        const int e = detail::decade(magnitude);
        // Floor to a multiple of three; e / 3 alone would truncate toward zero.
        int exponent = (e >= 0 ? e / 3 : -((2 - e) / 3)) * 3;
        exponent = exponent < -24 ? -24 : exponent > 24 ? 24 : exponent;
        return {detail::scale10(value, -exponent), exponent};
    }

    // The prefix for 10^exponent: "k" for 3, "" for 0, and "" as well for
    // an exponent that has none. Micro is "u" when ascii is set.
    constexpr std::string_view si_prefix(int exponent, bool ascii = false) noexcept
    {
        constexpr std::string_view prefixes[] = {"y", "z", "a", "f", "p", "n", "\xC2\xB5", "m", "",
                                                 "k", "M", "G", "T", "P", "E", "Z",        "Y"};
        if (exponent % 3 != 0 || exponent < -24 || exponent > 24)
            return {};
        if (exponent == -6 && ascii)
            return "u";
        return prefixes[static_cast<std::size_t>((exponent + 24) / 3)];
    }

    // ---- formatting ------------------------------------------------------

    struct engineering_format
    {
        // Digits kept in the mantissa, at least 1. Trailing zeros are
        // dropped: 4700 is "4.7k" at any setting of 2 or more.
        int significant_digits = 4;
        // Appended after the prefix: "F" gives "100nF".
        std::string_view unit{};
        // Write micro as 'u' instead of the micro sign.
        bool ascii = false;
    };

    // The value in engineering notation: mantissa, prefix, unit, with no
    // spaces. NaN and infinities are written as std::format writes them.
    inline std::string format_engineering(double value, const engineering_format &options = {})
    {
        if (!(value == value) || value > std::numeric_limits<double>::max() ||
            value < -std::numeric_limits<double>::max())
            return std::format("{}{}", value, options.unit);

        engineering_parts parts = to_engineering(value);
        const int digits =
            options.significant_digits < 1 ? 1 : options.significant_digits > 17 ? 17 : options.significant_digits;

        // Round to the requested digits first: 999.96 at four digits is
        // 1000, which belongs to the next prefix as 1.
        int decimals = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            const double magnitude = std::abs(parts.mantissa);
            const int integer_digits = magnitude >= 100.0 ? 3 : magnitude >= 10.0 ? 2 : 1;
            decimals = digits - integer_digits;
            const double step = detail::scale10(1.0, -decimals);
            parts.mantissa = std::round(parts.mantissa / step) * step;
            if (decimals < 0)
                decimals = 0;
            if (std::abs(parts.mantissa) < 1000.0 || parts.exponent >= 24)
                break;
            parts.mantissa /= 1000.0;
            parts.exponent += 3;
        }

        std::string text = std::format("{:.{}f}", parts.mantissa, decimals);
        if (text.find('.') != std::string::npos)
        {
            text.erase(text.find_last_not_of('0') + 1);
            if (text.back() == '.')
                text.pop_back();
        }
        if (text == "-0")
            text = "0";
        text += si_prefix(parts.exponent, options.ascii);
        text += options.unit;
        return text;
    }

    // ---- parsing ---------------------------------------------------------

    struct parsed_quantity
    {
        // In plain SI units: "4k7" is 4700.
        double value{};
        // Whatever followed the number and prefix, e.g. "F" for "100nF".
        // A view into the parsed text; empty when there was none.
        std::string_view unit{};
    };

    namespace detail
    {
        constexpr bool is_digit(char c) noexcept
        {
            return c >= '0' && c <= '9';
        }

        constexpr bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t';
        }

        // Reads an SI prefix at the front of text. Returns the number of
        // bytes it occupies, or 0, and sets its exponent.
        constexpr std::size_t read_prefix(std::string_view text, int &exponent) noexcept
        {
            if (text.empty())
                return 0;
            // The micro sign and the Greek mu, both two bytes in UTF-8.
            if (text.starts_with("\xC2\xB5") || text.starts_with("\xCE\xBC"))
            {
                exponent = -6;
                return 2;
            }
            switch (text.front())
            {
            case 'y':
                exponent = -24;
                return 1;
            case 'z':
                exponent = -21;
                return 1;
            case 'a':
                exponent = -18;
                return 1;
            case 'f':
                exponent = -15;
                return 1;
            case 'p':
                exponent = -12;
                return 1;
            case 'n':
                exponent = -9;
                return 1;
            case 'u':
                exponent = -6;
                return 1;
            case 'm':
                exponent = -3;
                return 1;
            case 'k':
            case 'K':
                exponent = 3;
                return 1;
            case 'M':
                exponent = 6;
                return 1;
            case 'G':
                exponent = 9;
                return 1;
            case 'T':
                exponent = 12;
                return 1;
            case 'P':
                exponent = 15;
                return 1;
            case 'E':
                exponent = 18;
                return 1;
            case 'Z':
                exponent = 21;
                return 1;
            case 'Y':
                exponent = 24;
                return 1;
            default:
                return 0;
            }
        }
    } // namespace detail

    // Reads a number with an optional SI prefix and unit:
    //
    //   "4700"  "4.7k"  "4.7 kOhm"  "1e-7"  "100nF"  "-3.3V"  "10 uF"
    //
    // and the RKM form (IEC 60062), where the prefix, or R for none, stands
    // in for the decimal point: "4k7" is 4700, "2M2" is 2.2e6, "4R7" is 4.7.
    //
    // Prefixes are case sensitive, as in SI ('m' is milli, 'M' is mega),
    // except that 'K' is accepted for kilo. The first letter after the
    // number is taken as a prefix whenever it can be one, so "1 m" is 0.001
    // with no unit: when the text carries a known unit, pass it to the
    // overload below instead. Returns nullopt for anything that is not
    // entirely a number in one of these forms. constexpr, and independent
    // of the locale: the decimal separator is always '.'.
    //
    // The value is the correctly rounded double, equal to the same number
    // written as a C++ literal, for up to 15 significant digits and a
    // magnitude between about 1e-22 and 1e+22 (zepto to zetta); outside
    // that it is within an ulp or two.
    constexpr std::optional<parsed_quantity> parse_engineering(std::string_view text) noexcept
    {
        while (!text.empty() && detail::is_space(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && detail::is_space(text.back()))
            text.remove_suffix(1);

        bool negative = false;
        if (!text.empty() && (text.front() == '+' || text.front() == '-'))
        {
            negative = text.front() == '-';
            text.remove_prefix(1);
        }

        // Digits accumulate in an integer and the decimal point moves the
        // exponent, so the final value is one exact scaling of an exact
        // mantissa. Digits past the 19th cannot change a double and only
        // shift the exponent.
        unsigned long long mantissa = 0;
        int exponent = 0;
        int kept = 0;
        bool any_digits = false;
        bool has_point = false;
        const auto take_digits = [&](bool fractional)
        {
            while (!text.empty() && detail::is_digit(text.front()))
            {
                any_digits = true;
                if (kept < 19)
                {
                    mantissa = mantissa * 10 + static_cast<unsigned long long>(text.front() - '0');
                    kept += mantissa != 0;
                    exponent -= fractional;
                }
                else
                    exponent += !fractional;
                text.remove_prefix(1);
            }
        };

        take_digits(false);
        if (!text.empty() && text.front() == '.')
        {
            has_point = true;
            text.remove_prefix(1);
            take_digits(true);
        }
        if (!any_digits)
            return std::nullopt;

        // A power of ten: e or E, then an optionally signed integer. Without
        // the integer the E is left alone, to be read as exa.
        bool has_power = false;
        if (text.size() >= 2 && (text[0] == 'e' || text[0] == 'E'))
        {
            std::string_view rest = text.substr(1);
            bool negative_power = false;
            if (rest.front() == '+' || rest.front() == '-')
            {
                negative_power = rest.front() == '-';
                rest.remove_prefix(1);
            }
            if (!rest.empty() && detail::is_digit(rest.front()))
            {
                int power = 0;
                while (!rest.empty() && detail::is_digit(rest.front()))
                {
                    if (power > 9999)
                        return std::nullopt;
                    power = power * 10 + (rest.front() - '0');
                    rest.remove_prefix(1);
                }
                exponent += negative_power ? -power : power;
                has_power = true;
                text = rest;
            }
        }

        while (!text.empty() && detail::is_space(text.front()))
            text.remove_prefix(1);

        // The prefix, or the RKM 'R'. Digits straight after either are the
        // fractional part, provided the number had no point or power of its
        // own; an R with no digits after it is not a marker but a unit.
        int prefix_exponent = 0;
        const std::size_t prefix_length = detail::read_prefix(text, prefix_exponent);
        const bool rkm_ohms = prefix_length == 0 && !text.empty() && text.front() == 'R';
        const std::size_t marker_length = rkm_ohms ? 1 : prefix_length;
        if (marker_length != 0 && text.size() > marker_length && detail::is_digit(text[marker_length]))
        {
            if (has_point || has_power)
                return std::nullopt;
            text.remove_prefix(marker_length);
            take_digits(true);
        }
        else if (!rkm_ohms)
            text.remove_prefix(prefix_length);
        exponent += prefix_exponent;

        while (!text.empty() && detail::is_space(text.front()))
            text.remove_prefix(1);
        for (const char c : text)
            if (detail::is_space(c) || detail::is_digit(c))
                return std::nullopt;

        const double magnitude = detail::scale10(static_cast<double>(mantissa), exponent);
        return parsed_quantity{negative ? -magnitude : magnitude, text};
    }

    // The same, for text whose unit is known: "10 mm" with unit "m" is
    // 0.01, and "10mil" with unit "mil" is 10 rather than a milli-"il".
    // The unit may be absent from the text, but nothing else may follow the
    // number. Returns the value alone.
    constexpr std::optional<double> parse_engineering(std::string_view text, std::string_view unit) noexcept
    {
        while (!text.empty() && detail::is_space(text.back()))
            text.remove_suffix(1);
        if (!unit.empty() && text.ends_with(unit))
            text.remove_suffix(unit.size());
        const auto parsed = parse_engineering(text);
        if (!parsed || !parsed->unit.empty())
            return std::nullopt;
        return parsed->value;
    }

} // namespace volt::math::electrical
