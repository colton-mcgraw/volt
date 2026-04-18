#pragma once

#include "volt/event/Event.hpp"
#include "volt/platform/InputState.hpp"
#include "volt/ui/UIRenderTypes.hpp"
#include "volt/ui/UIResources.hpp"
#include "volt/ui/UIMesh.hpp"
#include "volt/ui/UIStyle.hpp"
#include "volt/ui/UIText.hpp"
#include "volt/ui/Widgets.hpp"

#include <cstdint>
#include <unordered_map>
#include <string>
#include <variant>
#include <vector>

struct VkCommandBuffer_T;
using VkCommandBuffer = VkCommandBuffer_T*;

namespace volt::event {
class EventDispatcher;
}

namespace volt::ui {

enum class StackAxis {
  kVertical,
  kHorizontal,
};

class UILayer {
 public:
  struct FrameArgs {
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool minimized{false};
  };

  struct RenderPacket {
    std::uint32_t drawListCount{0};
    std::uint32_t clipRectCount{0};
    std::uint32_t widgetCount{0};
  };

  using WidgetPayload = std::variant<
      TextElement,
      ButtonElement,
      CheckboxElement,
      ToggleElement,
      SliderElement,
      TextInputElement,
      IconElement,
      ImageElement,
      PanelElement,
      ChartScaffoldElement,
      SchematicScaffoldElement>;

  struct WidgetNode {
    std::uint64_t id{0};
    WidgetKind kind{WidgetKind::kText};
    Rect bounds{};
    Rect clipRect{};
    std::string renderTargetOwnerKey{};
    std::string panelCacheKey{};
    bool cacheRenderTarget{false};
    bool cacheAllowImmediateFallback{true};
    bool cacheDirtyHint{false};
    WidgetPayload payload;
  };

  struct FlowColumn {
    Rect bounds{};
    float cursorY{0.0F};
    float spacing{8.0F};
    float padding{8.0F};
  };

  struct FlowRow {
    Rect bounds{};
    float cursorX{0.0F};
    float spacing{8.0F};
    float padding{8.0F};
    float rowHeight{0.0F};
  };

  struct StackLayout {
    Rect bounds{};
    StackAxis axis{StackAxis::kVertical};
    float cursorX{0.0F};
    float cursorY{0.0F};
    float spacing{8.0F};
    float padding{8.0F};
  };

  struct DockLayout {
    Rect remainingBounds{};
    float spacing{8.0F};
  };

  struct GridLayout {
    Rect bounds{};
    std::uint32_t columns{1};
    std::uint32_t currentColumn{0};
    float cursorY{0.0F};
    float rowHeight{0.0F};
    float currentRowHeight{0.0F};
    float columnWidth{0.0F};
    float columnSpacing{8.0F};
    float rowSpacing{8.0F};
    float padding{8.0F};
  };

  struct PanelContext {
    Rect clipRect{};
    float scrollOffsetY{0.0F};
    std::string renderTargetOwnerKey{};
  };

  struct RetainedPanelState {
    std::uint64_t widgetId{0};
    Rect bounds{};
    Rect clipRect{};
    std::string cacheKey{};
    std::string textureKey{};
    std::uint64_t signature{0};
    bool ready{false};
  };

  UILayer() = default;
  ~UILayer() = default;

  UILayer(const UILayer&) = delete;
  UILayer& operator=(const UILayer&) = delete;

  void beginFrame(const volt::platform::InputState& input, const FrameArgs& frameArgs);
  void setEventDispatcher(volt::event::EventDispatcher* dispatcher);
  void layoutPass();
  void paintPass();
  void endFrame();

  std::uint64_t addText(const Rect& bounds, const TextElement& text);
  std::uint64_t addButton(const Rect& bounds, const ButtonElement& button);
  std::uint64_t addCheckbox(const Rect& bounds, const CheckboxElement& checkbox);
  std::uint64_t addToggle(const Rect& bounds, const ToggleElement& toggle);
  std::uint64_t addSlider(const Rect& bounds, const SliderElement& slider);
  std::uint64_t addTextInput(const Rect& bounds, const TextInputElement& input);
  std::uint64_t addIcon(const Rect& bounds, const IconElement& icon);
  std::uint64_t addImage(const Rect& bounds, const ImageElement& image);
  std::uint64_t addPanel(const Rect& bounds, const PanelElement& panel);
  std::uint64_t addChartScaffold(const Rect& bounds, const ChartScaffoldElement& chart);
  std::uint64_t addSchematicScaffold(const Rect& bounds, const SchematicScaffoldElement& schematic);

