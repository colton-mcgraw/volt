#pragma once

#include <math/electrical/constants.hpp>
#include <math/electrical/detail/decimal.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace volt::math::electrical
{

    // ---------------------------------------------------------------------
    // Preferred values (IEC 60063 E series)
    // ---------------------------------------------------------------------
    //
    // The values resistors, capacitors and inductors are actually made in:
    // each series divides a decade into that many roughly geometric steps,
    // which then repeat in every decade. E3 to E24 are the historical
    // two-digit values; E48 to E192 are 10^(i / N) rounded to three digits.
    //
    //   preferred_nearest(4500.0, e_series::e12)   // 4700
    //   preferred_floor(4500.0, e_series::e96)     // 4420
    //
    // The functions take any finite value > 0, in any decade, and return a
    // value that compares equal to the same number written as a literal
    // (4700.0, 4.7e-6). Anything else (zero, negative, NaN, infinity) is
    // returned unchanged. They are constexpr.

    enum class e_series : std::uint8_t
    {
        e3 = 3,
        e6 = 6,
        e12 = 12,
        e24 = 24,
        e48 = 48,
        e96 = 96,
        e192 = 192,
    };

    namespace detail
    {
        // The two tables every series is a stride through, as three-digit
        // mantissas (100 means 1.00). E192 is the rounded formula with its
        // one standardised exception: 9.20 where the formula gives 9.19.
        inline constexpr std::uint16_t e24_mantissas[24] = {100, 110, 120, 130, 150, 160, 180, 200,
                                                            220, 240, 270, 300, 330, 360, 390, 430,
                                                            470, 510, 560, 620, 680, 750, 820, 910};

        inline constexpr std::uint16_t e192_mantissas[192] = {
            100, 101, 102, 104, 105, 106, 107, 109, 110, 111, 113, 114, 115, 117, 118, 120,
            121, 123, 124, 126, 127, 129, 130, 132, 133, 135, 137, 138, 140, 142, 143, 145,
            147, 149, 150, 152, 154, 156, 158, 160, 162, 164, 165, 167, 169, 172, 174, 176,
            178, 180, 182, 184, 187, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213,
            215, 218, 221, 223, 226, 229, 232, 234, 237, 240, 243, 246, 249, 252, 255, 258,
            261, 264, 267, 271, 274, 277, 280, 284, 287, 291, 294, 298, 301, 305, 309, 312,
            316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 357, 361, 365, 370, 374, 379,
            383, 388, 392, 397, 402, 407, 412, 417, 422, 427, 432, 437, 442, 448, 453, 459,
            464, 470, 475, 481, 487, 493, 499, 505, 511, 517, 523, 530, 536, 542, 549, 556,
            562, 569, 576, 583, 590, 597, 604, 612, 619, 626, 634, 642, 649, 657, 665, 673,
            681, 690, 698, 706, 715, 723, 732, 741, 750, 759, 768, 777, 787, 796, 806, 816,
            825, 835, 845, 856, 866, 876, 887, 898, 909, 920, 931, 942, 953, 965, 976, 988};
    } // namespace detail

    // Number of values per decade.
    constexpr std::size_t e_series_size(e_series series) noexcept
    {
        return static_cast<std::size_t>(series);
    }

    // The index-th value of the series' first decade, as a three-digit
    // mantissa in [100, 1000): 470 for 4.7. index < e_series_size(series).
    constexpr std::uint16_t e_series_mantissa(e_series series, std::size_t index) noexcept
    {
        const std::size_t count = e_series_size(series);
        return count <= 24 ? detail::e24_mantissas[index * (24 / count)] : detail::e192_mantissas[index * (192 / count)];
    }

    // The tolerance parts of this series are conventionally made to, as a
    // fraction: 0.01 for E96. The step between neighbours is about twice
    // this, so the tolerance bands of the series just tile a decade.
    constexpr double e_series_tolerance(e_series series) noexcept
    {
        switch (series)
        {
        case e_series::e3:
            return 0.40;
        case e_series::e6:
            return 0.20;
        case e_series::e12:
            return 0.10;
        case e_series::e24:
            return 0.05;
        case e_series::e48:
            return 0.02;
        case e_series::e96:
            return 0.01;
        case e_series::e192:
            return 0.005;
        }
        return 0.0;
    }

    namespace detail
    {
        // Preferred values are numbered across decades: index k is entry
        // k mod N of decade k div N, with index 0 at 1.0.
        constexpr double e_series_value(e_series series, long long index) noexcept
        {
            const long long count = static_cast<long long>(e_series_size(series));
            long long decade_index = index / count;
            long long entry = index % count;
            if (entry < 0)
            {
                entry += count;
                --decade_index;
            }
            return scale10(e_series_mantissa(series, static_cast<std::size_t>(entry)), static_cast<int>(decade_index) - 2);
        }

        // Index of the largest preferred value <= limit, for finite limit > 0.
        constexpr long long e_series_floor_index(e_series series, double limit) noexcept
        {
            const long long count = static_cast<long long>(e_series_size(series));
            const long long base = decade(limit) * count;
            // value(base + low) <= limit < value(base + high) throughout;
            // entry 0 is the decade itself, entry `count` the next one.
            long long low = 0;
            long long high = count;
            while (high - low > 1)
            {
                const long long middle = low + (high - low) / 2;
                if (e_series_value(series, base + middle) <= limit)
                    low = middle;
                else
                    high = middle;
            }
            return base + low;
        }

        // A value this close to a preferred one, relatively, is taken to be
        // it: a few ulps of F, which absorbs the rounding of a value that
        // was computed, or narrowed to float, rather than written out.
        template <floating_numeric F>
        inline constexpr double e_series_slack = 8.0 * static_cast<double>(std::numeric_limits<F>::epsilon());

        constexpr bool e_series_domain(double value) noexcept
        {
            return value > 0.0 && value <= std::numeric_limits<double>::max();
        }
    } // namespace detail

    // The largest preferred value <= value.
    template <floating_numeric F>
    constexpr F preferred_floor(F value, e_series series) noexcept
    {
        const double v = static_cast<double>(value);
        if (!detail::e_series_domain(v))
            return value;
        const double limit = v * (1.0 + detail::e_series_slack<F>);
        return static_cast<F>(detail::e_series_value(series, detail::e_series_floor_index(series, limit)));
    }

    // The smallest preferred value >= value.
    template <floating_numeric F>
    constexpr F preferred_ceil(F value, e_series series) noexcept
    {
        const double v = static_cast<double>(value);
        if (!detail::e_series_domain(v))
            return value;
        const long long index = detail::e_series_floor_index(series, v * (1.0 + detail::e_series_slack<F>));
        const double below = detail::e_series_value(series, index);
        if (below >= v * (1.0 - detail::e_series_slack<F>))
            return static_cast<F>(below);
        return static_cast<F>(detail::e_series_value(series, index + 1));
    }

    // The preferred value closest to value; the lower one on a tie.
    template <floating_numeric F>
    constexpr F preferred_nearest(F value, e_series series) noexcept
    {
        const double v = static_cast<double>(value);
        if (!detail::e_series_domain(v))
            return value;
        const long long index = detail::e_series_floor_index(series, v * (1.0 + detail::e_series_slack<F>));
        const double below = detail::e_series_value(series, index);
        const double above = detail::e_series_value(series, index + 1);
        return static_cast<F>(v - below <= above - v ? below : above);
    }

    // Whether value is a member of the series, in any decade.
    template <floating_numeric F>
    constexpr bool is_preferred(F value, e_series series) noexcept
    {
        const double v = static_cast<double>(value);
        if (!detail::e_series_domain(v))
            return false;
        const double below =
            detail::e_series_value(series, detail::e_series_floor_index(series, v * (1.0 + detail::e_series_slack<F>)));
        return below >= v * (1.0 - detail::e_series_slack<F>);
    }

} // namespace volt::math::electrical
