#pragma once

#include "volt/math/MathCommon.hpp"
#include "volt/math/Vector.hpp"

#include <array>
#include <stdexcept>

namespace volt::math {

template <typename T>
struct Mat3T {
  std::array<T, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};

  [[nodiscard]] static constexpr Mat3T identity() {
    return {};
  }

  [[nodiscard]] constexpr T at(int row, int column) const {
    return m[static_cast<std::size_t>((column * 3) + row)];
  }

  constexpr void set(int row, int column, T value) {
    m[static_cast<std::size_t>((column * 3) + row)] = value;
  }
};

template <typename T>
struct Mat4T {
  std::array<T, 16> m{1, 0, 0, 0,
                      0, 1, 0, 0,
                      0, 0, 1, 0,
                      0, 0, 0, 1};

  [[nodiscard]] static constexpr Mat4T identity() {
    return {};
  }

  [[nodiscard]] constexpr T at(int row, int column) const {
    return m[static_cast<std::size_t>((column * 4) + row)];
  }

  constexpr void set(int row, int column, T value) {
    m[static_cast<std::size_t>((column * 4) + row)] = value;
  }

  [[nodiscard]] constexpr Mat4T transposed() const {
    Mat4T result{};
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        result.set(col, row, at(row, col));
      }
    }
    return result;
  }
};

template <typename T>
[[nodiscard]] constexpr Mat4T<T> operator*(const Mat4T<T>& lhs, const Mat4T<T>& rhs) {
  Mat4T<T> result{};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      T sum{0};
      for (int k = 0; k < 4; ++k) {
        sum += lhs.at(row, k) * rhs.at(k, col);
      }
      result.set(row, col, sum);
    }
  }
  return result;
}

template <typename T>
[[nodiscard]] constexpr Vec4T<T> operator*(const Mat4T<T>& lhs, const Vec4T<T>& rhs) {
  return {
      (lhs.at(0, 0) * rhs.x) + (lhs.at(0, 1) * rhs.y) + (lhs.at(0, 2) * rhs.z) + (lhs.at(0, 3) * rhs.w),
      (lhs.at(1, 0) * rhs.x) + (lhs.at(1, 1) * rhs.y) + (lhs.at(1, 2) * rhs.z) + (lhs.at(1, 3) * rhs.w),
      (lhs.at(2, 0) * rhs.x) + (lhs.at(2, 1) * rhs.y) + (lhs.at(2, 2) * rhs.z) + (lhs.at(2, 3) * rhs.w),
      (lhs.at(3, 0) * rhs.x) + (lhs.at(3, 1) * rhs.y) + (lhs.at(3, 2) * rhs.z) + (lhs.at(3, 3) * rhs.w),
  };
}

using Mat3f = Mat3T<float>;
using Mat3d = Mat3T<double>;
using Mat4f = Mat4T<float>;
using Mat4d = Mat4T<double>;

template <typename T>
[[nodiscard]] constexpr T determinant(const Mat3T<T>& matrix) {
  return (matrix.at(0, 0) * ((matrix.at(1, 1) * matrix.at(2, 2)) - (matrix.at(1, 2) * matrix.at(2, 1)))) -
         (matrix.at(0, 1) * ((matrix.at(1, 0) * matrix.at(2, 2)) - (matrix.at(1, 2) * matrix.at(2, 0)))) +
         (matrix.at(0, 2) * ((matrix.at(1, 0) * matrix.at(2, 1)) - (matrix.at(1, 1) * matrix.at(2, 0))));
}

template <typename T>
[[nodiscard]] T determinant(const Mat4T<T>& matrix) {
  const T a0 = matrix.at(0, 0) * matrix.at(1, 1) - matrix.at(1, 0) * matrix.at(0, 1);
  const T a1 = matrix.at(0, 0) * matrix.at(1, 2) - matrix.at(1, 0) * matrix.at(0, 2);
  const T a2 = matrix.at(0, 0) * matrix.at(1, 3) - matrix.at(1, 0) * matrix.at(0, 3);
  const T a3 = matrix.at(0, 1) * matrix.at(1, 2) - matrix.at(1, 1) * matrix.at(0, 2);
  const T a4 = matrix.at(0, 1) * matrix.at(1, 3) - matrix.at(1, 1) * matrix.at(0, 3);
  const T a5 = matrix.at(0, 2) * matrix.at(1, 3) - matrix.at(1, 2) * matrix.at(0, 3);

  const T b0 = matrix.at(2, 0) * matrix.at(3, 1) - matrix.at(3, 0) * matrix.at(2, 1);
  const T b1 = matrix.at(2, 0) * matrix.at(3, 2) - matrix.at(3, 0) * matrix.at(2, 2);
  const T b2 = matrix.at(2, 0) * matrix.at(3, 3) - matrix.at(3, 0) * matrix.at(2, 3);
  const T b3 = matrix.at(2, 1) * matrix.at(3, 2) - matrix.at(3, 1) * matrix.at(2, 2);
  const T b4 = matrix.at(2, 1) * matrix.at(3, 3) - matrix.at(3, 1) * matrix.at(2, 3);
  const T b5 = matrix.at(2, 2) * matrix.at(3, 3) - matrix.at(3, 2) * matrix.at(2, 3);

  return (a0 * b5) - (a1 * b4) + (a2 * b3) + (a3 * b2) - (a4 * b1) + (a5 * b0);
}

