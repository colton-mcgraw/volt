#pragma once

#include <array>
#include <cstddef>

namespace volt::platform {

struct KeyboardState {
  static constexpr std::size_t kMaxKeys = 512;

  std::array<bool, kMaxKeys> down{};
};

struct MouseState {
  static constexpr std::size_t kButtonCount = 8;

  std::array<bool, kButtonCount> down{};
  double x{0.0};
  double y{0.0};
  double deltaX{0.0};
  double deltaY{0.0};
  double scrollX{0.0};
  double scrollY{0.0};
};

struct InputState {
  KeyboardState keyboard;
  MouseState mouse;
};

}  // namespace volt::platform
