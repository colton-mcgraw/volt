#pragma once

// -------------------------------------------------------------------------
// Decimal scaling
// -------------------------------------------------------------------------
//
// Component values are decimal: 4.7 k, 100 n, 0.1 u. Building them by
// repeated multiplication, or through pow and log10, lands an ulp or two
// away from the value the same number written as a literal has, and then
// 4.7e3 does not compare equal to "4k7". These two helpers keep the decimal
// arithmetic exact enough to avoid that, and are constexpr.

namespace volt::math::electrical::detail
{

    // 10^0 .. 10^22: every power of ten a double holds exactly.
    inline constexpr double exact_power_of_ten[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                                    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                                    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

    // mantissa * 10^exponent. For |exponent| <= 22 this is one multiplication
    // or division by an exact power of ten, so when the mantissa is exact
    // too (any integer below 2^53) the result is correctly rounded: it is
    // the double the decimal literal would give. Beyond that range (values
    // past roughly 1e-22 and 1e+22 once the digits are counted) it takes
    // two steps and is accurate to an ulp or two instead.
    constexpr double scale10(double mantissa, int exponent) noexcept
    {
        while (exponent > 22)
        {
            mantissa *= 1e22;
            exponent -= 22;
        }
        while (exponent < -22)
        {
            mantissa /= 1e22;
            exponent += 22;
        }
        return exponent >= 0 ? mantissa * exact_power_of_ten[exponent] : mantissa / exact_power_of_ten[-exponent];
    }

    // floor(log10(value)) for a finite value > 0: the e with
    // 10^e <= value < 10^(e + 1).
    constexpr int decade(double value) noexcept
    {
        int e = 0;
        while (value >= scale10(1.0, e + 1))
            ++e;
        while (value < scale10(1.0, e))
            --e;
        return e;
    }

} // namespace volt::math::electrical::detail
