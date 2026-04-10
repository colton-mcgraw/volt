#pragma once

#include "volt/math/Matrix.hpp"
#include "volt/math/Quaternion.hpp"
#include "volt/math/Vector.hpp"

namespace volt::math {

template <typename T>
[[nodiscard]] constexpr Mat4T<T> translationMatrix(const Vec3T<T>& translation) {
  Mat4T<T> result = Mat4T<T>::identity();
  result.set(0, 3, translation.x);
  result.set(1, 3, translation.y);
  result.set(2, 3, translation.z);
  return result;
}

template <typename T>
[[nodiscard]] constexpr Mat4T<T> scaleMatrix(const Vec3T<T>& scale) {
  Mat4T<T> result = Mat4T<T>::identity();
  result.set(0, 0, scale.x);
  result.set(1, 1, scale.y);
  result.set(2, 2, scale.z);
  return result;
}

template <typename T>
[[nodiscard]] Mat4T<T> rotationMatrix(const QuatT<T>& rotation) {
  const Mat3T<T> r3 = rotation.toMat3();
  Mat4T<T> result = Mat4T<T>::identity();
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      result.set(row, col, r3.at(row, col));
    }
  }
  return result;
}

template <typename T>
struct TransformT {
  Vec3T<T> position{};
  QuatT<T> rotation{};
  Vec3T<T> scale{static_cast<T>(1), static_cast<T>(1), static_cast<T>(1)};

  [[nodiscard]] Mat4T<T> matrix() const {
    return translationMatrix(position) * rotationMatrix(rotation) * scaleMatrix(scale);
  }
};

using Transformf = TransformT<float>;
using Transformd = TransformT<double>;

}  // namespace volt::math