  void beginPanel(const Rect& bounds, const PanelElement& panel);
  void endPanel();
  void beginScrollPanel(const Rect& bounds, const PanelElement& panel, float contentHeight, float& scrollOffsetY);
  void endScrollPanel();
  void beginFlowColumn(const Rect& bounds, float spacing = 8.0F, float padding = 8.0F);
  void endFlowColumn();
  void beginFlowRow(const Rect& bounds, float rowHeight, float spacing = 8.0F, float padding = 8.0F);
  void endFlowRow();
  void beginStack(const Rect& bounds, StackAxis axis, float spacing = 8.0F, float padding = 8.0F);
  void endStack();
  [[nodiscard]] Rect nextStackRect(float extent);
  void beginDock(const Rect& bounds, float spacing = 8.0F, float padding = 8.0F);
  void endDock();
  [[nodiscard]] Rect dockTopRect(float height);
  [[nodiscard]] Rect dockBottomRect(float height);
  [[nodiscard]] Rect dockLeftRect(float width);
  [[nodiscard]] Rect dockRightRect(float width);
  [[nodiscard]] Rect dockFillRect();
  void beginGrid(
      const Rect& bounds,
      std::uint32_t columns,
      float rowHeight,
      float columnSpacing = 8.0F,
      float rowSpacing = 8.0F,
      float padding = 8.0F);
  void endGrid();
  [[nodiscard]] Rect nextGridRect(std::uint32_t columnSpan = 1U, float height = -1.0F);
  std::uint64_t addTextFlow(float height, const TextElement& text);
  std::uint64_t addButtonFlow(float height, const ButtonElement& button);
  std::uint64_t addCheckboxFlow(float height, const CheckboxElement& checkbox);
  std::uint64_t addToggleFlow(float height, const ToggleElement& toggle);
  std::uint64_t addSliderFlow(float height, const SliderElement& slider);
  std::uint64_t addTextInputFlow(float height, const TextInputElement& input);
  std::uint64_t addIconFlow(float height, const IconElement& icon);
  std::uint64_t addImageFlow(float height, const ImageElement& image);
  std::uint64_t addTextRow(float width, const TextElement& text);
  std::uint64_t addButtonRow(float width, const ButtonElement& button);
  std::uint64_t addCheckboxRow(float width, const CheckboxElement& checkbox);
  std::uint64_t addToggleRow(float width, const ToggleElement& toggle);
  std::uint64_t addSliderRow(float width, const SliderElement& slider);
  std::uint64_t addTextInputRow(float width, const TextInputElement& input);
  std::uint64_t addIconRow(float width, const IconElement& icon);
  std::uint64_t addImageRow(float width, const ImageElement& image);

  void recordRenderPass();
  void recordRenderPass(VkCommandBuffer commandBuffer, std::uint32_t framebufferWidth, std::uint32_t framebufferHeight);
  [[nodiscard]] RenderPacket currentRenderPacket() const;
  [[nodiscard]] const std::vector<UiRenderCommand>& renderCommands() const;
  [[nodiscard]] std::uint64_t focusedWidgetId() const;
  [[nodiscard]] std::uint64_t hoveredWidgetId() const;
  [[nodiscard]] const std::vector<TextRun>& textRuns() const;
  [[nodiscard]] const UiMeshData& meshData() const;
  [[nodiscard]] const UIResourceRegistry& resources() const;
  UIResourceRegistry& resources();

 private:
  std::uint64_t addWidgetNode(WidgetKind kind, const Rect& bounds, const WidgetPayload& payload);
  Rect allocateFlowRect(float height);
  Rect allocateFlowRowRect(float width);
  Rect allocateStackRect(float extent);

  RenderPacket packet_{};
  FrameArgs frameArgs_{};
  std::vector<WidgetNode> widgets_;
  std::vector<UiRenderCommand> drawCommands_;
  std::vector<PanelContext> panelStack_;
  std::vector<FlowColumn> flowColumns_;
  std::vector<FlowRow> flowRows_;
  std::vector<StackLayout> stackLayouts_;
  std::vector<DockLayout> dockLayouts_;
  std::vector<GridLayout> gridLayouts_;
  std::vector<TextRun> textRuns_;
  UiMeshData meshData_{};
  std::unordered_map<std::string, RetainedPanelState> retainedPanelStates_;
  std::unordered_map<std::string, std::uint32_t> panelNameCounts_;
  UIResourceRegistry resources_{};
  StyleSheet styleSheet_{};
  bool styleLoaded_{false};
  std::uint64_t nextWidgetId_{1};
  std::uint64_t hoveredWidgetId_{0};
  std::uint64_t focusedWidgetId_{0};
  std::uint64_t activeWidgetId_{0};
  bool leftMouseDown_{false};
  bool leftMousePressed_{false};
  bool leftMouseReleased_{false};
  std::uint64_t frameIndex_{0};
  volt::event::EventDispatcher* eventDispatcher_{nullptr};
  std::uint64_t keyListenerId_{0};
  std::uint64_t textInputListenerId_{0};
  std::uint64_t mouseMoveListenerId_{0};
  std::uint64_t mouseButtonListenerId_{0};
  std::uint64_t mouseScrollListenerId_{0};
  std::uint64_t renderUiPassListenerId_{0};
  std::uint64_t consumedInputEventCount_{0};
  std::uint64_t consumedRenderEventCount_{0};
  std::vector<volt::event::KeyInputEvent> keyInputEvents_;
  std::vector<char32_t> textInputCodepoints_;
  std::uint64_t editingTextInputId_{0};
  std::size_t textInputCursorByte_{0};
  double lastMouseX_{0.0};
  double lastMouseY_{0.0};
  double scrollDeltaY_{0.0};
};

}  // namespace volt::ui
