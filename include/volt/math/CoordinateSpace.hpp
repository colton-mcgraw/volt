#pragma once

#include "volt/math/Matrix.hpp"
#include "volt/math/Vector.hpp"

namespace volt::math {

template <typename T>
[[nodiscard]] Vec3T<T> transformPoint(const Mat4T<T>& matrix, const Vec3T<T>& point) {
  const Vec4T<T> p{point.x, point.y, point.z, static_cast<T>(1)};
  const Vec4T<T> r = matrix * p;
  if (nearlyEqual(r.w, static_cast<T>(0))) {
    return {r.x, r.y, r.z};
  }
  return {r.x / r.w, r.y / r.w, r.z / r.w};
}

template <typename T>
[[nodiscard]] Vec3T<T> transformDirection(const Mat4T<T>& matrix, const Vec3T<T>& direction) {
  const Vec4T<T> d{direction.x, direction.y, direction.z, static_cast<T>(0)};
  const Vec4T<T> r = matrix * d;
  return {r.x, r.y, r.z};
}

template <typename T>
[[nodiscard]] Vec3T<T> worldToNdc(const Mat4T<T>& viewProjection, const Vec3T<T>& worldPosition) {
  return transformPoint(viewProjection, worldPosition);
}

template <typename T>
[[nodiscard]] Vec2T<T> screenToNdc(
    const Vec2T<T>& screen,
    T viewportWidth,
    T viewportHeight) {
  const T x = (static_cast<T>(2) * screen.x / viewportWidth) - static_cast<T>(1);
  const T y = static_cast<T>(1) - (static_cast<T>(2) * screen.y / viewportHeight);
  return {x, y};
}

}  // namespace volt::math
