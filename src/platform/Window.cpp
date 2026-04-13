#include "volt/platform/Window.hpp"

#include "volt/core/Logging.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"

#include <stdexcept>
#include <utility>

namespace volt::platform {

Window::Window(std::uint32_t width, std::uint32_t height, const std::string& title) {
  if (glfwRefCount_ == 0) {
    if (glfwInit() != GLFW_TRUE) {
      throw std::runtime_error("Failed to initialize GLFW");
    }
  }
  ++glfwRefCount_;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

  window_ = glfwCreateWindow(
      static_cast<int>(width),
      static_cast<int>(height),
      title.c_str(),
      nullptr,
      nullptr);

  if (window_ == nullptr) {
    --glfwRefCount_;
    if (glfwRefCount_ == 0) {
      glfwTerminate();
    }
    throw std::runtime_error("Failed to create GLFW window");
  }

  glfwSetWindowUserPointer(window_, this);
  glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);
  glfwSetWindowRefreshCallback(window_, refreshCallback);
  glfwSetWindowIconifyCallback(window_, iconifyCallback);
  glfwSetKeyCallback(window_, keyCallback);
  glfwSetCursorPosCallback(window_, cursorPosCallback);
  glfwSetMouseButtonCallback(window_, mouseButtonCallback);
  glfwSetScrollCallback(window_, scrollCallback);

  glfwGetCursorPos(window_, &inputState_.mouse.x, &inputState_.mouse.y);
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

Window::~Window() {
  if (window_ != nullptr) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }

  --glfwRefCount_;
  if (glfwRefCount_ == 0) {
    glfwTerminate();
  }
}

bool Window::shouldClose() const {
  return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void Window::requestClose() const {
  glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void Window::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  eventDispatcher_ = dispatcher;
}

void Window::setResizeRepaintCallback(std::function<void()> callback) {
  resizeRepaintCallback_ = std::move(callback);
}

void Window::pollEvents() {
  resetTransientInputState();
  glfwPollEvents();
}

void Window::waitEvents() {
  resetTransientInputState();
  glfwWaitEvents();
}

void Window::waitEventsTimeout(double timeoutSeconds) {
  resetTransientInputState();
  glfwWaitEventsTimeout(timeoutSeconds > 0.0 ? timeoutSeconds : 0.0);
}

bool Window::wasResized() const {
  return framebufferResized_;
}

void Window::acknowledgeResize() {
  framebufferResized_ = false;
}

bool Window::isMinimized() const {
  return minimized_;
}

std::pair<std::uint32_t, std::uint32_t> Window::framebufferExtent() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  return {
      static_cast<std::uint32_t>(width > 0 ? width : 0),
      static_cast<std::uint32_t>(height > 0 ? height : 0),
  };
}

const InputState& Window::inputSnapshot() const {
  return inputState_;
}

GLFWwindow* Window::nativeHandle() const {
  return window_;
}

void Window::framebufferSizeCallback(GLFWwindow* window, int width, int height) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr) {
    return;
  }

  self->framebufferResized_ = true;
  self->minimized_ = (width == 0 || height == 0);
  VOLT_LOG_DEBUG_CAT(volt::core::logging::Category::kPlatform, "Window framebuffer resize: ", width, "x", height);

  if (self->eventDispatcher_ != nullptr) {
    self->eventDispatcher_->enqueue({
        .type = volt::event::EventType::kWindowResized,
        .payload = volt::event::WindowResizeEvent{
            .width = static_cast<std::uint32_t>(width > 0 ? width : 0),
            .height = static_cast<std::uint32_t>(height > 0 ? height : 0),
        },
    });
  }

  if (!self->minimized_ && self->resizeRepaintCallback_) {
    self->resizeRepaintCallback_();
  }
}

void Window::refreshCallback(GLFWwindow* window) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr) {
    return;
  }

  if (!self->minimized_ && self->resizeRepaintCallback_) {
    self->resizeRepaintCallback_();
  }
}

