#include "volt/ui/UILayer.hpp"

#include "volt/core/Logging.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"
#include "volt/io/assets/AssetManager.hpp"
#include "volt/ui/UIMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <type_traits>
#include <utility>

namespace volt::ui {

namespace {

bool pointInRect(double x, double y, const Rect& rect) {
  return x >= rect.x && y >= rect.y && x <= (rect.x + rect.width) && y <= (rect.y + rect.height);
}

Rect intersectRect(const Rect& a, const Rect& b) {
  const float x0 = std::max(a.x, b.x);
  const float y0 = std::max(a.y, b.y);
  const float x1 = std::min(a.x + a.width, b.x + b.width);
  const float y1 = std::min(a.y + a.height, b.y + b.height);
  return {
      x0,
      y0,
      std::max(0.0F, x1 - x0),
      std::max(0.0F, y1 - y0),
  };
}

Rect insetRect(const Rect& bounds, float padding) {
  const float clampedPadding = std::max(0.0F, padding);
  const float insetX = std::min(clampedPadding, bounds.width * 0.5F);
  const float insetY = std::min(clampedPadding, bounds.height * 0.5F);
  return {
      bounds.x + insetX,
      bounds.y + insetY,
      std::max(0.0F, bounds.width - (insetX * 2.0F)),
      std::max(0.0F, bounds.height - (insetY * 2.0F)),
  };
}

Color mixColor(const Color& lhs, const Color& rhs, float t) {
  const float clampedT = std::clamp(t, 0.0F, 1.0F);
  return {
      lhs.r + (rhs.r - lhs.r) * clampedT,
      lhs.g + (rhs.g - lhs.g) * clampedT,
      lhs.b + (rhs.b - lhs.b) * clampedT,
      lhs.a + (rhs.a - lhs.a) * clampedT,
  };
}

std::string encodeUtf8(char32_t codepoint) {
  std::string utf8;
  if (codepoint <= 0x7FU) {
    utf8.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFU) {
    utf8.push_back(static_cast<char>(0xC0U | ((codepoint >> 6U) & 0x1FU)));
    utf8.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0xFFFFU) {
    utf8.push_back(static_cast<char>(0xE0U | ((codepoint >> 12U) & 0x0FU)));
    utf8.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    utf8.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint <= 0x10FFFFU) {
    utf8.push_back(static_cast<char>(0xF0U | ((codepoint >> 18U) & 0x07U)));
    utf8.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    utf8.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    utf8.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
  return utf8;
}

std::vector<GlyphCluster> decodeUtf8Glyphs(std::string_view text) {
  std::vector<GlyphCluster> glyphs;
  const bool decodeOk = decodeUtf8(text, &glyphs);
  (void)decodeOk;
  return glyphs;
}

std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
  return seed;
}

std::uint64_t hashFloat(float value) {
  std::uint32_t bits = 0U;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return static_cast<std::uint64_t>(bits);
}

std::uint64_t hashRect(const Rect& rect) {
  std::uint64_t hash = 0U;
  hash = hashCombine(hash, hashFloat(rect.x));
  hash = hashCombine(hash, hashFloat(rect.y));
  hash = hashCombine(hash, hashFloat(rect.width));
  hash = hashCombine(hash, hashFloat(rect.height));
  return hash;
}

std::uint64_t hashColor(const Color& color) {
  std::uint64_t hash = 0U;
  hash = hashCombine(hash, hashFloat(color.r));
  hash = hashCombine(hash, hashFloat(color.g));
  hash = hashCombine(hash, hashFloat(color.b));
  hash = hashCombine(hash, hashFloat(color.a));
  return hash;
}

Rect translateRect(const Rect& rect, float dx, float dy) {
  return Rect{rect.x + dx, rect.y + dy, rect.width, rect.height};
}

std::uint64_t hashUiRenderCommand(const UiRenderCommand& command) {
  return std::visit(
      [](const auto& typed) -> std::uint64_t {
        using T = std::decay_t<decltype(typed)>;
        std::uint64_t hash = 0U;
        hash = hashCombine(hash, static_cast<std::uint64_t>(typed.widgetId));
        hash = hashCombine(hash, hashRect(typed.bounds));
        hash = hashCombine(hash, hashRect(typed.clipRect));
        if constexpr (std::is_same_v<T, UiTextCommand>) {
          hash = hashCombine(hash, std::hash<std::string>{}(typed.text));
          hash = hashCombine(hash, std::hash<std::string>{}(typed.fontFamily));
          hash = hashCombine(hash, hashFloat(typed.fontSizePx));
          hash = hashCombine(hash, static_cast<std::uint64_t>(typed.glyphCount));
          hash = hashCombine(hash, hashColor(typed.color));
        } else if constexpr (std::is_same_v<T, UiRectCommand>) {
          hash = hashCombine(hash, hashColor(typed.fill));
          hash = hashCombine(hash, hashFloat(typed.cornerRadiusPx));
        } else if constexpr (std::is_same_v<T, UiImageCommand>) {
          hash = hashCombine(hash, std::hash<std::string>{}(typed.imageKey));
          hash = hashCombine(hash, hashColor(typed.tint));
        } else if constexpr (std::is_same_v<T, UiIconCommand>) {
          hash = hashCombine(hash, std::hash<std::string>{}(typed.iconKey));
          hash = hashCombine(hash, hashColor(typed.tint));
        } else if constexpr (std::is_same_v<T, UiChartScaffoldCommand>) {
          hash = hashCombine(hash, std::hash<std::string>{}(typed.chartKind));
        } else if constexpr (std::is_same_v<T, UiSchematicScaffoldCommand>) {
          hash = hashCombine(hash, std::hash<std::string>{}(typed.modelName));
        }
        return hash;
      },
      command);
}

std::uint64_t hashUiRenderCommands(const std::vector<UiRenderCommand>& commands, const Rect& bounds, const Rect& clipRect) {
  std::uint64_t hash = 0U;
  hash = hashCombine(hash, hashRect(bounds));
  hash = hashCombine(hash, hashRect(clipRect));
  hash = hashCombine(hash, static_cast<std::uint64_t>(commands.size()));
  for (const UiRenderCommand& command : commands) {
    hash = hashCombine(hash, hashUiRenderCommand(command));
  }
  return hash;
}

UiRenderCommand localizeUiRenderCommand(const UiRenderCommand& command, const Rect& panelBounds) {
  const float dx = -panelBounds.x;
  const float dy = -panelBounds.y;
  return std::visit(
      [&](const auto& typed) -> UiRenderCommand {
        using T = std::decay_t<decltype(typed)>;
        T localized = typed;
        localized.bounds = translateRect(localized.bounds, dx, dy);
        localized.clipRect = translateRect(localized.clipRect, dx, dy);
        return UiRenderCommand{localized};
      },
      command);
}

std::size_t previousUtf8Boundary(std::string_view text, std::size_t byteIndex) {
  if (byteIndex == 0) {
    return 0;
  }

  const std::vector<GlyphCluster> glyphs = decodeUtf8Glyphs(text);
  std::size_t previous = 0;
  for (const GlyphCluster& glyph : glyphs) {
    if (glyph.byteOffset >= byteIndex) {
      break;
    }
    previous = glyph.byteOffset;
  }
  return previous;
}

std::size_t nextUtf8Boundary(std::string_view text, std::size_t byteIndex) {
  const std::vector<GlyphCluster> glyphs = decodeUtf8Glyphs(text);
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    if (glyphs[i].byteOffset <= byteIndex) {
      continue;
    }
    return glyphs[i].byteOffset;
  }
  return text.size();
}

std::size_t utf8GlyphCount(std::string_view text) {
  return decodeUtf8Glyphs(text).size();
}

void erasePreviousUtf8Codepoint(std::string* text, std::size_t& cursorByteIndex) {
  if (text == nullptr || cursorByteIndex == 0 || cursorByteIndex > text->size()) {
    return;
  }

  const std::size_t eraseBegin = previousUtf8Boundary(*text, cursorByteIndex);
  text->erase(eraseBegin, cursorByteIndex - eraseBegin);
  cursorByteIndex = eraseBegin;
}

void eraseCurrentUtf8Codepoint(std::string* text, std::size_t& cursorByteIndex) {
  if (text == nullptr || cursorByteIndex >= text->size()) {
    return;
  }

  const std::size_t eraseEnd = nextUtf8Boundary(*text, cursorByteIndex);
  text->erase(cursorByteIndex, eraseEnd - cursorByteIndex);
}

void insertUtf8Codepoint(std::string* text, std::size_t& cursorByteIndex, char32_t codepoint, std::size_t maxLength) {
  if (text == nullptr || codepoint < 0x20 || codepoint == 0x7FU) {
    return;
  }

  if (maxLength > 0U && utf8GlyphCount(*text) >= maxLength) {
    return;
  }

  const std::string utf8 = encodeUtf8(codepoint);
  if (utf8.empty()) {
    return;
  }

  cursorByteIndex = std::min(cursorByteIndex, text->size());
  text->insert(cursorByteIndex, utf8);
  cursorByteIndex += utf8.size();
}

std::string injectCaretMarker(std::string text, std::size_t cursorByteIndex) {
  const std::size_t clampedCursor = std::min(cursorByteIndex, text.size());
  text.insert(clampedCursor, "|");
  return text;
}

constexpr int kKeyBackspace = 0x08;
constexpr int kKeyTab = 0x09;
constexpr int kKeyReturn = 0x0D;
constexpr int kKeyEscape = 0x1B;
constexpr int kKeyHome = 0x24;
constexpr int kKeyLeft = 0x25;
constexpr int kKeyRight = 0x27;
constexpr int kKeyEnd = 0x23;
constexpr int kKeyDelete = 0x2E;

void applyTextInputEvents(
    const std::vector<volt::event::KeyInputEvent>& keyEvents,
    const std::vector<char32_t>& codepoints,
    std::string* text,
    std::size_t maxLength,
    std::size_t& cursorByteIndex) {
  if (text == nullptr) {
    return;
  }

  cursorByteIndex = std::min(cursorByteIndex, text->size());

  for (const volt::event::KeyInputEvent& event : keyEvents) {
    switch (event.key) {
      case kKeyBackspace:
        erasePreviousUtf8Codepoint(text, cursorByteIndex);
        break;
      case kKeyDelete:
        eraseCurrentUtf8Codepoint(text, cursorByteIndex);
        break;
      case kKeyLeft:
        cursorByteIndex = previousUtf8Boundary(*text, cursorByteIndex);
        break;
      case kKeyRight:
        cursorByteIndex = nextUtf8Boundary(*text, cursorByteIndex);
        break;
      case kKeyHome:
        cursorByteIndex = 0;
        break;
      case kKeyEnd:
        cursorByteIndex = text->size();
        break;
      case kKeyTab:
      case kKeyReturn:
      case kKeyEscape:
        break;
      default:
        break;
    }
  }

  for (const char32_t codepoint : codepoints) {
    if (codepoint == U'\t' || codepoint == U'\r' || codepoint == U'\n') {
      continue;
    }
    insertUtf8Codepoint(text, cursorByteIndex, codepoint, maxLength);
  }
}

float accumulatedPanelScrollOffsetY(const std::vector<UILayer::PanelContext>& panelStack) {
  float offsetY = 0.0F;
  for (const UILayer::PanelContext& panel : panelStack) {
    offsetY += panel.scrollOffsetY;
  }
  return offsetY;
}

Rect translateRectForPanelStack(const Rect& bounds, const std::vector<UILayer::PanelContext>& panelStack) {
  Rect translated = bounds;
  translated.y -= accumulatedPanelScrollOffsetY(panelStack);
  return translated;
}

Rect clipRectToPanelStack(const Rect& bounds, const std::vector<UILayer::PanelContext>& panelStack) {
  if (panelStack.empty()) {
    return bounds;
  }

  return intersectRect(bounds, panelStack.back().clipRect);
}

Rect currentClipRectForPanelStack(const UILayer::FrameArgs& frameArgs, const std::vector<UILayer::PanelContext>& panelStack) {
  if (!panelStack.empty()) {
    return panelStack.back().clipRect;
  }

  return Rect{
      0.0F,
      0.0F,
      std::max(0.0F, static_cast<float>(frameArgs.width)),
      std::max(0.0F, static_cast<float>(frameArgs.height)),
  };
}

bool isWidgetEnabled(const UILayer::WidgetNode& widget) {
  switch (widget.kind) {
    case WidgetKind::kButton:
      return std::get<ButtonElement>(widget.payload).enabled;
    case WidgetKind::kCheckbox:
      return std::get<CheckboxElement>(widget.payload).enabled;
    case WidgetKind::kToggle:
      return std::get<ToggleElement>(widget.payload).enabled;
    case WidgetKind::kSlider:
      return std::get<SliderElement>(widget.payload).enabled;
    case WidgetKind::kTextInput:
      return std::get<TextInputElement>(widget.payload).enabled;
    default:
      return true;
  }
}

bool participatesInHoverHitTest(WidgetKind kind) {
  switch (kind) {
    case WidgetKind::kText:
    case WidgetKind::kIcon:
    case WidgetKind::kImage:
      return false;
    default:
      return true;
  }
}

float cachedImageAspectRatio(const std::string& imageKey) {
  if (imageKey.empty() || imageKey == "__white") {
    return 1.0F;
  }

  static std::unordered_map<std::string, float> aspectRatios;
  volt::io::AssetManager& assets = volt::io::AssetManager::instance();

  const auto it = aspectRatios.find(imageKey);
  if (it != aspectRatios.end() && !assets.hasImageChanged(imageKey)) {
    return std::max(0.01F, it->second);
  }

  const volt::io::LoadedImageAsset image = assets.loadImage(imageKey);
  const float aspectRatio = image.height > 0U
      ? static_cast<float>(image.width) / static_cast<float>(image.height)
      : 1.0F;
  aspectRatios[imageKey] = std::max(0.01F, aspectRatio);
  return aspectRatios[imageKey];
}

Rect allocateFlowImageRect(UILayer::FlowColumn& column, float height, float aspectRatio) {
  const float clampedHeight = std::max(0.0F, height);
  const float clampedAspectRatio = std::max(0.01F, aspectRatio);
  const float x = column.bounds.x + column.padding;
  const float maxWidth = std::max(0.0F, column.bounds.width - (column.padding * 2.0F));
  const float requestedWidth = clampedHeight * clampedAspectRatio;
  const float finalWidth = std::min(maxWidth, requestedWidth);
  const float finalHeight = finalWidth > 0.0F ? (finalWidth / clampedAspectRatio) : 0.0F;
  const float y = column.cursorY;

  column.cursorY += finalHeight + column.spacing;
  return {x, y, finalWidth, finalHeight};
}

}  // namespace

