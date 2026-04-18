#include "volt/platform/Window.hpp"

#include "volt/core/Logging.hpp"
#include "volt/platform/WindowBackendFactory.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace volt::platform {

namespace {

constexpr float kMinimumCaptionButtonSlotWidthPx = 78.0F;

[[nodiscard]] WindowChromeRect makeRect(float x, float y, float width, float height) {
  return WindowChromeRect{
      x,
      y,
      std::max(0.0F, width),
      std::max(0.0F, height),
  };
}

}  // namespace

Window::Window(
    std::uint32_t width,
    std::uint32_t height,
    const std::string& title,
    const WindowCreateOptions& options)
    : title_(title),
      options_(options) {
  backend_ = createWindowBackend(width, height, title, options);
  if (!backend_) {
    throw std::runtime_error("Failed to create platform window backend");
  }

  VOLT_LOG_INFO_CAT(
      volt::core::logging::Category::kPlatform,
      "Window created: ",
      title,
      " (",
      width,
      "x",
      height,
      ")");
}

Window::~Window() = default;

bool Window::shouldClose() const {
  return backend_->shouldClose();
}

void Window::requestClose() const {
  backend_->requestClose();
}

void Window::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  backend_->setEventDispatcher(dispatcher);
}

void Window::setResizeRepaintCallback(std::function<void()> callback) {
  backend_->setResizeRepaintCallback(std::move(callback));
}

void Window::pollEvents() {
  backend_->pollEvents();
}

void Window::waitEvents() {
  backend_->waitEvents();
}

void Window::waitEventsTimeout(double timeoutSeconds) {
  backend_->waitEventsTimeout(timeoutSeconds);
}

bool Window::wasResized() const {
  return backend_->wasResized();
}

void Window::acknowledgeResize() {
  backend_->acknowledgeResize();
}

bool Window::isMinimized() const {
  return backend_->isMinimized();
}

std::pair<std::uint32_t, std::uint32_t> Window::framebufferExtent() const {
  return backend_->framebufferExtent();
}

std::pair<std::uint32_t, std::uint32_t> Window::logicalExtent() const {
  return backend_->logicalExtent();
}

DisplayMetrics Window::displayMetrics() const {
  return backend_->displayMetrics();
}

const InputState& Window::inputSnapshot() const {
  return backend_->inputSnapshot();
}

WindowBackendType Window::backendType() const {
  return backend_->backendType();
}

void* Window::nativeWindowHandle() const {
  return backend_->nativeWindowHandle();
}

void* Window::nativeDisplayHandle() const {
  return backend_->nativeDisplayHandle();
}

void Window::beginInteractiveMove() {
  backend_->beginInteractiveMove();
}

void Window::beginInteractiveResize(WindowResizeEdge edge) {
  backend_->beginInteractiveResize(edge);
}

bool Window::isMaximized() const {
  return backend_->isMaximized();
}

void Window::toggleMaximized() {
  backend_->toggleMaximized();
}

void Window::minimize() {
  backend_->minimize();
}

const std::string& Window::title() const {
  return title_;
}

const char* Window::titleIconTextureId() const {
  return "image:icon";
}

std::string_view Window::minimizeLabel() const {
  return "- Min";
}

std::string_view Window::maximizeLabel() const {
  return "[] Max";
}

std::string_view Window::closeLabel() const {
  return "X Close";
}

