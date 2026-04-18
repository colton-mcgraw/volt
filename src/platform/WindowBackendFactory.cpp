#include "volt/platform/WindowBackendFactory.hpp"

#include "volt/platform/details/LinuxWindowBackend.hpp"
#include "volt/platform/details/MacOSWindowBackend.hpp"
#include "volt/platform/details/Win32WindowBackend.hpp"

#include <stdexcept>

namespace volt::platform {

std::unique_ptr<WindowBackend> createWindowBackend(
    std::uint32_t width,
    std::uint32_t height,
    const std::string& title,
    const WindowCreateOptions& options) {
#if defined(_WIN32)
  return std::make_unique<volt::platform::details::Win32WindowBackend>(width, height, title, options);
#elif defined(__APPLE__)
  return std::make_unique<volt::platform::details::MacOSWindowBackend>(width, height, title, options);
#elif defined(__linux__)
  return std::make_unique<volt::platform::details::LinuxWindowBackend>(width, height, title, options);
#else
  (void)width;
  (void)height;
  (void)title;
  (void)options;
  throw std::runtime_error("No window backend available for this platform");
#endif
}

}  // namespace volt::platform
