#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

namespace volt::math {

template <typename T>
inline constexpr T kPi = static_cast<T>(3.1415926535897932384626433832795);

template <typename T>
inline constexpr T kEpsilon = static_cast<T>(1e-6);

template <typename T>
[[nodiscard]] constexpr T radians(T degrees) {
  return degrees * (kPi<T> / static_cast<T>(180));
}

template <typename T>
[[nodiscard]] constexpr T degrees(T radiansValue) {
  return radiansValue * (static_cast<T>(180) / kPi<T>);
}

template <typename T>
[[nodiscard]] constexpr T clamp(T value, T minValue, T maxValue) {
  return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}

template <typename T>
[[nodiscard]] constexpr bool nearlyEqual(
    T lhs,
    T rhs,
    T epsilon = static_cast<T>(kEpsilon<T>)) {
  static_assert(std::is_floating_point_v<T>, "nearlyEqual expects floating point types");
  const T diff = lhs > rhs ? (lhs - rhs) : (rhs - lhs);
  return diff <= epsilon;
}

}  // namespace volt::math
