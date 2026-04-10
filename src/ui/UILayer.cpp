#include "volt/ui/UILayer.hpp"

#include "volt/core/Logging.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"
#include "volt/ui/UIMesh.hpp"

#include <algorithm>
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

}  // namespace

void UILayer::beginFrame(const volt::platform::InputState& input, const FrameArgs& frameArgs) {
  ++frameIndex_;
  frameArgs_ = frameArgs;
  widgets_.clear();
  drawCommands_.clear();
  panelStack_.clear();
  flowColumns_.clear();
  flowRows_.clear();
  textRuns_.clear();
  meshData_ = UiMeshData{};
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
  leftMousePressed_ = false;
  leftMouseReleased_ = false;
  VOLT_LOG_TRACE_CAT(
      volt::core::logging::Category::kUI,
      "UI begin frame ",
      frameIndex_,
      " size=",
      frameArgs_.framebufferWidth,
      "x",
      frameArgs_.framebufferHeight,
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

  if (leftMouseDown_) {
    leftMouseDown_ = input.mouse.down[0];
  }
}

void UILayer::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  if (eventDispatcher_ != nullptr) {
    if (keyListenerId_ != 0) {
      eventDispatcher_->unsubscribe(keyListenerId_);
      keyListenerId_ = 0;
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
            ++consumedInputEventCount_;
            VOLT_LOG_TRACE_CAT(
                volt::core::logging::Category::kUI,
                "UI consumed key input event count=",
                consumedInputEventCount_);
          }
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

          if (payload->action == 1) {
            leftMouseDown_ = true;
            leftMousePressed_ = true;
          } else if (payload->action == 0) {
            leftMouseDown_ = false;
            leftMouseReleased_ = true;
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
  drawCommands_.reserve(widgets_.size() * 2U);

  const ThemeTokens& theme = styleSheet_.theme();

  for (WidgetNode& widget : widgets_) {
    switch (widget.kind) {
      case WidgetKind::kText: {
        const auto& text = std::get<TextElement>(widget.payload);
        drawCommands_.push_back(UiTextCommand{
            widget.id,
            widget.bounds,
            text.text,
            text.fontFamily,
            text.fontSizePx,
          0,
            text.color,
        });
        textRuns_.push_back(buildTextRun(text.text, text.fontFamily, text.fontSizePx));
        auto& command = std::get<UiTextCommand>(drawCommands_.back());
        command.glyphCount = static_cast<std::uint32_t>(textRuns_.back().glyphs.size());
      } break;
      case WidgetKind::kButton: {
        auto& button = std::get<ButtonElement>(widget.payload);
        const bool isHovered = widget.id == hoveredWidgetId_;
        const bool isActive = widget.id == activeWidgetId_;
        const bool clicked = leftMouseReleased_ && isHovered && focusedWidgetId_ == widget.id;
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
        if (isActive) {
          bg = theme.buttonPressed;
        } else if (isHovered) {
          bg = theme.buttonHover;
        }

        drawCommands_.push_back(UiRectCommand{
            widget.id,
            widget.bounds,
            bg,
            6.0F,
        });
        drawCommands_.push_back(UiTextCommand{
            widget.id,
            widget.bounds,
            button.label,
            "default",
            14.0F,
          0,
            theme.textPrimary,
        });
        textRuns_.push_back(buildTextRun(button.label, "default", 14.0F));
        auto& command = std::get<UiTextCommand>(drawCommands_.back());
        command.glyphCount = static_cast<std::uint32_t>(textRuns_.back().glyphs.size());
      } break;
      case WidgetKind::kSlider: {
        auto& slider = std::get<SliderElement>(widget.payload);
        const bool isHovered = widget.id == hoveredWidgetId_;
        const bool isActive = widget.id == activeWidgetId_;
        const bool canDrag = isActive || (isHovered && leftMousePressed_);

        if (canDrag && leftMouseDown_ && slider.maxValue > slider.minValue && widget.bounds.width > 0.0F) {
          const float normalized =
              std::clamp(static_cast<float>((lastMouseX_ - widget.bounds.x) / widget.bounds.width), 0.0F, 1.0F);
          slider.value = slider.minValue + normalized * (slider.maxValue - slider.minValue);
        }

        drawCommands_.push_back(UiRectCommand{
            widget.id,
            widget.bounds,
            theme.sliderTrack,
            3.0F,
        });

        const float normalized =
            (slider.maxValue > slider.minValue)
                ? (slider.value - slider.minValue) / (slider.maxValue - slider.minValue)
                : 0.0F;
        const float clamped = std::clamp(normalized, 0.0F, 1.0F);
        const float knobWidth = std::min(12.0F, widget.bounds.width);
        const float knobX = widget.bounds.x + (widget.bounds.width - knobWidth) * clamped;

        drawCommands_.push_back(UiRectCommand{
            widget.id,
            Rect{knobX, widget.bounds.y, knobWidth, widget.bounds.height},
          theme.sliderKnob,
            6.0F,
        });
      } break;
      case WidgetKind::kIcon: {
        const auto& icon = std::get<IconElement>(widget.payload);
        drawCommands_.push_back(UiIconCommand{widget.id, widget.bounds, icon.iconKey, icon.tint});
      } break;
      case WidgetKind::kImage: {
        const auto& image = std::get<ImageElement>(widget.payload);
        drawCommands_.push_back(UiImageCommand{widget.id, widget.bounds, image.imageKey, image.tint});
      } break;
      case WidgetKind::kPanel: {
        const auto& panel = std::get<PanelElement>(widget.payload);
        drawCommands_.push_back(UiRectCommand{
            widget.id,
            widget.bounds,
            panel.background,
            panel.cornerRadiusPx,
        });
      } break;
      case WidgetKind::kChartScaffold: {
        const auto& chart = std::get<ChartScaffoldElement>(widget.payload);
        drawCommands_.push_back(UiRectCommand{
            widget.id,
            widget.bounds,
            theme.panelBackground,
            8.0F,
        });
        drawCommands_.push_back(UiChartScaffoldCommand{widget.id, widget.bounds, chart.chartKind});
      } break;
      case WidgetKind::kSchematicScaffold: {
        const auto& schematic = std::get<SchematicScaffoldElement>(widget.payload);
        drawCommands_.push_back(UiRectCommand{
            widget.id,
            widget.bounds,
            theme.panelBackground,
            8.0F,
        });
        drawCommands_.push_back(UiSchematicScaffoldCommand{widget.id, widget.bounds, schematic.modelName});
      } break;
      default:
        break;
    }
  }

  packet_.drawListCount = static_cast<std::uint32_t>(drawCommands_.size());
  packet_.clipRectCount = static_cast<std::uint32_t>(widgets_.size());
  packet_.widgetCount = static_cast<std::uint32_t>(widgets_.size());

  meshData_ = buildUiMesh(drawCommands_);
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

std::uint64_t UILayer::addSlider(const Rect& bounds, const SliderElement& slider) {
  return addWidgetNode(WidgetKind::kSlider, bounds, slider);
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
    panelStack_.push_back(widgets_.back().bounds);
  }
}

void UILayer::endPanel() {
  if (!panelStack_.empty()) {
    panelStack_.pop_back();
  }
}

void UILayer::beginFlowColumn(const Rect& bounds, float spacing, float padding) {
  flowColumns_.push_back(FlowColumn{
      bounds,
      bounds.y + padding,
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
  flowRows_.push_back(FlowRow{
      bounds,
      bounds.x + padding,
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

std::uint64_t UILayer::addTextFlow(float height, const TextElement& text) {
  return addText(allocateFlowRect(height), text);
}

std::uint64_t UILayer::addButtonFlow(float height, const ButtonElement& button) {
  return addButton(allocateFlowRect(height), button);
}

std::uint64_t UILayer::addSliderFlow(float height, const SliderElement& slider) {
  return addSlider(allocateFlowRect(height), slider);
}

std::uint64_t UILayer::addIconFlow(float height, const IconElement& icon) {
  return addIcon(allocateFlowRect(height), icon);
}

std::uint64_t UILayer::addImageFlow(float height, const ImageElement& image) {
  return addImage(allocateFlowRect(height), image);
}

std::uint64_t UILayer::addTextRow(float width, const TextElement& text) {
  return addText(allocateFlowRowRect(width), text);
}

std::uint64_t UILayer::addButtonRow(float width, const ButtonElement& button) {
  return addButton(allocateFlowRowRect(width), button);
}

std::uint64_t UILayer::addSliderRow(float width, const SliderElement& slider) {
  return addSlider(allocateFlowRowRect(width), slider);
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
  Rect finalBounds = bounds;
  if (!panelStack_.empty()) {
    finalBounds = intersectRect(finalBounds, panelStack_.back());
  }

  const std::uint64_t id = nextWidgetId_++;
  widgets_.push_back(WidgetNode{
      id,
      kind,
      finalBounds,
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

}  // namespace volt::ui
