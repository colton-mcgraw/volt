#pragma once

#include <cstdint>
#include <string>

namespace volt::core {

struct AppConfig {
  AppConfig();

  std::string appName{"Volt"};
  std::string windowTitle{"Volt"};
  std::uint32_t windowWidth{1600};
  std::uint32_t windowHeight{900};
  std::int32_t windowX{-1};
  std::int32_t windowY{-1};
  bool windowResizable{true};
  std::string windowIconPath{};
  std::int32_t windowResizeBorderThickness{0};
  std::int32_t windowTitleBarHeight{38};
  std::int32_t windowTitleBarPadding{12};
  std::int32_t windowTitleBarButtonSize{20};
  std::int32_t windowTitleBarButtonSpacing{0};
  bool useSystemTitleBar{false};
  bool useSystemResizeHandles{false};
};

}  // namespace volt::core
