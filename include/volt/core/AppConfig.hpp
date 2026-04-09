#pragma once

#include <cstdint>
#include <string>

namespace volt::core {

struct AppConfig {
  std::string appName{"Volt"};
  std::uint32_t windowWidth{1600};
  std::uint32_t windowHeight{900};
};

}  // namespace volt::core