void UILayer::beginFrame(const volt::platform::InputState& input, const FrameArgs& frameArgs) {
  ++frameIndex_;
  frameArgs_ = frameArgs;
  widgets_.clear();
  drawCommands_.clear();
  panelStack_.clear();
  flowColumns_.clear();
  flowRows_.clear();
  stackLayouts_.clear();
  dockLayouts_.clear();
  gridLayouts_.clear();
  textRuns_.clear();
  meshData_ = UiMeshData{};
  panelNameCounts_.clear();
  nextWidgetId_ = 1;

  if (!styleLoaded_) {
    styleLoaded_ = styleSheet_.loadFromFile("assets/ui/default.style");
    if (!styleLoaded_) {
      styleSheet_.applyDefaults();
      VOLT_LOG_WARN_CAT(volt::core::logging::Category::kUI, "UI style file not found; using defaults");
    }
  }

  // Scaffold behavior: keep packet counters deterministic while input/render internals are pending.
  packet_.drawListCount = 0U;
  packet_.clipRectCount = 0U;
  packet_.widgetCount = 0U;
  hoveredWidgetId_ = 0;
  const bool previousLeftMouseDown = leftMouseDown_;
  leftMouseDown_ = input.mouse.down[0];
  leftMousePressed_ = leftMouseDown_ && !previousLeftMouseDown;
  leftMouseReleased_ = !leftMouseDown_ && previousLeftMouseDown;
  lastMouseX_ = input.mouse.x;
  lastMouseY_ = input.mouse.y;
  scrollDeltaY_ = input.mouse.scrollY;
  VOLT_LOG_TRACE_CAT(
      volt::core::logging::Category::kUI,
      "UI begin frame ",
      frameIndex_,
      " size=",
      frameArgs_.width,
      "x",
      frameArgs_.height,
      " minimized=",
      frameArgs_.minimized ? "true" : "false");

  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kUiFrameBegin,
        .payload = volt::event::UiStageEvent{
            .stage = volt::event::UiStage::kFrameBegin,
            .frameIndex = frameIndex_,
        },
    });
  }
}