template <typename T>
[[nodiscard]] Mat4T<T> inverse(const Mat4T<T>& matrix) {
  Mat4T<T> result{};

  const T a0 = matrix.at(0, 0) * matrix.at(1, 1) - matrix.at(1, 0) * matrix.at(0, 1);
  const T a1 = matrix.at(0, 0) * matrix.at(1, 2) - matrix.at(1, 0) * matrix.at(0, 2);
  const T a2 = matrix.at(0, 0) * matrix.at(1, 3) - matrix.at(1, 0) * matrix.at(0, 3);
  const T a3 = matrix.at(0, 1) * matrix.at(1, 2) - matrix.at(1, 1) * matrix.at(0, 2);
  const T a4 = matrix.at(0, 1) * matrix.at(1, 3) - matrix.at(1, 1) * matrix.at(0, 3);
  const T a5 = matrix.at(0, 2) * matrix.at(1, 3) - matrix.at(1, 2) * matrix.at(0, 3);

  const T b0 = matrix.at(2, 0) * matrix.at(3, 1) - matrix.at(3, 0) * matrix.at(2, 1);
  const T b1 = matrix.at(2, 0) * matrix.at(3, 2) - matrix.at(3, 0) * matrix.at(2, 2);
  const T b2 = matrix.at(2, 0) * matrix.at(3, 3) - matrix.at(3, 0) * matrix.at(2, 3);
  const T b3 = matrix.at(2, 1) * matrix.at(3, 2) - matrix.at(3, 1) * matrix.at(2, 2);
  const T b4 = matrix.at(2, 1) * matrix.at(3, 3) - matrix.at(3, 1) * matrix.at(2, 3);
  const T b5 = matrix.at(2, 2) * matrix.at(3, 3) - matrix.at(3, 2) * matrix.at(2, 3);

  const T det = (a0 * b5) - (a1 * b4) + (a2 * b3) + (a3 * b2) - (a4 * b1) + (a5 * b0);
  if (nearlyEqual(det, static_cast<T>(0))) {
    throw std::runtime_error("Cannot invert singular 4x4 matrix");
  }

  result.set(0, 0, +(matrix.at(1, 1) * b5 - matrix.at(1, 2) * b4 + matrix.at(1, 3) * b3));
  result.set(1, 0, -(matrix.at(1, 0) * b5 - matrix.at(1, 2) * b2 + matrix.at(1, 3) * b1));
  result.set(2, 0, +(matrix.at(1, 0) * b4 - matrix.at(1, 1) * b2 + matrix.at(1, 3) * b0));
  result.set(3, 0, -(matrix.at(1, 0) * b3 - matrix.at(1, 1) * b1 + matrix.at(1, 2) * b0));

  result.set(0, 1, -(matrix.at(0, 1) * b5 - matrix.at(0, 2) * b4 + matrix.at(0, 3) * b3));
  result.set(1, 1, +(matrix.at(0, 0) * b5 - matrix.at(0, 2) * b2 + matrix.at(0, 3) * b1));
  result.set(2, 1, -(matrix.at(0, 0) * b4 - matrix.at(0, 1) * b2 + matrix.at(0, 3) * b0));
  result.set(3, 1, +(matrix.at(0, 0) * b3 - matrix.at(0, 1) * b1 + matrix.at(0, 2) * b0));

  result.set(0, 2, +(matrix.at(3, 1) * a5 - matrix.at(3, 2) * a4 + matrix.at(3, 3) * a3));
  result.set(1, 2, -(matrix.at(3, 0) * a5 - matrix.at(3, 2) * a2 + matrix.at(3, 3) * a1));
  result.set(2, 2, +(matrix.at(3, 0) * a4 - matrix.at(3, 1) * a2 + matrix.at(3, 3) * a0));
  result.set(3, 2, -(matrix.at(3, 0) * a3 - matrix.at(3, 1) * a1 + matrix.at(3, 2) * a0));

  result.set(0, 3, -(matrix.at(2, 1) * a5 - matrix.at(2, 2) * a4 + matrix.at(2, 3) * a3));
  result.set(1, 3, +(matrix.at(2, 0) * a5 - matrix.at(2, 2) * a2 + matrix.at(2, 3) * a1));
  result.set(2, 3, -(matrix.at(2, 0) * a4 - matrix.at(2, 1) * a2 + matrix.at(2, 3) * a0));
  result.set(3, 3, +(matrix.at(2, 0) * a3 - matrix.at(2, 1) * a1 + matrix.at(2, 2) * a0));

  const T invDet = static_cast<T>(1) / det;
  for (std::size_t i = 0; i < result.m.size(); ++i) {
    result.m[i] *= invDet;
  }
  return result;
}

template <typename T>
[[nodiscard]] bool nearlyEqual(
    const Mat4T<T>& lhs,
    const Mat4T<T>& rhs,
    T epsilon = static_cast<T>(kEpsilon<T>)) {
  for (std::size_t i = 0; i < lhs.m.size(); ++i) {
    if (!nearlyEqual(lhs.m[i], rhs.m[i], epsilon)) {
      return false;
    }
  }
  return true;
}

}  // namespace volt::math
