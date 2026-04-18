#include "volt/event/EventDispatcher.hpp"
#include "volt/platform/InputState.hpp"
#include "volt/ui/UILayer.hpp"
#include "volt/ui/UIMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[ui-layer-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

bool approximatelyEqual(float lhs, float rhs, float tolerance = 0.01F) {
  return std::fabs(lhs - rhs) <= tolerance;
}

bool approximatelyEqualColor(const volt::ui::Color& lhs, const volt::ui::Color& rhs, float tolerance = 0.01F) {
  return approximatelyEqual(lhs.r, rhs.r, tolerance) &&
         approximatelyEqual(lhs.g, rhs.g, tolerance) &&
         approximatelyEqual(lhs.b, rhs.b, tolerance) &&
         approximatelyEqual(lhs.a, rhs.a, tolerance);
}

std::string describeBatches(const volt::ui::UiMeshData& mesh) {
  std::string description;
  for (std::size_t index = 0; index < mesh.batches.size(); ++index) {
    if (!description.empty()) {
      description += "; ";
    }
    description += mesh.batches[index].layer == volt::ui::UiBatchLayer::kOpaque ? "opaque" : "transparent";
    description += ":" + std::to_string(mesh.batches[index].indexCount);
    description += ":" + mesh.batches[index].textureKey;
  }
  return description;
}

std::optional<std::filesystem::path> findWorkspaceRoot() {
  std::error_code ec;
  std::filesystem::path cursor = std::filesystem::current_path(ec);
  if (ec) {
    return std::nullopt;
  }

  for (int depth = 0; depth < 12; ++depth) {
    const auto fontPath = cursor / "assets" / "fonts" / "DefaultFont.ttf";
    if (std::filesystem::exists(fontPath, ec) && !ec) {
      return cursor;
    }

    if (!cursor.has_parent_path()) {
      break;
    }

    const std::filesystem::path parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }

  return std::nullopt;
}