void UILayer::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  if (eventDispatcher_ != nullptr) {
    if (keyListenerId_ != 0) {
      eventDispatcher_->unsubscribe(keyListenerId_);
      keyListenerId_ = 0;
    }

    if (textInputListenerId_ != 0) {
      eventDispatcher_->unsubscribe(textInputListenerId_);
      textInputListenerId_ = 0;
    }

    if (mouseMoveListenerId_ != 0) {
      eventDispatcher_->unsubscribe(mouseMoveListenerId_);
      mouseMoveListenerId_ = 0;
    }

    if (mouseButtonListenerId_ != 0) {
      eventDispatcher_->unsubscribe(mouseButtonListenerId_);
      mouseButtonListenerId_ = 0;
    }

    if (mouseScrollListenerId_ != 0) {
      eventDispatcher_->unsubscribe(mouseScrollListenerId_);
      mouseScrollListenerId_ = 0;
    }

    if (renderUiPassListenerId_ != 0) {
      eventDispatcher_->unsubscribe(renderUiPassListenerId_);
      renderUiPassListenerId_ = 0;
    }
  }

  eventDispatcher_ = dispatcher;
  VOLT_LOG_DEBUG_CAT(
      volt::core::logging::Category::kUI,
      "UI event dispatcher ",
      eventDispatcher_ == nullptr ? "detached" : "attached");

  if (eventDispatcher_ != nullptr) {
    keyListenerId_ = eventDispatcher_->subscribe(
        volt::event::EventType::kKeyInput,
        [this](const volt::event::Event& event) {
          const auto* payload = std::get_if<volt::event::KeyInputEvent>(&event.payload);
          if (payload == nullptr) {
            return;
          }

          if (payload->action != 0) {
            keyInputEvents_.push_back(*payload);
            ++consumedInputEventCount_;
            VOLT_LOG_TRACE_CAT(
                volt::core::logging::Category::kUI,
                "UI consumed key input event count=",
                consumedInputEventCount_);
          }
        });

    textInputListenerId_ = eventDispatcher_->subscribe(
        volt::event::EventType::kTextInput,
        [this](const volt::event::Event& event) {
          const auto* payload = std::get_if<volt::event::TextInputEvent>(&event.payload);
          if (payload == nullptr) {
            return;
          }

          textInputCodepoints_.push_back(payload->codepoint);
          ++consumedInputEventCount_;
        });

    mouseMoveListenerId_ = eventDispatcher_->subscribe(
        volt::event::EventType::kMouseMoved,
        [this](const volt::event::Event& event) {
          const auto* payload = std::get_if<volt::event::MouseMovedEvent>(&event.payload);
          if (payload == nullptr) {
            return;
          }

          lastMouseX_ = payload->x;
          lastMouseY_ = payload->y;
          ++consumedInputEventCount_;
          VOLT_LOG_TRACE_CAT(
              volt::core::logging::Category::kUI,
              "UI consumed mouse move x=",
              lastMouseX_,
              " y=",
              lastMouseY_);
        });

    mouseButtonListenerId_ = eventDispatcher_->subscribe(
        volt::event::EventType::kMouseButton,
        [this](const volt::event::Event& event) {
          const auto* payload = std::get_if<volt::event::MouseButtonEvent>(&event.payload);
          if (payload == nullptr || payload->button != 0) {
            return;
          }

          ++consumedInputEventCount_;
        });

    mouseScrollListenerId_ = eventDispatcher_->subscribe(
        volt::event::EventType::kMouseScrolled,
        [this](const volt::event::Event& event) {
          const auto* payload = std::get_if<volt::event::MouseScrolledEvent>(&event.payload);
          if (payload == nullptr) {
            return;
          }

          if (payload->xOffset != 0.0 || payload->yOffset != 0.0) {
            ++consumedInputEventCount_;
            VOLT_LOG_TRACE_CAT(
                volt::core::logging::Category::kUI,
                "UI consumed scroll input count=",
                consumedInputEventCount_);
          }
        });

    renderUiPassListenerId_ = eventDispatcher_->subscribe(
        volt::event::EventType::kRenderUiPass,
        [this](const volt::event::Event& event) {
          const auto* payload = std::get_if<volt::event::RenderStageEvent>(&event.payload);
          if (payload == nullptr || payload->stage != volt::event::RenderStage::kUiPass) {
            return;
          }

          recordRenderPass();
        });
  }
}

void UILayer::layoutPass() {
  VOLT_LOG_TRACE_CAT(volt::core::logging::Category::kUI, "UI layout pass frame=", frameIndex_);

  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kUiLayoutPass,
        .payload = volt::event::UiStageEvent{
            .stage = volt::event::UiStage::kLayoutPass,
            .frameIndex = frameIndex_,
        },
    });
  }

  for (WidgetNode& widget : widgets_) {
    widget.bounds.x = std::max(0.0F, widget.bounds.x);
    widget.bounds.y = std::max(0.0F, widget.bounds.y);
    widget.bounds.width = std::max(0.0F, widget.bounds.width);
    widget.bounds.height = std::max(0.0F, widget.bounds.height);
  }

  for (auto it = widgets_.rbegin(); it != widgets_.rend(); ++it) {
    if (!participatesInHoverHitTest(it->kind) || !isWidgetEnabled(*it)) {
      continue;
    }

    if (pointInRect(lastMouseX_, lastMouseY_, it->bounds)) {
      hoveredWidgetId_ = it->id;
      break;
    }
  }

  if (leftMousePressed_) {
    focusedWidgetId_ = hoveredWidgetId_;
    activeWidgetId_ = hoveredWidgetId_;
  }

  if (!leftMouseDown_ && leftMouseReleased_) {
    activeWidgetId_ = 0;
  }
}