void Window::iconifyCallback(GLFWwindow* window, int iconified) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr) {
    return;
  }

  self->minimized_ = (iconified == GLFW_TRUE);
  VOLT_LOG_DEBUG_CAT(
      volt::core::logging::Category::kPlatform,
      "Window iconify state changed: ",
      self->minimized_ ? "minimized" : "restored");

  if (self->eventDispatcher_ != nullptr) {
    self->eventDispatcher_->enqueue({
        .type = volt::event::EventType::kWindowMinimized,
        .payload = volt::event::WindowMinimizedEvent{
            .minimized = (iconified == GLFW_TRUE),
        },
    });
  }
}

void Window::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr || key < 0) {
    return;
  }

  const std::size_t keyIndex = static_cast<std::size_t>(key);
  if (keyIndex >= self->inputState_.keyboard.down.size()) {
    return;
  }

  if (action == GLFW_PRESS) {
    self->inputState_.keyboard.down[keyIndex] = true;
  } else if (action == GLFW_RELEASE) {
    self->inputState_.keyboard.down[keyIndex] = false;
  }

  if (self->eventDispatcher_ != nullptr) {
    self->eventDispatcher_->enqueue({
        .type = volt::event::EventType::kKeyInput,
        .payload = volt::event::KeyInputEvent{
            .key = key,
            .action = action,
            .mods = mods,
        },
    });
  }

  (void)scancode;
  (void)mods;
}

void Window::cursorPosCallback(GLFWwindow* window, double xPos, double yPos) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr) {
    return;
  }

  self->inputState_.mouse.deltaX += (xPos - self->inputState_.mouse.x);
  self->inputState_.mouse.deltaY += (yPos - self->inputState_.mouse.y);
  self->inputState_.mouse.x = xPos;
  self->inputState_.mouse.y = yPos;

  if (self->eventDispatcher_ != nullptr) {
    self->eventDispatcher_->enqueue({
        .type = volt::event::EventType::kMouseMoved,
        .payload = volt::event::MouseMovedEvent{
            .x = self->inputState_.mouse.x,
            .y = self->inputState_.mouse.y,
            .deltaX = self->inputState_.mouse.deltaX,
            .deltaY = self->inputState_.mouse.deltaY,
        },
    });
  }
}

void Window::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr || button < 0) {
    return;
  }

  const std::size_t buttonIndex = static_cast<std::size_t>(button);
  if (buttonIndex >= self->inputState_.mouse.down.size()) {
    return;
  }

  if (action == GLFW_PRESS) {
    self->inputState_.mouse.down[buttonIndex] = true;
  } else if (action == GLFW_RELEASE) {
    self->inputState_.mouse.down[buttonIndex] = false;
  }

  if (self->eventDispatcher_ != nullptr) {
    self->eventDispatcher_->enqueue({
        .type = volt::event::EventType::kMouseButton,
        .payload = volt::event::MouseButtonEvent{
            .button = button,
            .action = action,
            .mods = mods,
        },
    });
  }

  (void)mods;
}

void Window::scrollCallback(GLFWwindow* window, double xOffset, double yOffset) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr) {
    return;
  }

  self->inputState_.mouse.scrollX += xOffset;
  self->inputState_.mouse.scrollY += yOffset;

  if (self->eventDispatcher_ != nullptr) {
    self->eventDispatcher_->enqueue({
        .type = volt::event::EventType::kMouseScrolled,
        .payload = volt::event::MouseScrolledEvent{
            .xOffset = xOffset,
            .yOffset = yOffset,
        },
    });
  }
}

void Window::resetTransientInputState() {
  inputState_.mouse.deltaX = 0.0;
  inputState_.mouse.deltaY = 0.0;
  inputState_.mouse.scrollX = 0.0;
  inputState_.mouse.scrollY = 0.0;
}

}  // namespace volt::platform
