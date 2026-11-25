/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief `volt::math::detail::forward_like`, a local stand-in for `std::forward_like` (P2445).
 * @details It computes the same type and performs the same cast the standard specifies, and exists
 * only because `std::forward_like` is unusable on Clang: every Clang from 18 through 20 rejects
 * libstdc++'s implementation with "function with deduced return type cannot be used before it is
 * defined", and libc++ is not an escape because it is missing other things Volt needs. Since
 * the accessors in vector.hpp and matrix.hpp are the only callers, replacing the one function is
 * cheaper than giving up Clang -- and macOS with it.
 *
 * The difference that makes this one work is the trailing return type: the type is named rather
 * than deduced, so there is no deduced return type for a caller to reach before it is complete.
 */

#pragma once

#include <type_traits>

namespace volt::math::detail
{

    /** @brief `const B` when `A` is const, plain `B` otherwise. The standard's `COPY_CONST`. */
    template <typename A, typename B>
    using copy_const_t = std::conditional_t<std::is_const_v<A>, const B, B>;

    /** @brief `B&&` when `A` is an rvalue reference, `B&` otherwise. The standard's `OVERRIDE_REF`. */
    template <typename A, typename B>
    using override_ref_t = std::conditional_t<std::is_rvalue_reference_v<A>, B &&, B &>;

    /**
     * @brief The type `std::forward_like<T>(u)` would return.
     * @details Spelled exactly as [forward] specifies it:
     * `OVERRIDE_REF(T&&, COPY_CONST(remove_reference_t<T>, remove_reference_t<U>))`. Reference
     * collapsing in `T&&` is what distinguishes the two cases: an lvalue-reference `T` collapses to
     * an lvalue reference and yields `B&`, anything else yields `B&&`.
     */
    template <typename T, typename U>
    using forward_like_t = override_ref_t<T &&, copy_const_t<std::remove_reference_t<T>, std::remove_reference_t<U>>>;

    /**
     * @brief Casts @p value to the value category and constness of `T`.
     * @details The merge is the point: the result is const if *either* side is, so a const vector
     * never hands out a mutable reference to an element, and a temporary vector hands out an
     * rvalue so the element can be moved from.
     */
    template <typename T, typename U>
    [[nodiscard]] constexpr auto forward_like(U &&value) noexcept -> forward_like_t<T, U>
    {
        return static_cast<forward_like_t<T, U>>(value);
    }

} // namespace volt::math::detail