void UILayer::paintPass() {
  VOLT_LOG_TRACE_CAT(volt::core::logging::Category::kUI, "UI paint pass frame=", frameIndex_);

  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kUiPaintPass,
        .payload = volt::event::UiStageEvent{
            .stage = volt::event::UiStage::kPaintPass,
            .frameIndex = frameIndex_,
        },
    });
  }

  if (frameArgs_.minimized) {
    packet_.drawListCount = 0U;
    packet_.clipRectCount = 0U;
    packet_.widgetCount = static_cast<std::uint32_t>(widgets_.size());
    return;
  }

  drawCommands_.clear();
  drawCommands_.reserve(widgets_.size() * 4U);

  const ThemeTokens& theme = styleSheet_.theme();
  auto makeTextCommand = [this](
                             std::uint64_t widgetId,
                             const Rect& bounds,
                             const Rect& clipRect,
                             const std::string& text,
                             std::string fontFamily,
                             float fontSizePx,
                             const Color& color) {
    textRuns_.push_back(buildTextRun(text, fontFamily, fontSizePx));
    return UiTextCommand{
        widgetId,
        bounds,
        clipRect,
        text,
        std::move(fontFamily),
        fontSizePx,
        static_cast<std::uint32_t>(textRuns_.back().glyphs.size()),
        color,
    };
  };

  struct PendingRetainedPanel {
    WidgetNode* widget{nullptr};
    std::vector<UiRenderCommand> commands{};
  };

  struct OutputEntry {
    std::string retainedPanelKey{};
    std::vector<UiRenderCommand> commands{};
  };

  std::vector<OutputEntry> outputSequence;
  outputSequence.reserve(widgets_.size() * 2U);
  std::unordered_map<std::string, PendingRetainedPanel> pendingRetainedPanels;

  auto appendCommandForWidget = [&](WidgetNode& widget, UiRenderCommand command) {
    if (!widget.renderTargetOwnerKey.empty()) {
      auto pendingIt = pendingRetainedPanels.find(widget.renderTargetOwnerKey);
      if (pendingIt != pendingRetainedPanels.end()) {
        pendingIt->second.commands.push_back(std::move(command));
        return;
      }
    }

    OutputEntry entry{};
    entry.commands.push_back(std::move(command));
    outputSequence.push_back(std::move(entry));
  };

  bool focusedTextInputVisible = false;

  for (WidgetNode& widget : widgets_) {
    if (widget.cacheRenderTarget) {
      auto [pendingIt, inserted] = pendingRetainedPanels.try_emplace(widget.panelCacheKey);
      pendingIt->second.widget = &widget;
      if (inserted) {
        pendingIt->second.commands.reserve(32U);
      }

      OutputEntry retainedEntry{};
      retainedEntry.retainedPanelKey = widget.panelCacheKey;
      outputSequence.push_back(std::move(retainedEntry));
    }

    switch (widget.kind) {
      case WidgetKind::kText: {
        const auto& text = std::get<TextElement>(widget.payload);
        appendCommandForWidget(widget, makeTextCommand(widget.id, widget.bounds, widget.clipRect, text.text, text.fontFamily, text.fontSizePx, text.color));
      } break;
      case WidgetKind::kButton: {
        auto& button = std::get<ButtonElement>(widget.payload);
        const bool isEnabled = button.enabled;
        const bool isHovered = isEnabled && widget.id == hoveredWidgetId_;
        const bool isActive = isEnabled && widget.id == activeWidgetId_;
        const bool clicked = isEnabled && leftMouseReleased_ && isHovered && focusedWidgetId_ == widget.id;
        button.pressed = isActive;

        if (clicked) {
          VOLT_LOG_DEBUG_CAT(
              volt::core::logging::Category::kUI,
              "UI button clicked id=",
              widget.id,
              " label=",
              button.label);
        }

        Color bg = button.background;
        Color textColor = button.textColor;
        if (!isEnabled) {
          bg = mixColor(button.background, theme.panelBackground, 0.55F);
          bg.a *= 0.85F;
          textColor = mixColor(button.textColor, theme.textMuted, 0.65F);
          textColor.a *= 0.85F;
        } else if (isActive) {
          bg = mixColor(button.background, theme.buttonPressed, 0.65F);
        } else if (isHovered) {
          bg = mixColor(button.background, theme.buttonHover, 0.45F);
        }

        appendCommandForWidget(widget, UiRectCommand{widget.id, widget.bounds, widget.clipRect, bg, 6.0F});
        appendCommandForWidget(widget, makeTextCommand(widget.id, widget.bounds, widget.clipRect, button.label, "default", 14.0F, textColor));
      } break;
      case WidgetKind::kCheckbox: {
        auto& checkbox = std::get<CheckboxElement>(widget.payload);
        const bool isEnabled = checkbox.enabled;
        const bool isHovered = isEnabled && widget.id == hoveredWidgetId_;
        const bool clicked = isEnabled && leftMouseReleased_ && isHovered && focusedWidgetId_ == widget.id;
        if (clicked && checkbox.checked != nullptr) {
          *checkbox.checked = !*checkbox.checked;
        }

        const bool checked = checkbox.checked != nullptr && *checkbox.checked;
        Color boxColor = checkbox.boxColor;
        Color checkColor = checkbox.checkColor;
        Color textColor = checkbox.textColor;
        if (!isEnabled) {
          boxColor = mixColor(checkbox.boxColor, theme.panelBackground, 0.55F);
          checkColor = mixColor(checkbox.checkColor, theme.textMuted, 0.55F);
          textColor = mixColor(checkbox.textColor, theme.textMuted, 0.65F);
        } else if (isHovered) {
          boxColor = mixColor(checkbox.boxColor, theme.buttonHover, 0.28F);
        }

        const float boxSize = std::min(std::max(12.0F, widget.bounds.height - 8.0F), 18.0F);
        const Rect boxBounds{
            widget.bounds.x + 6.0F,
            widget.bounds.y + std::max(0.0F, (widget.bounds.height - boxSize) * 0.5F),
            boxSize,
            boxSize,
        };
        appendCommandForWidget(widget, UiRectCommand{widget.id, boxBounds, widget.clipRect, mixColor(boxColor, theme.textPrimary, 0.10F), 4.0F});
        appendCommandForWidget(
            widget,
            UiRectCommand{
                widget.id,
                {boxBounds.x + 1.5F, boxBounds.y + 1.5F, std::max(0.0F, boxBounds.width - 3.0F), std::max(0.0F, boxBounds.height - 3.0F)},
                widget.clipRect,
                boxColor,
                3.0F,
            });

        if (checked) {
          appendCommandForWidget(
              widget,
              UiRectCommand{
                  widget.id,
                  {boxBounds.x + 4.0F, boxBounds.y + 4.0F, std::max(0.0F, boxBounds.width - 8.0F), std::max(0.0F, boxBounds.height - 8.0F)},
                  widget.clipRect,
                  checkColor,
                  2.0F,
              });
        }

        const Rect textBounds{
            boxBounds.x + boxBounds.width + 8.0F,
            widget.bounds.y,
            std::max(0.0F, widget.bounds.width - ((boxBounds.x - widget.bounds.x) + boxBounds.width + 12.0F)),
            widget.bounds.height,
        };
        appendCommandForWidget(widget, makeTextCommand(widget.id, textBounds, widget.clipRect, checkbox.label, "default", 13.0F, textColor));
      } break;
      case WidgetKind::kToggle: {
        auto& toggle = std::get<ToggleElement>(widget.payload);
        const bool isEnabled = toggle.enabled;
        const bool isHovered = isEnabled && widget.id == hoveredWidgetId_;
        const bool clicked = isEnabled && leftMouseReleased_ && isHovered && focusedWidgetId_ == widget.id;
        if (clicked && toggle.value != nullptr) {
          *toggle.value = !*toggle.value;
        }

        const bool enabledValue = toggle.value != nullptr && *toggle.value;
        Color trackColor = enabledValue ? toggle.trackOnColor : toggle.trackOffColor;
        Color knobColor = toggle.knobColor;
        Color textColor = toggle.textColor;
        if (!isEnabled) {
          trackColor = mixColor(trackColor, theme.panelBackground, 0.55F);
          knobColor = mixColor(knobColor, theme.textMuted, 0.55F);
          textColor = mixColor(textColor, theme.textMuted, 0.65F);
        } else if (isHovered) {
          trackColor = mixColor(trackColor, theme.buttonHover, 0.25F);
        }

        const float trackWidth = std::min(42.0F, std::max(30.0F, widget.bounds.width * 0.28F));
        const float trackHeight = std::min(20.0F, std::max(14.0F, widget.bounds.height - 8.0F));
        const Rect trackBounds{
            widget.bounds.x + widget.bounds.width - trackWidth - 6.0F,
            widget.bounds.y + std::max(0.0F, (widget.bounds.height - trackHeight) * 0.5F),
            trackWidth,
            trackHeight,
        };
        appendCommandForWidget(widget, UiRectCommand{widget.id, trackBounds, widget.clipRect, trackColor, trackHeight * 0.5F});

        const float knobSize = std::max(10.0F, trackHeight - 4.0F);
        const float knobMinX = trackBounds.x + 2.0F;
        const float knobMaxX = trackBounds.x + std::max(2.0F, trackBounds.width - knobSize - 2.0F);
        const float knobX = enabledValue ? knobMaxX : knobMinX;
        appendCommandForWidget(widget, UiRectCommand{widget.id, {knobX, trackBounds.y + 2.0F, knobSize, knobSize}, widget.clipRect, knobColor, knobSize * 0.5F});

        const Rect textBounds{
            widget.bounds.x + 6.0F,
            widget.bounds.y,
            std::max(0.0F, trackBounds.x - widget.bounds.x - 12.0F),
            widget.bounds.height,
        };
        appendCommandForWidget(widget, makeTextCommand(widget.id, textBounds, widget.clipRect, toggle.label, "default", 13.0F, textColor));
      } break;
      case WidgetKind::kSlider: {
        auto& slider = std::get<SliderElement>(widget.payload);
        float sliderValue = slider.valueBinding != nullptr ? *slider.valueBinding : slider.value;
        const bool isEnabled = slider.enabled;
        const bool isHovered = isEnabled && widget.id == hoveredWidgetId_;
        const bool isActive = isEnabled && widget.id == activeWidgetId_;
        const bool canDrag = isEnabled && (isActive || (isHovered && leftMousePressed_));

        if (canDrag && leftMouseDown_ && slider.maxValue > slider.minValue && widget.bounds.width > 0.0F) {
          const float normalized =
              std::clamp(static_cast<float>((lastMouseX_ - widget.bounds.x) / widget.bounds.width), 0.0F, 1.0F);
          sliderValue = slider.minValue + normalized * (slider.maxValue - slider.minValue);
          slider.value = sliderValue;
          if (slider.valueBinding != nullptr) {
            *slider.valueBinding = sliderValue;
          }
        }

        Color trackColor = slider.trackColor;
        Color knobColor = slider.knobColor;
        if (!isEnabled) {
          trackColor = mixColor(slider.trackColor, theme.panelBackground, 0.5F);
          trackColor.a *= 0.85F;
          knobColor = mixColor(slider.knobColor, theme.textMuted, 0.55F);
          knobColor.a *= 0.85F;
        } else if (isActive) {
          knobColor = mixColor(slider.knobColor, theme.textPrimary, 0.35F);
        } else if (isHovered) {
          knobColor = mixColor(slider.knobColor, theme.textPrimary, 0.18F);
        }

        appendCommandForWidget(widget, UiRectCommand{widget.id, widget.bounds, widget.clipRect, trackColor, 3.0F});

        const float normalized =
            (slider.maxValue > slider.minValue)
            ? (sliderValue - slider.minValue) / (slider.maxValue - slider.minValue)
                : 0.0F;
        const float clamped = std::clamp(normalized, 0.0F, 1.0F);
        const float knobWidth = std::min(12.0F, widget.bounds.width);
        const float knobX = widget.bounds.x + (widget.bounds.width - knobWidth) * clamped;

        appendCommandForWidget(widget, UiRectCommand{widget.id, Rect{knobX, widget.bounds.y, knobWidth, widget.bounds.height}, widget.clipRect, knobColor, 6.0F});
      } break;
      case WidgetKind::kTextInput: {
        auto& input = std::get<TextInputElement>(widget.payload);
        const bool isEnabled = input.enabled;
        const bool isFocused = isEnabled && widget.id == focusedWidgetId_;
        const bool isHovered = isEnabled && widget.id == hoveredWidgetId_;

        std::string value;
        if (input.value != nullptr) {
          value = *input.value;
        }

        if (isFocused) {
          focusedTextInputVisible = true;
          if (editingTextInputId_ != widget.id) {
            editingTextInputId_ = widget.id;
            textInputCursorByte_ = value.size();
          }

          if (input.value != nullptr) {
            applyTextInputEvents(keyInputEvents_, textInputCodepoints_, input.value, input.maxLength, textInputCursorByte_);
            value = *input.value;
          }
        }

        Color borderColor = input.borderColor;
        Color background = input.background;
        Color textColor = input.textColor;
        if (!isEnabled) {
          borderColor = mixColor(input.borderColor, theme.panelBackground, 0.55F);
          background = mixColor(input.background, theme.panelBackground, 0.45F);
          textColor = mixColor(input.textColor, theme.textMuted, 0.65F);
        } else if (isFocused) {
          borderColor = mixColor(input.borderColor, theme.buttonHover, 0.45F);
        } else if (isHovered) {
          borderColor = mixColor(input.borderColor, theme.textPrimary, 0.18F);
        }

        appendCommandForWidget(widget, UiRectCommand{widget.id, widget.bounds, widget.clipRect, borderColor, 6.0F});
        const Rect innerBounds{
            widget.bounds.x + 1.5F,
            widget.bounds.y + 1.5F,
            std::max(0.0F, widget.bounds.width - 3.0F),
            std::max(0.0F, widget.bounds.height - 3.0F),
        };
        appendCommandForWidget(widget, UiRectCommand{widget.id, innerBounds, widget.clipRect, background, 5.0F});

        std::string displayText = value;
        Color displayColor = textColor;
        if (displayText.empty()) {
          if (isFocused) {
            displayText = "|";
            displayColor = input.textColor;
          } else {
            displayText = input.placeholder;
            displayColor = input.placeholderColor;
          }
        } else if (isFocused) {
          displayText = injectCaretMarker(displayText, textInputCursorByte_);
        }

        const Rect textBounds{
            innerBounds.x + 8.0F,
            innerBounds.y,
            std::max(0.0F, innerBounds.width - 16.0F),
            innerBounds.height,
        };
        appendCommandForWidget(widget, makeTextCommand(widget.id, textBounds, widget.clipRect, displayText, "default", 13.0F, displayColor));
      } break;
      case WidgetKind::kIcon: {
        const auto& icon = std::get<IconElement>(widget.payload);
        appendCommandForWidget(widget, UiIconCommand{widget.id, widget.bounds, widget.clipRect, icon.iconKey, icon.tint});
      } break;
      case WidgetKind::kImage: {
        const auto& image = std::get<ImageElement>(widget.payload);
        appendCommandForWidget(widget, UiImageCommand{widget.id, widget.bounds, widget.clipRect, image.imageKey, image.tint});
      } break;
      case WidgetKind::kPanel: {
        const auto& panel = std::get<PanelElement>(widget.payload);
        appendCommandForWidget(widget, UiRectCommand{widget.id, widget.bounds, widget.clipRect, panel.background, panel.cornerRadiusPx});
      } break;
      case WidgetKind::kChartScaffold: {
        const auto& chart = std::get<ChartScaffoldElement>(widget.payload);
        appendCommandForWidget(widget, UiRectCommand{widget.id, widget.bounds, widget.clipRect, theme.panelBackground, 8.0F});
        appendCommandForWidget(widget, UiChartScaffoldCommand{widget.id, widget.bounds, widget.clipRect, chart.chartKind});
      } break;
      case WidgetKind::kSchematicScaffold: {
        const auto& schematic = std::get<SchematicScaffoldElement>(widget.payload);
        appendCommandForWidget(widget, UiRectCommand{widget.id, widget.bounds, widget.clipRect, theme.panelBackground, 8.0F});
        appendCommandForWidget(widget, UiSchematicScaffoldCommand{widget.id, widget.bounds, widget.clipRect, schematic.modelName});
      } break;
      default:
        break;
    }
  }

  if (!focusedTextInputVisible) {
    editingTextInputId_ = 0;
    textInputCursorByte_ = 0;
  }

  std::vector<UiRetainedPanel> retainedPanels;
  retainedPanels.reserve(pendingRetainedPanels.size());

  drawCommands_.clear();
  drawCommands_.reserve(widgets_.size() * 4U);

  for (const OutputEntry& entry : outputSequence) {
    if (entry.retainedPanelKey.empty()) {
      drawCommands_.insert(drawCommands_.end(), entry.commands.begin(), entry.commands.end());
      continue;
    }

    const auto pendingIt = pendingRetainedPanels.find(entry.retainedPanelKey);
    if (pendingIt == pendingRetainedPanels.end() || pendingIt->second.widget == nullptr) {
      continue;
    }

    WidgetNode& panelWidget = *pendingIt->second.widget;
    const std::vector<UiRenderCommand>& panelCommands = pendingIt->second.commands;
    RetainedPanelState& retainedState = retainedPanelStates_[entry.retainedPanelKey];
    const bool geometryChanged =
        retainedState.bounds.x != panelWidget.bounds.x ||
        retainedState.bounds.y != panelWidget.bounds.y ||
        retainedState.bounds.width != panelWidget.bounds.width ||
        retainedState.bounds.height != panelWidget.bounds.height ||
        retainedState.clipRect.x != panelWidget.clipRect.x ||
        retainedState.clipRect.y != panelWidget.clipRect.y ||
        retainedState.clipRect.width != panelWidget.clipRect.width ||
        retainedState.clipRect.height != panelWidget.clipRect.height;

    retainedState.widgetId = panelWidget.id;
    retainedState.bounds = panelWidget.bounds;
    retainedState.clipRect = panelWidget.clipRect;
    retainedState.cacheKey = entry.retainedPanelKey;
    retainedState.textureKey = "rt-panel:" + entry.retainedPanelKey;

    const std::uint64_t signature = hashUiRenderCommands(panelCommands, panelWidget.bounds, panelWidget.clipRect);
    const bool needsRefresh = panelWidget.cacheDirtyHint || !retainedState.ready || geometryChanged || retainedState.signature != signature;

    if (!needsRefresh && retainedState.ready) {
      drawCommands_.push_back(UiImageCommand{
          panelWidget.id,
          panelWidget.bounds,
          panelWidget.clipRect,
          retainedState.textureKey,
          {1.0F, 1.0F, 1.0F, 1.0F},
      });
      continue;
    }

    if (panelWidget.cacheAllowImmediateFallback) {
      drawCommands_.insert(drawCommands_.end(), panelCommands.begin(), panelCommands.end());
    }

    std::vector<UiRenderCommand> localizedCommands;
    localizedCommands.reserve(panelCommands.size());
    for (const UiRenderCommand& command : panelCommands) {
      localizedCommands.push_back(localizeUiRenderCommand(command, panelWidget.bounds));
    }

    UiMeshData retainedMesh = buildUiMesh(localizedCommands);
    if (!retainedMesh.vertices.empty() && !retainedMesh.indices.empty() && !retainedMesh.batches.empty()) {
      retainedPanels.push_back(UiRetainedPanel{
          panelWidget.id,
          panelWidget.bounds,
          panelWidget.clipRect,
          retainedState.textureKey,
          signature,
          std::move(retainedMesh.vertices),
          std::move(retainedMesh.indices),
          std::move(retainedMesh.batches),
      });
      retainedState.signature = signature;
      retainedState.ready = true;
    }

    if (panelWidget.kind == WidgetKind::kPanel) {
      if (auto* panel = std::get_if<PanelElement>(&panelWidget.payload); panel != nullptr && panel->cacheDirtyBinding != nullptr) {
        *panel->cacheDirtyBinding = false;
      }
    }
  }

  packet_.drawListCount = static_cast<std::uint32_t>(drawCommands_.size());
  packet_.clipRectCount = static_cast<std::uint32_t>(widgets_.size());
  packet_.widgetCount = static_cast<std::uint32_t>(widgets_.size());

  meshData_ = buildUiMesh(drawCommands_, UiMeshBuildOptions{true});
  meshData_.retainedPanels = std::move(retainedPanels);
}