bool runWidgetFrame(
    volt::ui::UILayer& uiLayer,
    const volt::platform::InputState& input,
    const std::function<void(volt::ui::UILayer&)>& build) {
  uiLayer.beginFrame(input, {.width = 400U, .height = 300U, .minimized = false});
  build(uiLayer);
  uiLayer.layoutPass();
  uiLayer.paintPass();
  uiLayer.endFrame();
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path oldCwd = std::filesystem::current_path();

  const auto workspaceRoot = findWorkspaceRoot();
  ok = expect(workspaceRoot.has_value(), "locate workspace root with assets/fonts/DefaultFont.ttf") && ok;
  if (workspaceRoot.has_value()) {
    std::error_code ec;
    std::filesystem::current_path(*workspaceRoot, ec);
    ok = expect(!ec, "switch to workspace root cwd") && ok;
  }

  volt::ui::UILayer uiLayer;
  uiLayer.beginFrame(volt::platform::InputState{}, {.width = 800U, .height = 600U, .minimized = false});
  uiLayer.beginFlowColumn({10.0F, 20.0F, 200.0F, 300.0F}, 8.0F, 8.0F);
  uiLayer.addImageFlow(70.0F, volt::ui::ImageElement{"__white", {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.addButtonFlow(30.0F, volt::ui::ButtonElement{"Button", true, false, {0.1F, 0.2F, 0.3F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.endFlowColumn();
  uiLayer.layoutPass();
  uiLayer.paintPass();

  const auto& commands = uiLayer.renderCommands();
  ok = expect(commands.size() >= 3U, "paint pass should emit image plus button background and label commands") && ok;

  const auto* imageCommand = commands.empty() ? nullptr : std::get_if<volt::ui::UiImageCommand>(&commands[0]);
  ok = expect(imageCommand != nullptr, "first render command should be the image") && ok;
  if (imageCommand != nullptr) {
    ok = expect(approximatelyEqual(imageCommand->bounds.width, 70.0F), "square flow image width should match requested height") && ok;
    ok = expect(approximatelyEqual(imageCommand->bounds.height, 70.0F), "square flow image height should remain requested height") && ok;
  }

  const auto* buttonCommand = commands.size() < 2U ? nullptr : std::get_if<volt::ui::UiRectCommand>(&commands[1]);
  ok = expect(buttonCommand != nullptr, "second render command should be the button background") && ok;
  if (buttonCommand != nullptr) {
    ok = expect(approximatelyEqual(buttonCommand->bounds.width, 184.0F), "button flow width should still use full inner column width") && ok;
    ok = expect(approximatelyEqual(buttonCommand->bounds.height, 30.0F), "button flow height should remain unchanged") && ok;
  }

  volt::ui::UILayer clippedFlowLayer;
  clippedFlowLayer.beginFrame(volt::platform::InputState{}, {.width = 800U, .height = 600U, .minimized = false});
  clippedFlowLayer.beginPanel({20.0F, 30.0F, 100.0F, 90.0F}, volt::ui::PanelElement{"clip-panel", {0.1F, 0.1F, 0.1F, 1.0F}, 6.0F});
  clippedFlowLayer.beginFlowColumn({0.0F, 0.0F, 200.0F, 200.0F}, 8.0F, 8.0F);
  clippedFlowLayer.addButtonFlow(24.0F, volt::ui::ButtonElement{"Clipped", true, false, {0.2F, 0.2F, 0.4F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  clippedFlowLayer.endFlowColumn();
  clippedFlowLayer.endPanel();
  clippedFlowLayer.layoutPass();
  clippedFlowLayer.paintPass();

  const auto& clippedCommands = clippedFlowLayer.renderCommands();
  const auto* clippedButton = clippedCommands.size() < 2U ? nullptr : std::get_if<volt::ui::UiRectCommand>(&clippedCommands[1]);
  ok = expect(clippedButton != nullptr, "clipped flow should emit a button rect after the panel background") && ok;
  if (clippedButton != nullptr) {
    ok = expect(approximatelyEqual(clippedButton->bounds.x, 28.0F), "flow column should inherit panel-clipped x origin") && ok;
    ok = expect(approximatelyEqual(clippedButton->bounds.y, 38.0F), "flow column should inherit panel-clipped y origin") && ok;
    ok = expect(approximatelyEqual(clippedButton->bounds.width, 84.0F), "flow column should use the panel-clipped inner width") && ok;
    ok = expect(approximatelyEqual(clippedButton->bounds.height, 24.0F), "flow column should preserve requested row height") && ok;
  }

  volt::platform::InputState hoverInput{};
  hoverInput.mouse.x = 40.0;
  hoverInput.mouse.y = 40.0;

  volt::ui::UILayer interactionLayer;
  interactionLayer.beginFrame(hoverInput, {.width = 400U, .height = 300U, .minimized = false});
  const std::uint64_t disabledButtonId = interactionLayer.addButton(
      {20.0F, 20.0F, 120.0F, 30.0F},
      volt::ui::ButtonElement{"Disabled", false, false, {0.40F, 0.30F, 0.20F, 1.0F}, {0.90F, 0.80F, 0.70F, 1.0F}});
  interactionLayer.layoutPass();
  interactionLayer.paintPass();

  ok = expect(interactionLayer.hoveredWidgetId() != disabledButtonId,
              "disabled buttons should not become hovered through input snapshot hit testing") && ok;

  const auto& interactionCommands = interactionLayer.renderCommands();
  const auto* disabledButtonRect = interactionCommands.empty() ? nullptr : std::get_if<volt::ui::UiRectCommand>(&interactionCommands[0]);
  const auto* disabledButtonText = interactionCommands.size() < 2U ? nullptr : std::get_if<volt::ui::UiTextCommand>(&interactionCommands[1]);
  ok = expect(disabledButtonRect != nullptr, "disabled button should still render its background") && ok;
  ok = expect(disabledButtonText != nullptr, "disabled button should still render its label") && ok;
  if (disabledButtonRect != nullptr) {
    ok = expect(disabledButtonRect->fill.r < 0.40F && disabledButtonRect->fill.g < 0.30F,
                "disabled button background should be visually muted") && ok;
  }
  if (disabledButtonText != nullptr) {
    ok = expect(disabledButtonText->color.r < 0.90F && disabledButtonText->color.g < 0.80F,
                "disabled button label should be visually muted") && ok;
  }

  volt::ui::UILayer sliderStyleLayer;
  sliderStyleLayer.beginFrame(volt::platform::InputState{}, {.width = 400U, .height = 300U, .minimized = false});
  sliderStyleLayer.addSlider(
      {16.0F, 16.0F, 120.0F, 14.0F},
      volt::ui::SliderElement{0.0F, 100.0F, 25.0F, {0.30F, 0.10F, 0.20F, 1.0F}, {0.90F, 0.40F, 0.20F, 1.0F}, true});
  sliderStyleLayer.layoutPass();
  sliderStyleLayer.paintPass();

  const auto& sliderCommands = sliderStyleLayer.renderCommands();
  const auto* sliderTrack = sliderCommands.empty() ? nullptr : std::get_if<volt::ui::UiRectCommand>(&sliderCommands[0]);
  const auto* sliderKnob = sliderCommands.size() < 2U ? nullptr : std::get_if<volt::ui::UiRectCommand>(&sliderCommands[1]);
  ok = expect(sliderTrack != nullptr, "slider should render a track rect") && ok;
  ok = expect(sliderKnob != nullptr, "slider should render a knob rect") && ok;
  if (sliderTrack != nullptr) {
    ok = expect(approximatelyEqualColor(sliderTrack->fill, {0.30F, 0.10F, 0.20F, 1.0F}),
                "slider track should honor the widget's track color") && ok;
  }
  if (sliderKnob != nullptr) {
    ok = expect(approximatelyEqualColor(sliderKnob->fill, {0.90F, 0.40F, 0.20F, 1.0F}),
                "slider knob should honor the widget's knob color") && ok;
  }

  float scrollOffsetY = 0.0F;
  volt::platform::InputState scrollInput{};
  scrollInput.mouse.x = 40.0;
  scrollInput.mouse.y = 40.0;
  scrollInput.mouse.scrollY = -2.0;

  volt::ui::UILayer scrollLayer;
  scrollLayer.beginFrame(scrollInput, {.width = 400U, .height = 300U, .minimized = false});
  scrollLayer.beginScrollPanel(
      {20.0F, 20.0F, 100.0F, 60.0F},
      volt::ui::PanelElement{"scroll-panel", {0.10F, 0.10F, 0.12F, 1.0F}, 6.0F},
      140.0F,
      scrollOffsetY);
  scrollLayer.beginFlowColumn({20.0F, 20.0F, 100.0F, 140.0F}, 8.0F, 8.0F);
  scrollLayer.addButtonFlow(24.0F, volt::ui::ButtonElement{"One", true, false, {0.2F, 0.2F, 0.4F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  scrollLayer.addButtonFlow(24.0F, volt::ui::ButtonElement{"Two", true, false, {0.2F, 0.2F, 0.4F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  scrollLayer.addButtonFlow(24.0F, volt::ui::ButtonElement{"Three", true, false, {0.2F, 0.2F, 0.4F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  scrollLayer.endFlowColumn();
  scrollLayer.endScrollPanel();
  scrollLayer.layoutPass();
  scrollLayer.paintPass();

  ok = expect(approximatelyEqual(scrollOffsetY, 48.0F),
              "scroll panel should advance its external scroll offset from mouse wheel input") && ok;

  const auto& scrollCommands = scrollLayer.renderCommands();
  const auto* scrolledSecondButton = scrollCommands.size() < 4U ? nullptr : std::get_if<volt::ui::UiRectCommand>(&scrollCommands[3]);
  ok = expect(scrolledSecondButton != nullptr,
              "scroll panel should render scrolled child button commands after the panel background") && ok;
  if (scrolledSecondButton != nullptr) {
    ok = expect(approximatelyEqual(scrolledSecondButton->bounds.y, 20.0F),
                "scrolled content should be clipped against the panel viewport") && ok;
    ok = expect(approximatelyEqual(scrolledSecondButton->bounds.height, 16.0F),
                "partially visible scrolled content should be height-clipped by the panel viewport") && ok;
  }

  std::vector<volt::ui::UiRenderCommand> textCommands;
  textCommands.push_back(volt::ui::UiTextCommand{
      1U,
      {40.0F, 30.0F, 240.0F, 100.0F},
      {40.0F, 30.0F, 240.0F, 100.0F},
      "Oa",
      "default",
      48.0F,
      2U,
      {1.0F, 1.0F, 1.0F, 1.0F},
  });

  const volt::ui::UiMeshData mesh = volt::ui::buildUiMesh(textCommands);
  ok = expect(mesh.vertices.size() >= 4U, "text mesh should emit glyph vertices") && ok;
  if (mesh.vertices.size() >= 4U) {
    float minY = mesh.vertices.front().y;
    float maxY = mesh.vertices.front().y;
    for (const auto& vertex : mesh.vertices) {
      minY = std::min(minY, vertex.y);
      maxY = std::max(maxY, vertex.y);
    }

    const float inkCenterY = (minY + maxY) * 0.5F;
    const float boxCenterY = 30.0F + 50.0F;
    ok = expect(std::fabs(inkCenterY - boxCenterY) <= 2.0F,
                "text ink bounds should be vertically centered inside the label rect") && ok;
  }

  std::vector<volt::ui::UiRenderCommand> fractionalTextCommandsA;
  fractionalTextCommandsA.push_back(volt::ui::UiTextCommand{
      2U,
      {40.25F, 30.25F, 240.0F, 100.0F},
      {40.25F, 30.25F, 240.0F, 100.0F},
      "Volt",
      "default",
      18.0F,
      4U,
      {1.0F, 1.0F, 1.0F, 1.0F},
  });
  std::vector<volt::ui::UiRenderCommand> fractionalTextCommandsB;
  fractionalTextCommandsB.push_back(volt::ui::UiTextCommand{
      3U,
      {40.75F, 30.75F, 240.0F, 100.0F},
      {40.75F, 30.75F, 240.0F, 100.0F},
      "Volt",
      "default",
      18.0F,
      4U,
      {1.0F, 1.0F, 1.0F, 1.0F},
  });

  const volt::ui::UiMeshData fractionalMeshA = volt::ui::buildUiMesh(fractionalTextCommandsA);
  const volt::ui::UiMeshData fractionalMeshB = volt::ui::buildUiMesh(fractionalTextCommandsB);
  ok = expect(!fractionalMeshA.vertices.empty() && !fractionalMeshB.vertices.empty(),
              "fractional text meshes should emit vertices") && ok;
  if (!fractionalMeshA.vertices.empty() && !fractionalMeshB.vertices.empty()) {
    float minYA = fractionalMeshA.vertices.front().y;
    float minYB = fractionalMeshB.vertices.front().y;
    for (const auto& vertex : fractionalMeshA.vertices) {
      minYA = std::min(minYA, vertex.y);
    }
    for (const auto& vertex : fractionalMeshB.vertices) {
      minYB = std::min(minYB, vertex.y);
    }

    ok = expect(std::fabs((minYB - minYA) - 0.5F) <= 0.15F,
                "text mesh should preserve fractional vertical movement instead of snapping to whole pixels") && ok;
  }

  std::vector<volt::ui::UiRenderCommand> opaqueBatchCommands;
  opaqueBatchCommands.push_back(volt::ui::UiRectCommand{
      10U,
      {10.0F, 10.0F, 40.0F, 20.0F},
      {0.0F, 0.0F, 120.0F, 60.0F},
      {0.2F, 0.2F, 0.2F, 1.0F},
      0.0F,
  });
    opaqueBatchCommands.push_back(volt::ui::UiImageCommand{
      11U,
      {20.0F, 14.0F, 40.0F, 20.0F},
      {0.0F, 0.0F, 120.0F, 60.0F},
      "test-image",
      {1.0F, 1.0F, 1.0F, 1.0F},
  });
  opaqueBatchCommands.push_back(volt::ui::UiRectCommand{
      12U,
      {30.0F, 18.0F, 40.0F, 20.0F},
      {0.0F, 0.0F, 120.0F, 60.0F},
      {0.1F, 0.5F, 0.3F, 1.0F},
      0.0F,
  });

  const volt::ui::UiMeshData defaultOpaqueMesh = volt::ui::buildUiMesh(opaqueBatchCommands);
  ok = expect(defaultOpaqueMesh.batches.size() == 3U,
          "default mesh build should preserve paint-order-separated solid batches (got " +
            std::to_string(defaultOpaqueMesh.batches.size()) + ")") && ok;

  const volt::ui::UiMeshData optimizedOpaqueMesh =
      volt::ui::buildUiMesh(opaqueBatchCommands, volt::ui::UiMeshBuildOptions{true});
  ok = expect(optimizedOpaqueMesh.batches.size() == 2U,
          "opaque mesh build should merge solid rects across transparent separators (got " +
            std::to_string(optimizedOpaqueMesh.batches.size()) + "; batches=" +
            describeBatches(optimizedOpaqueMesh) + ")") && ok;
  if (optimizedOpaqueMesh.batches.size() == 2U) {
    ok = expect(optimizedOpaqueMesh.batches[0].layer == volt::ui::UiBatchLayer::kOpaque,
                "opaque mesh build should emit opaque batches first") && ok;
    ok = expect(optimizedOpaqueMesh.batches[0].indexCount == 12U,
                "opaque batch should contain both solid rects") && ok;
    ok = expect(optimizedOpaqueMesh.batches[1].layer == volt::ui::UiBatchLayer::kTransparent,
          "image content should stay in the transparent lane") && ok;
  }
  if (optimizedOpaqueMesh.vertices.size() >= 12U) {
    const float firstOpaqueDepth = optimizedOpaqueMesh.vertices[0].z;
    const float secondOpaqueDepth = optimizedOpaqueMesh.vertices[4].z;
    const float transparentDepth = optimizedOpaqueMesh.vertices[8].z;
    ok = expect(firstOpaqueDepth < transparentDepth && transparentDepth < secondOpaqueDepth,
                "vertex depths should preserve original paint order after opaque batch reordering") && ok;
  }

  volt::ui::UILayer stackLayoutLayer;
  stackLayoutLayer.beginFrame(volt::platform::InputState{}, {.width = 400U, .height = 300U, .minimized = false});
  stackLayoutLayer.beginStack({10.0F, 12.0F, 150.0F, 100.0F}, volt::ui::StackAxis::kVertical, 6.0F, 4.0F);
  const volt::ui::Rect stackRectA = stackLayoutLayer.nextStackRect(20.0F);
  const volt::ui::Rect stackRectB = stackLayoutLayer.nextStackRect(30.0F);
  stackLayoutLayer.endStack();
  ok = expect(approximatelyEqual(stackRectA.x, 14.0F) && approximatelyEqual(stackRectA.y, 16.0F),
              "vertical stack should start at padded origin") && ok;
  ok = expect(approximatelyEqual(stackRectA.width, 142.0F) && approximatelyEqual(stackRectA.height, 20.0F),
              "vertical stack should fill cross-axis width and preserve requested height") && ok;
  ok = expect(approximatelyEqual(stackRectB.y, 42.0F),
              "vertical stack should advance by prior extent plus spacing") && ok;

  volt::ui::UILayer dockLayoutLayer;
  dockLayoutLayer.beginFrame(volt::platform::InputState{}, {.width = 400U, .height = 300U, .minimized = false});
  dockLayoutLayer.beginDock({0.0F, 0.0F, 200.0F, 120.0F}, 6.0F, 8.0F);
  const volt::ui::Rect dockTop = dockLayoutLayer.dockTopRect(20.0F);
  const volt::ui::Rect dockLeft = dockLayoutLayer.dockLeftRect(40.0F);
  const volt::ui::Rect dockFill = dockLayoutLayer.dockFillRect();
  dockLayoutLayer.endDock();
  ok = expect(approximatelyEqual(dockTop.x, 8.0F) && approximatelyEqual(dockTop.width, 184.0F),
              "dock top should honor padding and span the remaining width") && ok;
  ok = expect(approximatelyEqual(dockLeft.y, 34.0F) && approximatelyEqual(dockLeft.width, 40.0F),
              "dock left should consume width from the remaining area below the top slot") && ok;
  ok = expect(approximatelyEqual(dockFill.x, 54.0F) && approximatelyEqual(dockFill.width, 138.0F),
              "dock fill should receive the remainder after top and left slots") && ok;

  volt::ui::UILayer gridLayoutLayer;
  gridLayoutLayer.beginFrame(volt::platform::InputState{}, {.width = 400U, .height = 300U, .minimized = false});
  gridLayoutLayer.beginGrid({20.0F, 20.0F, 170.0F, 100.0F}, 3U, 20.0F, 6.0F, 8.0F, 4.0F);
  const volt::ui::Rect gridRectA = gridLayoutLayer.nextGridRect(1U);
  const volt::ui::Rect gridRectB = gridLayoutLayer.nextGridRect(2U);
  const volt::ui::Rect gridRectC = gridLayoutLayer.nextGridRect(1U);
  gridLayoutLayer.endGrid();
  ok = expect(approximatelyEqual(gridRectA.x, 24.0F) && approximatelyEqual(gridRectA.width, 50.0F),
              "grid should compute even column widths inside padded bounds") && ok;
  ok = expect(approximatelyEqual(gridRectB.x, 80.0F) && approximatelyEqual(gridRectB.width, 106.0F),
              "grid should expand spanned cells across adjacent columns and spacing") && ok;
  ok = expect(approximatelyEqual(gridRectC.x, 24.0F) && approximatelyEqual(gridRectC.y, 52.0F),
              "grid should wrap to the next row when the current row runs out of columns") && ok;

  bool checkboxValue = false;
  volt::ui::UILayer checkboxLayer;
  runWidgetFrame(
      checkboxLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 40.0;
        input.mouse.y = 36.0;
        input.mouse.down[0] = true;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addCheckbox(
            {20.0F, 20.0F, 160.0F, 32.0F},
            volt::ui::CheckboxElement{"Receive updates", &checkboxValue, true});
      });
  runWidgetFrame(
      checkboxLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 40.0;
        input.mouse.y = 36.0;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addCheckbox(
            {20.0F, 20.0F, 160.0F, 32.0F},
            volt::ui::CheckboxElement{"Receive updates", &checkboxValue, true});
      });
  ok = expect(checkboxValue, "checkbox should toggle its bound boolean on click release") && ok;

  bool dispatchedCheckboxValue = false;
  volt::event::EventDispatcher mouseDispatcher;
  volt::ui::UILayer dispatchedCheckboxLayer;
  dispatchedCheckboxLayer.setEventDispatcher(&mouseDispatcher);

  mouseDispatcher.enqueue({
      .type = volt::event::EventType::kMouseButton,
      .payload = volt::event::MouseButtonEvent{.button = 0, .action = 1, .mods = 0},
  });
  mouseDispatcher.dispatchQueued();
  runWidgetFrame(
      dispatchedCheckboxLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 40.0;
        input.mouse.y = 36.0;
        input.mouse.down[0] = true;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addCheckbox(
            {20.0F, 20.0F, 160.0F, 32.0F},
            volt::ui::CheckboxElement{"Dispatcher click", &dispatchedCheckboxValue, true});
      });

  mouseDispatcher.enqueue({
      .type = volt::event::EventType::kMouseButton,
      .payload = volt::event::MouseButtonEvent{.button = 0, .action = 0, .mods = 0},
  });
  mouseDispatcher.dispatchQueued();
  runWidgetFrame(
      dispatchedCheckboxLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 40.0;
        input.mouse.y = 36.0;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addCheckbox(
            {20.0F, 20.0F, 160.0F, 32.0F},
            volt::ui::CheckboxElement{"Dispatcher click", &dispatchedCheckboxValue, true});
      });
  ok = expect(dispatchedCheckboxValue,
              "dispatcher-fed mouse press and release should still toggle interactive widgets") && ok;

  bool toggleValue = false;
  volt::ui::UILayer toggleLayer;
  runWidgetFrame(
      toggleLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 60.0;
        input.mouse.y = 36.0;
        input.mouse.down[0] = true;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addToggle(
            {20.0F, 20.0F, 180.0F, 32.0F},
            volt::ui::ToggleElement{"GPU atlas", &toggleValue, true});
      });
  runWidgetFrame(
      toggleLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 60.0;
        input.mouse.y = 36.0;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addToggle(
            {20.0F, 20.0F, 180.0F, 32.0F},
            volt::ui::ToggleElement{"GPU atlas", &toggleValue, true});
      });
  ok = expect(toggleValue, "toggle should flip its bound boolean on click release") && ok;

  constexpr int kTestKeyBackspace = 0x08;
  constexpr int kTestKeyLeft = 0x25;

  volt::event::EventDispatcher dispatcher;
  volt::ui::UILayer textInputLayer;
  textInputLayer.setEventDispatcher(&dispatcher);
  std::string textValue = "Hi";

  runWidgetFrame(
      textInputLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 40.0;
        input.mouse.y = 36.0;
        input.mouse.down[0] = true;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addTextInput(
            {20.0F, 20.0F, 180.0F, 32.0F},
            volt::ui::TextInputElement{&textValue, "Type here", 32U, true});
      });
  runWidgetFrame(
      textInputLayer,
      [] {
        volt::platform::InputState input{};
        input.mouse.x = 40.0;
        input.mouse.y = 36.0;
        return input;
      }(),
      [&](volt::ui::UILayer& layer) {
        layer.addTextInput(
            {20.0F, 20.0F, 180.0F, 32.0F},
            volt::ui::TextInputElement{&textValue, "Type here", 32U, true});
      });

  dispatcher.enqueue({
      .type = volt::event::EventType::kTextInput,
      .payload = volt::event::TextInputEvent{.codepoint = U'!'},
  });
  dispatcher.dispatchQueued();
  runWidgetFrame(
      textInputLayer,
      volt::platform::InputState{},
      [&](volt::ui::UILayer& layer) {
        layer.addTextInput(
            {20.0F, 20.0F, 180.0F, 32.0F},
            volt::ui::TextInputElement{&textValue, "Type here", 32U, true});
      });
  ok = expect(textValue == "Hi!", "text input should append printable text input events") && ok;

  dispatcher.enqueue({
      .type = volt::event::EventType::kKeyInput,
      .payload = volt::event::KeyInputEvent{.key = kTestKeyLeft, .action = 1, .mods = 0},
  });
  dispatcher.enqueue({
      .type = volt::event::EventType::kKeyInput,
      .payload = volt::event::KeyInputEvent{.key = kTestKeyBackspace, .action = 1, .mods = 0},
  });
  dispatcher.dispatchQueued();
  runWidgetFrame(
      textInputLayer,
      volt::platform::InputState{},
      [&](volt::ui::UILayer& layer) {
        layer.addTextInput(
            {20.0F, 20.0F, 180.0F, 32.0F},
            volt::ui::TextInputElement{&textValue, "Type here", 32U, true});
      });
  ok = expect(textValue == "H!", "text input should honor arrow and backspace editing events") && ok;

  const auto& textInputCommands = textInputLayer.renderCommands();
  const auto* renderedInputText = textInputCommands.size() < 3U
      ? nullptr
      : std::get_if<volt::ui::UiTextCommand>(&textInputCommands[2]);
  ok = expect(renderedInputText != nullptr, "text input should render its current text payload") && ok;
  if (renderedInputText != nullptr) {
    ok = expect(renderedInputText->text.find('|') != std::string::npos,
                "focused text input should render a visible caret marker") && ok;
  }

  std::error_code restoreEc;
  std::filesystem::current_path(oldCwd, restoreEc);
  ok = expect(!restoreEc, "restore original cwd") && ok;

  if (!ok) {
    std::cerr << "[ui-layer-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[ui-layer-test] All UILayer tests passed." << '\n';
  return 0;
}