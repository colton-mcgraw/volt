#pragma once

#include "volt/math/MathCommon.hpp"
#include "volt/math/Matrix.hpp"
#include "volt/math/Vector.hpp"

#include <cmath>

namespace volt::math {

template <typename T>
struct QuatT {
  T x{0};
  T y{0};
  T z{0};
  T w{1};

  [[nodiscard]] static constexpr QuatT identity() {
    return {};
  }

  [[nodiscard]] constexpr QuatT conjugate() const {
    return {-x, -y, -z, w};
  }

  [[nodiscard]] QuatT inverse() const {
    const T lenSq = lengthSquared();
    if (nearlyEqual(lenSq, static_cast<T>(0))) {
      return identity();
    }
    const QuatT c = conjugate();
    return {c.x / lenSq, c.y / lenSq, c.z / lenSq, c.w / lenSq};
  }

  [[nodiscard]] constexpr T lengthSquared() const {
    return (x * x) + (y * y) + (z * z) + (w * w);
  }

  [[nodiscard]] T length() const {
    return static_cast<T>(std::sqrt(lengthSquared()));
  }

  [[nodiscard]] QuatT normalized() const {
    const T len = length();
    if (nearlyEqual(len, static_cast<T>(0))) {
      return identity();
    }
    return {x / len, y / len, z / len, w / len};
  }

  [[nodiscard]] constexpr QuatT operator*(const QuatT& rhs) const {
    return {
        (w * rhs.x) + (x * rhs.w) + (y * rhs.z) - (z * rhs.y),
        (w * rhs.y) - (x * rhs.z) + (y * rhs.w) + (z * rhs.x),
        (w * rhs.z) + (x * rhs.y) - (y * rhs.x) + (z * rhs.w),
        (w * rhs.w) - (x * rhs.x) - (y * rhs.y) - (z * rhs.z),
    };
  }

  [[nodiscard]] static QuatT fromAxisAngle(const Vec3T<T>& axis, T angleRadians) {
    const Vec3T<T> unitAxis = axis.normalized();
    const T halfAngle = angleRadians / static_cast<T>(2);
    const T s = static_cast<T>(std::sin(halfAngle));
    const T c = static_cast<T>(std::cos(halfAngle));
    return {unitAxis.x * s, unitAxis.y * s, unitAxis.z * s, c};
  }

  [[nodiscard]] Mat3T<T> toMat3() const {
    const QuatT q = normalized();
    const T xx = q.x * q.x;
    const T yy = q.y * q.y;
    const T zz = q.z * q.z;
    const T xy = q.x * q.y;
    const T xz = q.x * q.z;
    const T yz = q.y * q.z;
    const T wx = q.w * q.x;
    const T wy = q.w * q.y;
    const T wz = q.w * q.z;

    Mat3T<T> result{};
    result.set(0, 0, static_cast<T>(1) - static_cast<T>(2) * (yy + zz));
    result.set(0, 1, static_cast<T>(2) * (xy - wz));
    result.set(0, 2, static_cast<T>(2) * (xz + wy));

    result.set(1, 0, static_cast<T>(2) * (xy + wz));
    result.set(1, 1, static_cast<T>(1) - static_cast<T>(2) * (xx + zz));
    result.set(1, 2, static_cast<T>(2) * (yz - wx));

    result.set(2, 0, static_cast<T>(2) * (xz - wy));
    result.set(2, 1, static_cast<T>(2) * (yz + wx));
    result.set(2, 2, static_cast<T>(1) - static_cast<T>(2) * (xx + yy));
    return result;
  }

  [[nodiscard]] Vec3T<T> rotateVector(const Vec3T<T>& vector) const {
    const QuatT vecQuat{vector.x, vector.y, vector.z, static_cast<T>(0)};
    const QuatT rotated = (*this) * vecQuat * inverse();
    return {rotated.x, rotated.y, rotated.z};
  }

  [[nodiscard]] static QuatT fromMat3(const Mat3T<T>& matrix) {
    const T trace = matrix.at(0, 0) + matrix.at(1, 1) + matrix.at(2, 2);
    QuatT result{};

    if (trace > static_cast<T>(0)) {
      const T s = static_cast<T>(0.5) / static_cast<T>(std::sqrt(trace + static_cast<T>(1)));
      result.w = static_cast<T>(0.25) / s;
      result.x = (matrix.at(2, 1) - matrix.at(1, 2)) * s;
      result.y = (matrix.at(0, 2) - matrix.at(2, 0)) * s;
      result.z = (matrix.at(1, 0) - matrix.at(0, 1)) * s;
    } else if (matrix.at(0, 0) > matrix.at(1, 1) && matrix.at(0, 0) > matrix.at(2, 2)) {
      const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(
          static_cast<T>(1) + matrix.at(0, 0) - matrix.at(1, 1) - matrix.at(2, 2)));
      result.w = (matrix.at(2, 1) - matrix.at(1, 2)) / s;
      result.x = static_cast<T>(0.25) * s;
      result.y = (matrix.at(0, 1) + matrix.at(1, 0)) / s;
      result.z = (matrix.at(0, 2) + matrix.at(2, 0)) / s;
    } else if (matrix.at(1, 1) > matrix.at(2, 2)) {
      const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(
          static_cast<T>(1) + matrix.at(1, 1) - matrix.at(0, 0) - matrix.at(2, 2)));
      result.w = (matrix.at(0, 2) - matrix.at(2, 0)) / s;
      result.x = (matrix.at(0, 1) + matrix.at(1, 0)) / s;
      result.y = static_cast<T>(0.25) * s;
      result.z = (matrix.at(1, 2) + matrix.at(2, 1)) / s;
    } else {
      const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(
          static_cast<T>(1) + matrix.at(2, 2) - matrix.at(0, 0) - matrix.at(1, 1)));
      result.w = (matrix.at(1, 0) - matrix.at(0, 1)) / s;
      result.x = (matrix.at(0, 2) + matrix.at(2, 0)) / s;
      result.y = (matrix.at(1, 2) + matrix.at(2, 1)) / s;
      result.z = static_cast<T>(0.25) * s;
    }

    return result.normalized();
  }
};

using Quatf = QuatT<float>;
using Quatd = QuatT<double>;

}  // namespace volt::math