WindowChromeLayout Window::buildChromeLayout(
    std::uint32_t framebufferWidth,
    std::uint32_t framebufferHeight) const {
  WindowChromeLayout layout{};

  const float windowWidthF = static_cast<float>(framebufferWidth);
  const float windowHeightF = static_cast<float>(framebufferHeight);
  const float titleBarHeight = static_cast<float>(std::max(1, options_.titleBarHeight));
  const float titleBarPadding = static_cast<float>(std::max(1, options_.titleBarPadding));
  const float titleBarButtonSize = static_cast<float>(std::max(1, options_.titleBarButtonSize));
  const float titleBarButtonSpacing = static_cast<float>(std::max(0, options_.titleBarButtonSpacing));
  const float baseButtonSlotWidth = titleBarButtonSize + (titleBarPadding * 2.0F);
  const float controlButtonWidth = std::max(baseButtonSlotWidth, kMinimumCaptionButtonSlotWidthPx);
  const float titleButtonsGroupWidth = (controlButtonWidth * 3.0F) + (titleBarButtonSpacing * 2.0F);
  const float buttonStripX = std::max(0.0F, windowWidthF - titleButtonsGroupWidth);
  const float titleIconSize = std::max(1.0F, titleBarHeight - (titleBarPadding * 2.0F));
  const float titleIconY = std::max(0.0F, (titleBarHeight - titleIconSize) * 0.5F);
  const float titleTextX = titleBarPadding + titleIconSize + titleBarPadding;
  const float titleTextWidth =
      std::max(0.0F, windowWidthF - titleButtonsGroupWidth - titleTextX - titleBarPadding);
  const float resizeGrip = static_cast<float>(
      options_.resizeBorderThickness > 0 ? options_.resizeBorderThickness : 6);

  layout.titleBar = makeRect(0.0F, 0.0F, windowWidthF, titleBarHeight);
  layout.titleIcon = makeRect(titleBarPadding, titleIconY, titleIconSize, titleIconSize);
  layout.titleText = makeRect(titleTextX, 0.0F, titleTextWidth, 18.0F);
  layout.dragRegion = makeRect(0.0F, 0.0F, buttonStripX, titleBarHeight);
  layout.minimizeButton = makeRect(buttonStripX, 0.0F, controlButtonWidth, titleBarHeight);
  layout.maximizeButton = makeRect(
      buttonStripX + controlButtonWidth + titleBarButtonSpacing,
      0.0F,
      controlButtonWidth,
      titleBarHeight);
  layout.closeButton = makeRect(
      buttonStripX + (controlButtonWidth * 2.0F) + (titleBarButtonSpacing * 2.0F),
      0.0F,
      controlButtonWidth,
      titleBarHeight);
  layout.titleBarPadding = titleBarPadding;
  layout.titleBarButtonSpacing = titleBarButtonSpacing;
  layout.titleTextBaselineY = std::max(0.0F, (titleBarHeight - 18.0F) * 0.5F);
  layout.buttonTextBaselineY = std::max(0.0F, (titleBarHeight - 14.0F) * 0.5F);

  const bool resizable = options_.windowResizable && !isMaximized();
  layout.hasResizeRegions = resizable;
  if (resizable) {
    const float cornerGrip = resizeGrip * 2.0F;
    layout.leftResize = makeRect(0.0F, 0.0F, resizeGrip, windowHeightF);
    layout.rightResize = makeRect(windowWidthF - resizeGrip, 0.0F, resizeGrip, windowHeightF);
    layout.topResize = makeRect(0.0F, 0.0F, windowWidthF, resizeGrip);
    layout.bottomResize = makeRect(0.0F, windowHeightF - resizeGrip, windowWidthF, resizeGrip);
    layout.topLeftResize = makeRect(0.0F, 0.0F, cornerGrip, cornerGrip);
    layout.topRightResize = makeRect(windowWidthF - cornerGrip, 0.0F, cornerGrip, cornerGrip);
    layout.bottomLeftResize = makeRect(0.0F, windowHeightF - cornerGrip, cornerGrip, cornerGrip);
    layout.bottomRightResize =
        makeRect(windowWidthF - cornerGrip, windowHeightF - cornerGrip, cornerGrip, cornerGrip);
  }

  return layout;
}

void Window::handleChromePointerPress(
    bool leftMousePressed,
    std::uint64_t hoveredWidgetId,
    const WindowChromeWidgetIds& widgetIds) {
  if (!leftMousePressed || hoveredWidgetId == 0U) {
    return;
  }

  if (hoveredWidgetId == widgetIds.closeButton) {
    requestClose();
  } else if (hoveredWidgetId == widgetIds.minimizeButton) {
    minimize();
  } else if (hoveredWidgetId == widgetIds.maximizeButton) {
    toggleMaximized();
  } else if (hoveredWidgetId == widgetIds.dragRegion) {
    beginInteractiveMove();
  } else if (hoveredWidgetId == widgetIds.leftResize) {
    beginInteractiveResize(WindowResizeEdge::kLeft);
  } else if (hoveredWidgetId == widgetIds.rightResize) {
    beginInteractiveResize(WindowResizeEdge::kRight);
  } else if (hoveredWidgetId == widgetIds.topResize) {
    beginInteractiveResize(WindowResizeEdge::kTop);
  } else if (hoveredWidgetId == widgetIds.bottomResize) {
    beginInteractiveResize(WindowResizeEdge::kBottom);
  } else if (hoveredWidgetId == widgetIds.topLeftResize) {
    beginInteractiveResize(WindowResizeEdge::kTopLeft);
  } else if (hoveredWidgetId == widgetIds.topRightResize) {
    beginInteractiveResize(WindowResizeEdge::kTopRight);
  } else if (hoveredWidgetId == widgetIds.bottomLeftResize) {
    beginInteractiveResize(WindowResizeEdge::kBottomLeft);
  } else if (hoveredWidgetId == widgetIds.bottomRightResize) {
    beginInteractiveResize(WindowResizeEdge::kBottomRight);
  }
}

}  // namespace volt::platform
