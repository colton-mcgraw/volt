#pragma once

#include <math/electrical/constants.hpp>

#include <type_traits>

namespace volt::math::electrical
{

    // ---------------------------------------------------------------------
    // Combining elements
    // ---------------------------------------------------------------------
    //
    // series() and parallel() are written for impedances, real or complex,
    // and take any number of them. They serve every element once it is
    // clear which rule its value follows:
    //
    //   resistors, inductors, impedances   series() in series, parallel() in parallel
    //   capacitors, conductances           the other way round
    //
    // so two capacitors in series are parallel(c1, c2).

    template <quantity T, quantity... Ts>
    constexpr auto series(T first, Ts... rest) noexcept
    {
        using R = std::common_type_t<T, Ts...>;
        return (detail::as<R>(first) + ... + detail::as<R>(rest));
    }

    // 1 / (1/a + 1/b + ...). A zero branch shorts the combination, so the
    // result is zero rather than the NaN the formula would produce for
    // complex values. Leave an open branch out instead of passing infinity.
    template <quantity T, quantity... Ts>
    constexpr auto parallel(T first, Ts... rest) noexcept
    {
        using R = std::common_type_t<T, Ts...>;
        if (first == T{} || (... || (rest == Ts{})))
            return R{};
        return R(1) / ((R(1) / detail::as<R>(first)) + ... + (R(1) / detail::as<R>(rest)));
    }

    // ---------------------------------------------------------------------
    // Dividers
    // ---------------------------------------------------------------------

    // Voltage across `bottom` when `input` is applied across top + bottom
    // in series, unloaded.
    template <quantity V, quantity Zt, quantity Zb>
    constexpr auto voltage_divider(V input, Zt top, Zb bottom) noexcept
    {
        using R = std::common_type_t<V, Zt, Zb>;
        return detail::as<R>(input) * detail::as<R>(bottom) / (detail::as<R>(top) + detail::as<R>(bottom));
    }

    // Current through `branch` when `input` splits between `branch` and
    // `other` in parallel.
    template <quantity I, quantity Zb, quantity Zo>
    constexpr auto current_divider(I input, Zb branch, Zo other) noexcept
    {
        using R = std::common_type_t<I, Zb, Zo>;
        return detail::as<R>(input) * detail::as<R>(other) / (detail::as<R>(branch) + detail::as<R>(other));
    }

    // ---------------------------------------------------------------------
    // Wye and delta
    // ---------------------------------------------------------------------
    //
    // The two three-terminal networks between nodes a, b and c: a delta has
    // one element between each pair of nodes, a wye one element from each
    // node to a common centre. to_wye() and to_delta() convert between the
    // equivalent pairs.

    template <quantity T>
    struct delta
    {
        T ab{};
        T bc{};
        T ca{};

        constexpr bool operator==(const delta &) const noexcept = default;
    };

    template <quantity T>
    struct wye
    {
        T a{};
        T b{};
        T c{};

        constexpr bool operator==(const wye &) const noexcept = default;
    };

    template <quantity T>
    delta(T, T, T) -> delta<T>;

    template <quantity T>
    wye(T, T, T) -> wye<T>;

    // Each wye leg is the product of the two delta elements meeting at its
    // node, over the sum of all three.
    template <quantity T>
    constexpr wye<T> to_wye(const delta<T> &d) noexcept
    {
        const T total = d.ab + d.bc + d.ca;
        return {d.ab * d.ca / total, d.ab * d.bc / total, d.bc * d.ca / total};
    }

    // Each delta element is the sum of the pairwise products of the wye
    // legs, over the leg at the opposite node.
    template <quantity T>
    constexpr delta<T> to_delta(const wye<T> &y) noexcept
    {
        const T products = y.a * y.b + y.b * y.c + y.c * y.a;
        return {products / y.c, products / y.a, products / y.b};
    }

} // namespace volt::math::electrical
