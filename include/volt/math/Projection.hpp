#pragma once

#include "volt/math/MathCommon.hpp"
#include "volt/math/Matrix.hpp"
#include "volt/math/Vector.hpp"

#include <cmath>

namespace volt::math {

enum class ClipSpaceZ {
  kMinusOneToOne,
  kZeroToOne,
};

template <typename T>
[[nodiscard]] Mat4T<T> perspectiveRH(
    T verticalFovRadians,
    T aspectRatio,
    T nearPlane,
    T farPlane,
    ClipSpaceZ clipSpace = ClipSpaceZ::kZeroToOne) {
  const T tanHalfFovy = static_cast<T>(std::tan(verticalFovRadians / static_cast<T>(2)));

  Mat4T<T> result{};
  result.set(0, 0, static_cast<T>(1) / (aspectRatio * tanHalfFovy));
  result.set(1, 1, static_cast<T>(1) / tanHalfFovy);
  result.set(3, 2, static_cast<T>(-1));
  result.set(3, 3, static_cast<T>(0));

  if (clipSpace == ClipSpaceZ::kZeroToOne) {
    result.set(2, 2, farPlane / (nearPlane - farPlane));
    result.set(2, 3, (farPlane * nearPlane) / (nearPlane - farPlane));
  } else {
    result.set(2, 2, (farPlane + nearPlane) / (nearPlane - farPlane));
    result.set(2, 3, (static_cast<T>(2) * farPlane * nearPlane) / (nearPlane - farPlane));
  }

  return result;
}

template <typename T>
[[nodiscard]] Mat4T<T> orthographicRH(
    T left,
    T right,
    T bottom,
    T top,
    T nearPlane,
    T farPlane,
    ClipSpaceZ clipSpace = ClipSpaceZ::kZeroToOne) {
  Mat4T<T> result = Mat4T<T>::identity();
  result.set(0, 0, static_cast<T>(2) / (right - left));
  result.set(1, 1, static_cast<T>(2) / (top - bottom));
  result.set(0, 3, -(right + left) / (right - left));
  result.set(1, 3, -(top + bottom) / (top - bottom));

  if (clipSpace == ClipSpaceZ::kZeroToOne) {
    result.set(2, 2, static_cast<T>(1) / (nearPlane - farPlane));
    result.set(2, 3, nearPlane / (nearPlane - farPlane));
  } else {
    result.set(2, 2, static_cast<T>(2) / (nearPlane - farPlane));
    result.set(2, 3, (farPlane + nearPlane) / (nearPlane - farPlane));
  }

  return result;
}

template <typename T>
[[nodiscard]] Mat4T<T> lookAtRH(
    const Vec3T<T>& eye,
    const Vec3T<T>& target,
    const Vec3T<T>& up) {
  const Vec3T<T> forward = (eye - target).normalized();
  const Vec3T<T> right = cross(up.normalized(), forward).normalized();
  const Vec3T<T> cameraUp = cross(forward, right);

  Mat4T<T> result = Mat4T<T>::identity();
  result.set(0, 0, right.x);
  result.set(1, 0, right.y);
  result.set(2, 0, right.z);

  result.set(0, 1, cameraUp.x);
  result.set(1, 1, cameraUp.y);
  result.set(2, 1, cameraUp.z);

  result.set(0, 2, forward.x);
  result.set(1, 2, forward.y);
  result.set(2, 2, forward.z);

  result.set(0, 3, -dot(right, eye));
  result.set(1, 3, -dot(cameraUp, eye));
  result.set(2, 3, -dot(forward, eye));
  return result;
}

}  // namespace volt::math
