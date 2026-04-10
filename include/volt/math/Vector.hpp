#pragma once

#include "volt/math/MathCommon.hpp"

#include <cmath>

namespace volt::math {

template <typename T>
struct Vec2T {
  T x{0};
  T y{0};

  [[nodiscard]] constexpr Vec2T operator+(const Vec2T& rhs) const {
    return {x + rhs.x, y + rhs.y};
  }

  [[nodiscard]] constexpr Vec2T operator-(const Vec2T& rhs) const {
    return {x - rhs.x, y - rhs.y};
  }

  [[nodiscard]] constexpr Vec2T operator*(T scalar) const {
    return {x * scalar, y * scalar};
  }

  [[nodiscard]] constexpr Vec2T operator/(T scalar) const {
    return {x / scalar, y / scalar};
  }

  constexpr Vec2T& operator+=(const Vec2T& rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
  }

  constexpr Vec2T& operator-=(const Vec2T& rhs) {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
  }

  [[nodiscard]] constexpr T lengthSquared() const {
    return (x * x) + (y * y);
  }

  [[nodiscard]] T length() const {
    return static_cast<T>(std::sqrt(lengthSquared()));
  }

  [[nodiscard]] Vec2T normalized() const {
    const T len = length();
    if (nearlyEqual(len, static_cast<T>(0))) {
      return {};
    }
    return *this / len;
  }
};

template <typename T>
struct Vec3T {
  T x{0};
  T y{0};
  T z{0};

  [[nodiscard]] constexpr Vec3T operator+(const Vec3T& rhs) const {
    return {x + rhs.x, y + rhs.y, z + rhs.z};
  }

  [[nodiscard]] constexpr Vec3T operator-(const Vec3T& rhs) const {
    return {x - rhs.x, y - rhs.y, z - rhs.z};
  }

  [[nodiscard]] constexpr Vec3T operator*(T scalar) const {
    return {x * scalar, y * scalar, z * scalar};
  }

  [[nodiscard]] constexpr Vec3T operator/(T scalar) const {
    return {x / scalar, y / scalar, z / scalar};
  }

  constexpr Vec3T& operator+=(const Vec3T& rhs) {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    return *this;
  }

  constexpr Vec3T& operator-=(const Vec3T& rhs) {
    x -= rhs.x;
    y -= rhs.y;
    z -= rhs.z;
    return *this;
  }

  [[nodiscard]] constexpr T lengthSquared() const {
    return (x * x) + (y * y) + (z * z);
  }

  [[nodiscard]] T length() const {
    return static_cast<T>(std::sqrt(lengthSquared()));
  }

  [[nodiscard]] Vec3T normalized() const {
    const T len = length();
    if (nearlyEqual(len, static_cast<T>(0))) {
      return {};
    }
    return *this / len;
  }
};

template <typename T>
struct Vec4T {
  T x{0};
  T y{0};
  T z{0};
  T w{0};

  [[nodiscard]] constexpr Vec4T operator+(const Vec4T& rhs) const {
    return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w};
  }

  [[nodiscard]] constexpr Vec4T operator-(const Vec4T& rhs) const {
    return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w};
  }

  [[nodiscard]] constexpr Vec4T operator*(T scalar) const {
    return {x * scalar, y * scalar, z * scalar, w * scalar};
  }
};

template <typename T>
[[nodiscard]] constexpr T dot(const Vec2T<T>& lhs, const Vec2T<T>& rhs) {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y);
}

template <typename T>
[[nodiscard]] constexpr T dot(const Vec3T<T>& lhs, const Vec3T<T>& rhs) {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

template <typename T>
[[nodiscard]] constexpr T dot(const Vec4T<T>& lhs, const Vec4T<T>& rhs) {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z) + (lhs.w * rhs.w);
}

template <typename T>
[[nodiscard]] constexpr Vec3T<T> cross(const Vec3T<T>& lhs, const Vec3T<T>& rhs) {
  return {
      (lhs.y * rhs.z) - (lhs.z * rhs.y),
      (lhs.z * rhs.x) - (lhs.x * rhs.z),
      (lhs.x * rhs.y) - (lhs.y * rhs.x),
  };
}

using Vec2f = Vec2T<float>;
using Vec2d = Vec2T<double>;
using Vec3f = Vec3T<float>;
using Vec3d = Vec3T<double>;
using Vec4f = Vec4T<float>;
using Vec4d = Vec4T<double>;

}  // namespace volt::math
