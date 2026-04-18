#pragma once

#include "volt/platform/WindowBackend.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace volt::platform {

[[nodiscard]] std::unique_ptr<WindowBackend> createWindowBackend(
    std::uint32_t width,
    std::uint32_t height,
    const std::string& title,
    const WindowCreateOptions& options);

}  // namespace volt::platform
