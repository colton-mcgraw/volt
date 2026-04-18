#pragma once

#include "volt/platform/WindowBackend.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace volt::event {
class EventDispatcher;
}

namespace volt::platform {

struct WindowChromeRect {
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
};

struct WindowChromeLayout {
  WindowChromeRect titleBar{};
  WindowChromeRect titleIcon{};
  WindowChromeRect titleText{};
  WindowChromeRect dragRegion{};
  WindowChromeRect minimizeButton{};
  WindowChromeRect maximizeButton{};
  WindowChromeRect closeButton{};
  WindowChromeRect leftResize{};
  WindowChromeRect rightResize{};
  WindowChromeRect topResize{};
  WindowChromeRect bottomResize{};
  WindowChromeRect topLeftResize{};
  WindowChromeRect topRightResize{};
  WindowChromeRect bottomLeftResize{};
  WindowChromeRect bottomRightResize{};

  float titleBarPadding{0.0F};
  float titleBarButtonSpacing{0.0F};
  float titleTextBaselineY{0.0F};
  float buttonTextBaselineY{0.0F};
  bool hasResizeRegions{false};
};

struct WindowChromeWidgetIds {
  std::uint64_t dragRegion{0U};
  std::uint64_t minimizeButton{0U};
  std::uint64_t maximizeButton{0U};
  std::uint64_t closeButton{0U};
  std::uint64_t leftResize{0U};
  std::uint64_t rightResize{0U};
  std::uint64_t topResize{0U};
  std::uint64_t bottomResize{0U};
  std::uint64_t topLeftResize{0U};
  std::uint64_t topRightResize{0U};
  std::uint64_t bottomLeftResize{0U};
  std::uint64_t bottomRightResize{0U};
};

class Window {
 public:
  Window(
      std::uint32_t width,
      std::uint32_t height,
      const std::string& title,
      const WindowCreateOptions& options = {});
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool shouldClose() const;
  void requestClose() const;
  void setEventDispatcher(volt::event::EventDispatcher* dispatcher);
  void setResizeRepaintCallback(std::function<void()> callback);

  void pollEvents();
  void waitEvents();
  void waitEventsTimeout(double timeoutSeconds);

  [[nodiscard]] bool wasResized() const;
  void acknowledgeResize();

  [[nodiscard]] bool isMinimized() const;
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> framebufferExtent() const;
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> logicalExtent() const;
  [[nodiscard]] DisplayMetrics displayMetrics() const;
  [[nodiscard]] const InputState& inputSnapshot() const;

  [[nodiscard]] WindowBackendType backendType() const;
  [[nodiscard]] void* nativeWindowHandle() const;
  [[nodiscard]] void* nativeDisplayHandle() const;

  void beginInteractiveMove();
  void beginInteractiveResize(WindowResizeEdge edge);
  [[nodiscard]] bool isMaximized() const;
  void toggleMaximized();
  void minimize();

  [[nodiscard]] const std::string& title() const;
  [[nodiscard]] const char* titleIconTextureId() const;
  [[nodiscard]] std::string_view minimizeLabel() const;
  [[nodiscard]] std::string_view maximizeLabel() const;
  [[nodiscard]] std::string_view closeLabel() const;
  [[nodiscard]] WindowChromeLayout buildChromeLayout(
      std::uint32_t framebufferWidth,
      std::uint32_t framebufferHeight) const;
  void handleChromePointerPress(
      bool leftMousePressed,
      std::uint64_t hoveredWidgetId,
      const WindowChromeWidgetIds& widgetIds);

 private:
  std::string title_{};
  WindowCreateOptions options_{};
  std::unique_ptr<WindowBackend> backend_;
};

}  // namespace volt::platform
