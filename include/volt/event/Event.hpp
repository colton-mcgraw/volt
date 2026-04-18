#pragma once

#include "volt/event/EventType.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace volt::event {

struct FrameLifecycleEvent {
  std::uint64_t frameIndex{0};
};

struct WindowResizeEvent {
  std::uint32_t width{0};
  std::uint32_t height{0};
};

struct WindowMinimizedEvent {
  bool minimized{false};
};

struct KeyInputEvent {
  int key{0};
  int action{0};
  int mods{0};
};

struct TextInputEvent {
  char32_t codepoint{0};
};

struct MouseMovedEvent {
  double x{0.0};
  double y{0.0};
  double deltaX{0.0};
  double deltaY{0.0};
};

struct MouseButtonEvent {
  int button{0};
  int action{0};
  int mods{0};
};

struct MouseScrolledEvent {
  double xOffset{0.0};
  double yOffset{0.0};
};

struct RenderStageEvent {
  RenderStage stage{RenderStage::kFrameBegin};
  std::uint64_t frameIndex{0};
};

struct UiStageEvent {
  UiStage stage{UiStage::kFrameBegin};
  std::uint64_t frameIndex{0};
};

struct ImportLifecycleEvent {
  ImportStage stage{ImportStage::kStart};
  std::string path;
  bool success{false};
};

using EventPayload = std::variant<
    std::monostate,
    FrameLifecycleEvent,
    WindowResizeEvent,
    WindowMinimizedEvent,
    KeyInputEvent,
  TextInputEvent,
    MouseMovedEvent,
    MouseButtonEvent,
    MouseScrolledEvent,
    RenderStageEvent,
    UiStageEvent,
    ImportLifecycleEvent>;

struct Event {
  EventType type{EventType::kUnknown};
  EventPayload payload{};
  std::uint64_t sequence{0};
};

}  // namespace volt::event