void UILayer::endFrame() {
  VOLT_LOG_TRACE_CAT(
      volt::core::logging::Category::kUI,
      "UI end frame ",
      frameIndex_,
      " inputEvents=",
      consumedInputEventCount_,
      " renderEvents=",
      consumedRenderEventCount_);

  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kUiFrameEnd,
        .payload = volt::event::UiStageEvent{
            .stage = volt::event::UiStage::kFrameEnd,
            .frameIndex = frameIndex_,
        },
    });
  }

  // Transient UI state reset will be added when widgets and command buffers are implemented.
  consumedInputEventCount_ = 0;
  consumedRenderEventCount_ = 0;
  leftMousePressed_ = false;
  leftMouseReleased_ = false;
  keyInputEvents_.clear();
  textInputCodepoints_.clear();
}

void UILayer::recordRenderPass() {
  // Event-side notification hook remains for diagnostics while explicit Vulkan callback drives recording.
  ++consumedRenderEventCount_;
  VOLT_LOG_TRACE_CAT(
      volt::core::logging::Category::kUI,
      "UI render pass hook frame=",
      frameIndex_,
      " count=",
      consumedRenderEventCount_);
}

void UILayer::recordRenderPass(
    VkCommandBuffer commandBuffer,
    std::uint32_t framebufferWidth,
    std::uint32_t framebufferHeight) {
  ++consumedRenderEventCount_;

  std::uint32_t textCount = 0;
  std::uint32_t rectCount = 0;
  std::uint32_t imageCount = 0;
  std::uint32_t iconCount = 0;
  std::uint32_t chartCount = 0;
  std::uint32_t schematicCount = 0;

  for (const UiRenderCommand& command : drawCommands_) {
    std::visit(
        [&](const auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, UiTextCommand>) {
            ++textCount;
          } else if constexpr (std::is_same_v<T, UiRectCommand>) {
            ++rectCount;
          } else if constexpr (std::is_same_v<T, UiImageCommand>) {
            ++imageCount;
          } else if constexpr (std::is_same_v<T, UiIconCommand>) {
            ++iconCount;
          } else if constexpr (std::is_same_v<T, UiChartScaffoldCommand>) {
            ++chartCount;
          } else if constexpr (std::is_same_v<T, UiSchematicScaffoldCommand>) {
            ++schematicCount;
          }
        },
        command);
  }

  VOLT_LOG_TRACE_CAT(
      volt::core::logging::Category::kUI,
      "UI Vulkan pass frame=",
      frameIndex_,
      " commands=",
      drawCommands_.size(),
      " [text=",
      textCount,
      " rect=",
      rectCount,
      " image=",
      imageCount,
      " icon=",
      iconCount,
      " chart=",
      chartCount,
      " schematic=",
      schematicCount,
      "]",
      " mesh[v=",
      meshData_.vertices.size(),
      " i=",
      meshData_.indices.size(),
      " b=",
      meshData_.batches.size(),
      "]",
      " framebuffer=",
      framebufferWidth,
      "x",
      framebufferHeight);

  (void)commandBuffer;
}

