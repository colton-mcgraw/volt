#pragma once

#include "volt/math/Matrix.hpp"
#include "volt/math/Quaternion.hpp"
#include "volt/math/Transform.hpp"

namespace volt::math {

template <typename T>
[[nodiscard]] bool validateMat4Inverse(T epsilon = static_cast<T>(kEpsilon<T>)) {
  const Mat4T<T> transform = translationMatrix(Vec3T<T>{static_cast<T>(2), static_cast<T>(-3), static_cast<T>(5)}) *
                             rotationMatrix(QuatT<T>::fromAxisAngle(Vec3T<T>{static_cast<T>(0), static_cast<T>(1), static_cast<T>(0)},
                                                                   radians(static_cast<T>(35)))) *
                             scaleMatrix(Vec3T<T>{static_cast<T>(1.5), static_cast<T>(0.75), static_cast<T>(2.25)});

  const Mat4T<T> inv = inverse(transform);
  const Mat4T<T> identityCandidate = transform * inv;
  return nearlyEqual(identityCandidate, Mat4T<T>::identity(), epsilon);
}

template <typename T>
[[nodiscard]] bool validateQuaternionMatrixRoundTrip(T epsilon = static_cast<T>(1e-4)) {
  const QuatT<T> original = QuatT<T>::fromAxisAngle(
      Vec3T<T>{static_cast<T>(1), static_cast<T>(2), static_cast<T>(3)},
      radians(static_cast<T>(42))).normalized();

  const Mat3T<T> matrix = original.toMat3();
  const QuatT<T> recovered = QuatT<T>::fromMat3(matrix).normalized();

  // Quaternion sign can flip while representing the same rotation.
  const T dotValue = (original.x * recovered.x) +
                     (original.y * recovered.y) +
                     (original.z * recovered.z) +
                     (original.w * recovered.w);
  const T absDot = dotValue < static_cast<T>(0) ? -dotValue : dotValue;
  return nearlyEqual(absDot, static_cast<T>(1), epsilon);
}

}  // namespace volt::math
