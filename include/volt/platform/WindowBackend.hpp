#pragma once

#include "volt/platform/InputState.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace volt::event {
class EventDispatcher;
}

namespace volt::platform {

struct DisplayMetrics {
  float dpiX{96.0F};
  float dpiY{96.0F};
  float ppiX{96.0F};
  float ppiY{96.0F};
  float contentScaleX{1.0F};
  float contentScaleY{1.0F};
};

enum class WindowBackendType {
  kWin32,
  kLinux,
  kMacOS,
  kUnknown,
};

struct WindowCreateOptions {
  bool useSystemTitleBar{false};
  bool useSystemResizeHandles{false};
  bool windowResizable{true};
  std::int32_t initialX{-1};
  std::int32_t initialY{-1};
  std::int32_t resizeBorderThickness{0};
  std::int32_t titleBarHeight{0};
  std::int32_t titleBarPadding{0};
  std::int32_t titleBarButtonSize{0};
  std::int32_t titleBarButtonSpacing{0};
  std::string windowIconPath{};
};

enum class WindowResizeEdge {
  kLeft,
  kTop,
  kRight,
  kBottom,
  kTopLeft,
  kTopRight,
  kBottomLeft,
  kBottomRight,
};

class WindowBackend {
 public:
  virtual ~WindowBackend() = default;

  [[nodiscard]] virtual bool shouldClose() const = 0;
  virtual void requestClose() = 0;
  virtual void setEventDispatcher(volt::event::EventDispatcher* dispatcher) = 0;
  virtual void setResizeRepaintCallback(std::function<void()> callback) = 0;

  virtual void pollEvents() = 0;
  virtual void waitEvents() = 0;
  virtual void waitEventsTimeout(double timeoutSeconds) = 0;

  [[nodiscard]] virtual bool wasResized() const = 0;
  virtual void acknowledgeResize() = 0;

  [[nodiscard]] virtual bool isMinimized() const = 0;
  [[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> framebufferExtent() const = 0;
  [[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> logicalExtent() const = 0;
  [[nodiscard]] virtual DisplayMetrics displayMetrics() const = 0;
  [[nodiscard]] virtual const InputState& inputSnapshot() const = 0;

  [[nodiscard]] virtual WindowBackendType backendType() const = 0;
  [[nodiscard]] virtual void* nativeWindowHandle() const = 0;
  [[nodiscard]] virtual void* nativeDisplayHandle() const = 0;

  virtual void beginInteractiveMove() = 0;
  virtual void beginInteractiveResize(WindowResizeEdge edge) = 0;
  [[nodiscard]] virtual bool isMaximized() const = 0;
  virtual void toggleMaximized() = 0;
  virtual void minimize() = 0;
};

}  // namespace volt::platform