std::uint64_t UILayer::addText(const Rect& bounds, const TextElement& text) {
  return addWidgetNode(WidgetKind::kText, bounds, text);
}

std::uint64_t UILayer::addButton(const Rect& bounds, const ButtonElement& button) {
  return addWidgetNode(WidgetKind::kButton, bounds, button);
}

std::uint64_t UILayer::addCheckbox(const Rect& bounds, const CheckboxElement& checkbox) {
  return addWidgetNode(WidgetKind::kCheckbox, bounds, checkbox);
}

std::uint64_t UILayer::addToggle(const Rect& bounds, const ToggleElement& toggle) {
  return addWidgetNode(WidgetKind::kToggle, bounds, toggle);
}

std::uint64_t UILayer::addSlider(const Rect& bounds, const SliderElement& slider) {
  return addWidgetNode(WidgetKind::kSlider, bounds, slider);
}

std::uint64_t UILayer::addTextInput(const Rect& bounds, const TextInputElement& input) {
  return addWidgetNode(WidgetKind::kTextInput, bounds, input);
}

std::uint64_t UILayer::addIcon(const Rect& bounds, const IconElement& icon) {
  return addWidgetNode(WidgetKind::kIcon, bounds, icon);
}

std::uint64_t UILayer::addImage(const Rect& bounds, const ImageElement& image) {
  return addWidgetNode(WidgetKind::kImage, bounds, image);
}

std::uint64_t UILayer::addPanel(const Rect& bounds, const PanelElement& panel) {
  return addWidgetNode(WidgetKind::kPanel, bounds, panel);
}

std::uint64_t UILayer::addChartScaffold(const Rect& bounds, const ChartScaffoldElement& chart) {
  return addWidgetNode(WidgetKind::kChartScaffold, bounds, chart);
}

std::uint64_t UILayer::addSchematicScaffold(
    const Rect& bounds,
    const SchematicScaffoldElement& schematic) {
  return addWidgetNode(WidgetKind::kSchematicScaffold, bounds, schematic);
}

void UILayer::beginPanel(const Rect& bounds, const PanelElement& panel) {
  const std::uint64_t id = addPanel(bounds, panel);
  if (!widgets_.empty() && widgets_.back().id == id) {
    panelStack_.push_back(PanelContext{widgets_.back().clipRect, 0.0F, widgets_.back().renderTargetOwnerKey});
  }
}

void UILayer::endPanel() {
  if (!panelStack_.empty()) {
    panelStack_.pop_back();
  }
}

void UILayer::beginScrollPanel(const Rect& bounds,
                               const PanelElement& panel,
                               float contentHeight,
                               float& scrollOffsetY) {
  const std::uint64_t id = addPanel(bounds, panel);
  if (widgets_.empty() || widgets_.back().id != id) {
    return;
  }

  const Rect clipRect = widgets_.back().clipRect;
  const float visibleHeight = std::max(0.0F, clipRect.height);
  const float maxScrollOffset = std::max(0.0F, contentHeight - visibleHeight);
  scrollOffsetY = std::clamp(scrollOffsetY, 0.0F, maxScrollOffset);

  if (maxScrollOffset > 0.0F && pointInRect(lastMouseX_, lastMouseY_, clipRect) && std::abs(scrollDeltaY_) > 0.001) {
    scrollOffsetY = std::clamp(scrollOffsetY - static_cast<float>(scrollDeltaY_) * 24.0F, 0.0F, maxScrollOffset);
  }

  panelStack_.push_back(PanelContext{clipRect, scrollOffsetY, widgets_.back().renderTargetOwnerKey});
}

void UILayer::endScrollPanel() {
  endPanel();
}

void UILayer::beginFlowColumn(const Rect& bounds, float spacing, float padding) {
  const Rect translatedBounds = clipRectToPanelStack(bounds, panelStack_);
  flowColumns_.push_back(FlowColumn{
      translatedBounds,
      translatedBounds.y + std::max(0.0F, padding),
      std::max(0.0F, spacing),
      std::max(0.0F, padding),
  });
}

void UILayer::endFlowColumn() {
  if (!flowColumns_.empty()) {
    flowColumns_.pop_back();
  }
}

void UILayer::beginFlowRow(const Rect& bounds, float rowHeight, float spacing, float padding) {
  const Rect translatedBounds = clipRectToPanelStack(bounds, panelStack_);
  flowRows_.push_back(FlowRow{
      translatedBounds,
      translatedBounds.x + std::max(0.0F, padding),
      std::max(0.0F, spacing),
      std::max(0.0F, padding),
      std::max(0.0F, rowHeight),
  });
}

void UILayer::endFlowRow() {
  if (!flowRows_.empty()) {
    flowRows_.pop_back();
  }
}

void UILayer::beginStack(const Rect& bounds, StackAxis axis, float spacing, float padding) {
  const Rect translatedBounds = clipRectToPanelStack(bounds, panelStack_);
  stackLayouts_.push_back(StackLayout{
      translatedBounds,
      axis,
      translatedBounds.x + std::max(0.0F, padding),
      translatedBounds.y + std::max(0.0F, padding),
      std::max(0.0F, spacing),
      std::max(0.0F, padding),
  });
}

void UILayer::endStack() {
  if (!stackLayouts_.empty()) {
    stackLayouts_.pop_back();
  }
}

Rect UILayer::nextStackRect(float extent) {
  return allocateStackRect(extent);
}

void UILayer::beginDock(const Rect& bounds, float spacing, float padding) {
  const Rect clippedBounds = clipRectToPanelStack(bounds, panelStack_);
  dockLayouts_.push_back(DockLayout{insetRect(clippedBounds, padding), std::max(0.0F, spacing)});
}

void UILayer::endDock() {
  if (!dockLayouts_.empty()) {
    dockLayouts_.pop_back();
  }
}

Rect UILayer::dockTopRect(float height) {
  if (dockLayouts_.empty()) {
    return {0.0F, 0.0F, 0.0F, std::max(0.0F, height)};
  }

  DockLayout& dock = dockLayouts_.back();
  const float finalHeight = std::min(std::max(0.0F, height), dock.remainingBounds.height);
  const Rect rect{dock.remainingBounds.x, dock.remainingBounds.y, dock.remainingBounds.width, finalHeight};
  const float consumedHeight = std::min(dock.remainingBounds.height, finalHeight + dock.spacing);
  dock.remainingBounds.y += consumedHeight;
  dock.remainingBounds.height = std::max(0.0F, dock.remainingBounds.height - consumedHeight);
  return rect;
}

Rect UILayer::dockBottomRect(float height) {
  if (dockLayouts_.empty()) {
    return {0.0F, 0.0F, 0.0F, std::max(0.0F, height)};
  }

  DockLayout& dock = dockLayouts_.back();
  const float finalHeight = std::min(std::max(0.0F, height), dock.remainingBounds.height);
  const Rect rect{
      dock.remainingBounds.x,
      dock.remainingBounds.y + std::max(0.0F, dock.remainingBounds.height - finalHeight),
      dock.remainingBounds.width,
      finalHeight,
  };
  dock.remainingBounds.height = std::max(0.0F, dock.remainingBounds.height - finalHeight - dock.spacing);
  return rect;
}

Rect UILayer::dockLeftRect(float width) {
  if (dockLayouts_.empty()) {
    return {0.0F, 0.0F, std::max(0.0F, width), 0.0F};
  }

  DockLayout& dock = dockLayouts_.back();
  const float finalWidth = std::min(std::max(0.0F, width), dock.remainingBounds.width);
  const Rect rect{dock.remainingBounds.x, dock.remainingBounds.y, finalWidth, dock.remainingBounds.height};
  const float consumedWidth = std::min(dock.remainingBounds.width, finalWidth + dock.spacing);
  dock.remainingBounds.x += consumedWidth;
  dock.remainingBounds.width = std::max(0.0F, dock.remainingBounds.width - consumedWidth);
  return rect;
}

Rect UILayer::dockRightRect(float width) {
  if (dockLayouts_.empty()) {
    return {0.0F, 0.0F, std::max(0.0F, width), 0.0F};
  }

  DockLayout& dock = dockLayouts_.back();
  const float finalWidth = std::min(std::max(0.0F, width), dock.remainingBounds.width);
  const Rect rect{
      dock.remainingBounds.x + std::max(0.0F, dock.remainingBounds.width - finalWidth),
      dock.remainingBounds.y,
      finalWidth,
      dock.remainingBounds.height,
  };
  dock.remainingBounds.width = std::max(0.0F, dock.remainingBounds.width - finalWidth - dock.spacing);
  return rect;
}

Rect UILayer::dockFillRect() {
  if (dockLayouts_.empty()) {
    return {};
  }

  DockLayout& dock = dockLayouts_.back();
  const Rect rect = dock.remainingBounds;
  dock.remainingBounds = {};
  return rect;
}

void UILayer::beginGrid(
    const Rect& bounds,
    std::uint32_t columns,
    float rowHeight,
    float columnSpacing,
    float rowSpacing,
    float padding) {
  const Rect clippedBounds = clipRectToPanelStack(bounds, panelStack_);
  const Rect paddedBounds = insetRect(clippedBounds, padding);
  const std::uint32_t resolvedColumns = std::max<std::uint32_t>(1U, columns);
  const float totalColumnSpacing = std::max(0.0F, columnSpacing) * static_cast<float>(resolvedColumns - 1U);
  const float columnWidth =
      resolvedColumns > 0U ? std::max(0.0F, (paddedBounds.width - totalColumnSpacing) / static_cast<float>(resolvedColumns)) : 0.0F;
  gridLayouts_.push_back(GridLayout{
      paddedBounds,
      resolvedColumns,
      0U,
      paddedBounds.y,
      std::max(0.0F, rowHeight),
      0.0F,
      columnWidth,
      std::max(0.0F, columnSpacing),
      std::max(0.0F, rowSpacing),
      std::max(0.0F, padding),
  });
}

void UILayer::endGrid() {
  if (!gridLayouts_.empty()) {
    gridLayouts_.pop_back();
  }
}

Rect UILayer::nextGridRect(std::uint32_t columnSpan, float height) {
  if (gridLayouts_.empty()) {
    return {0.0F, 0.0F, 0.0F, std::max(0.0F, height)};
  }

  GridLayout& grid = gridLayouts_.back();
  const std::uint32_t span = std::min(std::max<std::uint32_t>(1U, columnSpan), grid.columns);
  if (grid.currentColumn > 0U && grid.currentColumn + span > grid.columns) {
    grid.cursorY += grid.currentRowHeight + grid.rowSpacing;
    grid.currentColumn = 0U;
    grid.currentRowHeight = 0.0F;
  }

  const float resolvedHeight = height >= 0.0F ? height : grid.rowHeight;
  const float finalHeight = std::max(0.0F, resolvedHeight);
  const float x = grid.bounds.x + static_cast<float>(grid.currentColumn) * (grid.columnWidth + grid.columnSpacing);
  const float width =
      (grid.columnWidth * static_cast<float>(span)) + (grid.columnSpacing * static_cast<float>(span - 1U));
  const Rect rect{x, grid.cursorY, width, finalHeight};

  grid.currentRowHeight = std::max(grid.currentRowHeight, finalHeight);
  grid.currentColumn += span;
  return rect;
}

std::uint64_t UILayer::addTextFlow(float height, const TextElement& text) {
  return addText(allocateFlowRect(height), text);
}

std::uint64_t UILayer::addButtonFlow(float height, const ButtonElement& button) {
  return addButton(allocateFlowRect(height), button);
}

std::uint64_t UILayer::addCheckboxFlow(float height, const CheckboxElement& checkbox) {
  return addCheckbox(allocateFlowRect(height), checkbox);
}

std::uint64_t UILayer::addToggleFlow(float height, const ToggleElement& toggle) {
  return addToggle(allocateFlowRect(height), toggle);
}

std::uint64_t UILayer::addSliderFlow(float height, const SliderElement& slider) {
  return addSlider(allocateFlowRect(height), slider);
}

std::uint64_t UILayer::addTextInputFlow(float height, const TextInputElement& input) {
  return addTextInput(allocateFlowRect(height), input);
}

std::uint64_t UILayer::addIconFlow(float height, const IconElement& icon) {
  return addIcon(allocateFlowRect(height), icon);
}

std::uint64_t UILayer::addImageFlow(float height, const ImageElement& image) {
  if (flowColumns_.empty()) {
    const float clampedHeight = std::max(0.0F, height);
    const float aspectRatio = cachedImageAspectRatio(image.imageKey);
    return addImage({0.0F, 0.0F, clampedHeight * aspectRatio, clampedHeight}, image);
  }

  FlowColumn& column = flowColumns_.back();
  return addImage(allocateFlowImageRect(column, height, cachedImageAspectRatio(image.imageKey)), image);
}

std::uint64_t UILayer::addTextRow(float width, const TextElement& text) {
  return addText(allocateFlowRowRect(width), text);
}

std::uint64_t UILayer::addButtonRow(float width, const ButtonElement& button) {
  return addButton(allocateFlowRowRect(width), button);
}

std::uint64_t UILayer::addCheckboxRow(float width, const CheckboxElement& checkbox) {
  return addCheckbox(allocateFlowRowRect(width), checkbox);
}

std::uint64_t UILayer::addToggleRow(float width, const ToggleElement& toggle) {
  return addToggle(allocateFlowRowRect(width), toggle);
}

std::uint64_t UILayer::addSliderRow(float width, const SliderElement& slider) {
  return addSlider(allocateFlowRowRect(width), slider);
}

std::uint64_t UILayer::addTextInputRow(float width, const TextInputElement& input) {
  return addTextInput(allocateFlowRowRect(width), input);
}

std::uint64_t UILayer::addIconRow(float width, const IconElement& icon) {
  return addIcon(allocateFlowRowRect(width), icon);
}

std::uint64_t UILayer::addImageRow(float width, const ImageElement& image) {
  return addImage(allocateFlowRowRect(width), image);
}

const std::vector<UiRenderCommand>& UILayer::renderCommands() const {
  return drawCommands_;
}

std::uint64_t UILayer::addWidgetNode(
    WidgetKind kind,
    const Rect& bounds,
  const WidgetPayload& payload) {
  const Rect translatedBounds = translateRectForPanelStack(bounds, panelStack_);
  const Rect clipRect = currentClipRectForPanelStack(frameArgs_, panelStack_);
  const Rect finalBounds = clipRectToPanelStack(translatedBounds, panelStack_);
  const Rect widgetClipRect = kind == WidgetKind::kPanel ? finalBounds : clipRect;
  const std::string inheritedRenderTargetOwnerKey = panelStack_.empty() ? std::string{} : panelStack_.back().renderTargetOwnerKey;
  std::string renderTargetOwnerKey = inheritedRenderTargetOwnerKey;
  std::string panelCacheKey{};
  bool cacheRenderTarget = false;
  bool cacheAllowImmediateFallback = true;
  bool cacheDirtyHint = false;

  if (kind == WidgetKind::kPanel) {
    if (const auto* panel = std::get_if<PanelElement>(&payload);
        panel != nullptr && inheritedRenderTargetOwnerKey.empty() && panel->cacheRenderTarget) {
      const std::uint32_t occurrence = panelNameCounts_[panel->panelName]++;
      panelCacheKey = panel->panelName + "#" + std::to_string(occurrence);
      renderTargetOwnerKey = panelCacheKey;
      cacheRenderTarget = true;
      cacheAllowImmediateFallback = panel->cacheAllowImmediateFallback;
      cacheDirtyHint = panel->cacheDirty || (panel->cacheDirtyBinding != nullptr && *panel->cacheDirtyBinding);
    }
  }

  const std::uint64_t id = nextWidgetId_++;
  widgets_.push_back(WidgetNode{
      id,
      kind,
      finalBounds,
      widgetClipRect,
      renderTargetOwnerKey,
      panelCacheKey,
      cacheRenderTarget,
      cacheAllowImmediateFallback,
      cacheDirtyHint,
      payload,
  });
  return id;
}

UILayer::RenderPacket UILayer::currentRenderPacket() const {
  return packet_;
}

std::uint64_t UILayer::focusedWidgetId() const {
  return focusedWidgetId_;
}

std::uint64_t UILayer::hoveredWidgetId() const {
  return hoveredWidgetId_;
}

const std::vector<TextRun>& UILayer::textRuns() const {
  return textRuns_;
}

const UiMeshData& UILayer::meshData() const {
  return meshData_;
}

const UIResourceRegistry& UILayer::resources() const {
  return resources_;
}

UIResourceRegistry& UILayer::resources() {
  return resources_;
}

Rect UILayer::allocateFlowRect(float height) {
  if (flowColumns_.empty()) {
    return {0.0F, 0.0F, 0.0F, std::max(0.0F, height)};
  }

  FlowColumn& column = flowColumns_.back();
  const float clampedHeight = std::max(0.0F, height);
  const float x = column.bounds.x + column.padding;
  const float width = std::max(0.0F, column.bounds.width - (column.padding * 2.0F));
  const float y = column.cursorY;
  column.cursorY += clampedHeight + column.spacing;
  return {x, y, width, clampedHeight};
}

Rect UILayer::allocateFlowRowRect(float width) {
  if (flowRows_.empty()) {
    return {0.0F, 0.0F, std::max(0.0F, width), 0.0F};
  }

  FlowRow& row = flowRows_.back();
  const float clampedWidth = std::max(0.0F, width);
  const float x = row.cursorX;
  const float y = row.bounds.y + row.padding;
  const float maxWidth = row.bounds.width - row.padding;
  const float allowedWidth = std::max(0.0F, maxWidth - (x - row.bounds.x));
  const float finalWidth = std::min(clampedWidth, allowedWidth);
  row.cursorX += finalWidth + row.spacing;
  return {x, y, finalWidth, row.rowHeight};
}

Rect UILayer::allocateStackRect(float extent) {
  if (stackLayouts_.empty()) {
    return {0.0F, 0.0F, 0.0F, std::max(0.0F, extent)};
  }

  StackLayout& stack = stackLayouts_.back();
  const float clampedExtent = std::max(0.0F, extent);
  if (stack.axis == StackAxis::kVertical) {
    const float x = stack.bounds.x + stack.padding;
    const float width = std::max(0.0F, stack.bounds.width - (stack.padding * 2.0F));
    const float y = stack.cursorY;
    stack.cursorY += clampedExtent + stack.spacing;
    return {x, y, width, clampedExtent};
  }

  const float x = stack.cursorX;
  const float y = stack.bounds.y + stack.padding;
  const float height = std::max(0.0F, stack.bounds.height - (stack.padding * 2.0F));
  stack.cursorX += clampedExtent + stack.spacing;
  return {x, y, clampedExtent, height};
}

}  // namespace volt::ui
