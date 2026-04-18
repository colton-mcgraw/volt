#include "AppUi.hpp"

#include "volt/core/Logging.hpp"
#include "volt/io/assets/Font.hpp"
#include "volt/io/assets/Manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace volt::app {
namespace {

using volt::platform::Window;
using volt::ui::Color;
using volt::ui::PanelElement;
using volt::ui::Rect;
using volt::ui::TextElement;
using volt::ui::UILayer;

const char* samplingModeLabel(volt::io::FontSamplingMode mode);

std::string buildActiveFontLabel() {
  std::string label{"Font: default"};

  volt::io::FontMetrics effectiveMetrics{};
  if (volt::io::defaultFontMetrics(effectiveMetrics)) {
    const std::string effectiveMode = effectiveMetrics.msdfEnabled ? "msdf" : (effectiveMetrics.sdfEnabled ? "sdf" : "coverage");
    const std::string samplingMode = samplingModeLabel(effectiveMetrics.samplingMode);
    label += " | eff=" + effectiveMode;
    label += " sample=" + samplingMode;
    label += " spread=" + std::to_string(static_cast<int>(std::lround(effectiveMetrics.sdfSpreadPx)));
    label += " edge=" + std::to_string(std::round(effectiveMetrics.sdfEdge * 100.0F) / 100.0F);
    label += " aa=" + std::to_string(std::round(effectiveMetrics.sdfAaStrength * 100.0F) / 100.0F);
    label += " rev=" + std::to_string(volt::io::defaultFontAtlasRevision());
  }

  return label;
}

std::string buildDisplayMetricsLabel(const Window& window) {
  const auto metrics = window.displayMetrics();
  const auto [logicalWidth, logicalHeight] = window.logicalExtent();
  const auto [framebufferWidth, framebufferHeight] = window.framebufferExtent();

  auto rounded = [](float value) {
    return std::to_string(static_cast<int>(std::lround(value)));
  };
  auto rounded2 = [](float value) {
    return std::to_string(std::round(value * 100.0F) / 100.0F);
  };

  return std::string("Display: dpi=") + rounded(metrics.dpiX) + "x" + rounded(metrics.dpiY) +
         " ppi=" + rounded(metrics.ppiX) + "x" + rounded(metrics.ppiY) +
         " scale=" + rounded2(metrics.contentScaleX) + "x" + rounded2(metrics.contentScaleY) +
         " ui=" + std::to_string(logicalWidth) + "x" + std::to_string(logicalHeight) +
         " fb=" + std::to_string(framebufferWidth) + "x" + std::to_string(framebufferHeight);
}

std::string buildFrameTimingLabel(const AppUiFrameStats& frameStats) {
  if (frameStats.loopMs <= 0 && frameStats.renderCpuMs <= 0) {
    return "Loop/Wait/UI/Renderer: -";
  }

  return std::string("Loop ") + std::to_string(frameStats.loopMs) +
         " ms | Wait " + std::to_string(frameStats.waitMs) +
         " | UI " + std::to_string(frameStats.uiMs) +
         " | Renderer " + std::to_string(frameStats.rendererMs) +
         " | RenderCPU " + std::to_string(frameStats.renderCpuMs);
}

std::string buildFrameOverlayHeadline(const AppUiFrameStats& frameStats) {
  if (frameStats.renderCpuMs <= 0) {
    return "Frame CPU: -";
  }

  return std::string("Frame CPU: ") + std::to_string(frameStats.renderCpuMs) + " ms";
}

std::string buildFrameOverlayLoopLabel(const AppUiFrameStats& frameStats) {
  if (frameStats.loopMs <= 0) {
    return "Loop/Wait: -";
  }

  return std::string("Loop ") + std::to_string(frameStats.loopMs) +
         " ms | Wait " + std::to_string(frameStats.waitMs) + " ms";
}

std::string buildFrameOverlayStageLabel(const AppUiFrameStats& frameStats) {
  return std::string("UI ") + std::to_string(frameStats.uiMs) +
         " ms | Renderer " + std::to_string(frameStats.rendererMs) + " ms";
}

std::string buildFrameOverlayVectorTextLabel(const AppUiFrameStats& frameStats) {
  if (!frameStats.vectorTextGpuValid) {
    return "VT GPU fc/cs/fe+bc/ts/be+f: -";
  }

  return std::string("VT GPU ") +
         std::to_string(frameStats.vectorTextFlattenCountMs) + "/" +
         std::to_string(frameStats.vectorTextCurveScanMs) + "/" +
         std::to_string(frameStats.vectorTextFlattenEmitBinCountMs) + "/" +
         std::to_string(frameStats.vectorTextTileScanMs) + "/" +
         std::to_string(frameStats.vectorTextBinEmitFineMs) + " ms";
}

std::string buildFrameOverlayRendererSyncLabel(const AppUiFrameStats& frameStats) {
  if (!frameStats.rendererSyncValid) {
    return "R sync fw/acq/rec/upl/draw/sub/pre: -";
  }
  std::string label = std::string("R sync ") +
         std::to_string(frameStats.rendererFenceWaitMs) + "/" +
         std::to_string(frameStats.rendererAcquireMs) + "/" +
         std::to_string(frameStats.rendererRecordMs) + "/" +
         std::to_string(frameStats.rendererRecordUploadMs) + "/" +
         std::to_string(frameStats.rendererRecordDrawMs) + "/" +
         std::to_string(frameStats.rendererUiVectorTextPrepMs) + "/" +
         std::to_string(frameStats.rendererSubmitMs) + "/" +
         std::to_string(frameStats.rendererPresentMs) + " ms";
  label += " | UI draws: " + std::to_string(frameStats.uiDrawCallCount);
  if (frameStats.uiDrawCallCount > 0) {
    label += " (" + std::to_string(frameStats.uiDrawCallAvgMs) + " avg / " +
             std::to_string(frameStats.uiDrawCallMaxMs) + " max @" +
             std::to_string(frameStats.uiDrawCallMaxBatchIndex) + ":" +
             frameStats.uiDrawCallMaxTextureKey + ")";
  }
  if (frameStats.uiDescriptorResolveTotalMs > 0.0) {
    label += " | tex-resolve " + std::to_string(frameStats.uiDescriptorResolveTotalMs) +
             "/" + std::to_string(frameStats.uiDescriptorResolveMaxMs) + " ms";
    if (!frameStats.uiDescriptorResolveMaxTextureKey.empty()) {
      label += " (max " + frameStats.uiDescriptorResolveMaxTextureKey + ")";
    }
  }
  return label;
}

const char* samplingModeLabel(volt::io::FontSamplingMode mode) {
  switch (mode) {
    case volt::io::FontSamplingMode::kSignedDistanceField:
      return "sdf";
    case volt::io::FontSamplingMode::kMultiChannelSignedDistanceField:
      return "msdf";
    case volt::io::FontSamplingMode::kAuto:
    default:
      return "auto";
  }
}

bool dumpDefaultFontAtlasImage(std::string& outDumpPath, std::string& outError) {
  outDumpPath.clear();
  outError.clear();

  if (!volt::io::ensureDefaultFontAtlas()) {
    outError = "default font atlas is not available";
    return false;
  }

  const std::filesystem::path sourcePath =
      std::filesystem::path("assets") / "images" / "ui-font-default.png";
  std::error_code ec;
  if (!std::filesystem::exists(sourcePath, ec) || ec) {
    outError = "source atlas image is missing";
    return false;
  }

  const std::filesystem::path dumpDir =
      std::filesystem::path("logs") / "atlas-dumps";
  std::filesystem::create_directories(dumpDir, ec);
  if (ec) {
    outError = "failed to create dump directory: " + ec.message();
    return false;
  }

  const std::uint64_t revision = volt::io::defaultFontAtlasRevision();
  const std::uint64_t timestampMs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const std::filesystem::path dumpPath = dumpDir /
                                         ("ui-font-default-rev-" + std::to_string(revision) +
                                          "-" + std::to_string(timestampMs) + ".png");

  std::filesystem::copy_file(sourcePath, dumpPath, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    outError = "failed to write atlas dump: " + ec.message();
    return false;
  }

  outDumpPath = dumpPath.lexically_normal().string();
  return true;
}

Rect insetRect(const Rect& bounds, float inset) {
  const float clampedInset = std::max(0.0F, inset);
  return {
      bounds.x + clampedInset,
      bounds.y + clampedInset,
      std::max(0.0F, bounds.width - (clampedInset * 2.0F)),
      std::max(0.0F, bounds.height - (clampedInset * 2.0F)),
  };
}

void addCardHeader(UILayer& uiLayer, const Rect& bounds, const std::string& title, const std::string& subtitle) {
  uiLayer.addText(
      {bounds.x + 10.0F, bounds.y + 8.0F, bounds.width - 20.0F, 16.0F},
      TextElement{title, "default", 12.0F, {0.95F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addText(
      {bounds.x + 10.0F, bounds.y + 22.0F, bounds.width - 20.0F, 12.0F},
      TextElement{subtitle, "default", 9.0F, {0.66F, 0.74F, 0.84F, 1.0F}});
}

void buildSidebarPanel(UILayer& uiLayer, const Rect& bounds, AppUiFrameBindings* bindings) {
  if (bindings == nullptr) {
    return;
  }

  uiLayer.beginPanel(bounds, PanelElement{"left_panel", {0.08F, 0.10F, 0.12F, 1.0F}, 10.0F, false});
  const Rect inner = insetRect(bounds, 8.0F);
  uiLayer.beginFlowColumn(inner, 10.0F, 8.0F);
  uiLayer.addTextFlow(28.0F, TextElement{"Volt UI Foundation", "default", 18.0F, {0.95F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addTextFlow(20.0F, TextElement{"Actions", "default", 13.0F, {0.70F, 0.74F, 0.80F, 1.0F}});
  uiLayer.addButtonFlow(34.0F, volt::ui::ButtonElement{"Import", true, false, {0.20F, 0.31F, 0.45F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  bindings->dumpAtlasButtonId = uiLayer.addButtonFlow(
      34.0F,
      volt::ui::ButtonElement{"Dump Atlas", true, false, {0.20F, 0.31F, 0.45F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.addSliderFlow(18.0F, volt::ui::SliderElement{0.0F, 100.0F, 38.0F, {0.17F, 0.19F, 0.22F, 1.0F}, {0.85F, 0.88F, 0.93F, 1.0F}});
  uiLayer.addImageFlow(70.0F, volt::ui::ImageElement{"image:preview-board", {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.addImageFlow(70.0F, volt::ui::ImageElement{"image:codec-render-test", {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.endFlowColumn();
  uiLayer.endPanel();
}

void buildFontInfoPanel(
    UILayer& uiLayer,
    const Rect& bounds,
    const std::string& activeFontLabel,
    const std::string& displayMetricsLabel,
    const std::string& atlasDumpStatus) {
  uiLayer.beginPanel(bounds, PanelElement{"font_info", {0.11F, 0.13F, 0.17F, 1.0F}, 8.0F, false});
  uiLayer.addText(
      {bounds.x + 12.0F, bounds.y + 10.0F, bounds.width - 24.0F, 18.0F},
      TextElement{activeFontLabel, "default", 12.0F, {0.89F, 0.93F, 0.98F, 1.0F}});
  uiLayer.addText(
      {bounds.x + 12.0F, bounds.y + 26.0F, bounds.width - 24.0F, 14.0F},
      TextElement{displayMetricsLabel, "default", 10.0F, {0.80F, 0.86F, 0.94F, 1.0F}});
  uiLayer.addText(
      {bounds.x + 12.0F, bounds.y + 40.0F, bounds.width - 24.0F, 14.0F},
      TextElement{atlasDumpStatus, "default", 10.0F, {0.71F, 0.78F, 0.88F, 1.0F}});
  uiLayer.endPanel();
}

void buildScrollTestCard(UILayer& uiLayer, const Rect& bounds, AppUiState& uiState) {
  uiLayer.beginPanel(bounds, PanelElement{"scroll_test_card", {0.10F, 0.12F, 0.16F, 1.0F}, 8.0F, false});
  addCardHeader(uiLayer, bounds, "Scroll + Clip Test", "Wheel over viewport. Items should clip cleanly.");

  const Rect viewport{bounds.x + 8.0F, bounds.y + 38.0F, bounds.width - 16.0F, bounds.height - 46.0F};
  uiLayer.beginScrollPanel(
      viewport,
      PanelElement{"scroll_test_viewport", {0.08F, 0.10F, 0.13F, 0.92F}, 6.0F},
      280.0F,
      uiState.scrollTestOffsetY);
  uiLayer.beginFlowColumn({viewport.x, viewport.y, viewport.width, 280.0F}, 8.0F, 8.0F);
  uiLayer.addTextFlow(
      20.0F,
      TextElement{"Scroll offset: " + std::to_string(static_cast<int>(std::lround(uiState.scrollTestOffsetY))), "default", 10.0F, {0.84F, 0.90F, 0.97F, 1.0F}});
  uiLayer.addButtonFlow(28.0F, volt::ui::ButtonElement{"Enabled action", true, false, {0.24F, 0.34F, 0.50F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.addButtonFlow(28.0F, volt::ui::ButtonElement{"Disabled action", false, false, {0.24F, 0.34F, 0.50F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.addSliderFlow(14.0F, volt::ui::SliderElement{0.0F, 100.0F, 62.0F, {0.18F, 0.22F, 0.28F, 1.0F}, {0.92F, 0.86F, 0.48F, 1.0F}});
  uiLayer.beginPanel(
      {viewport.x + 8.0F, viewport.y + 88.0F, viewport.width - 16.0F, 24.0F},
      PanelElement{"clip_probe", {0.16F, 0.12F, 0.15F, 1.0F}, 4.0F});
  uiLayer.addText(
      {viewport.x + 12.0F, viewport.y + 90.0F, (viewport.width - 16.0F) * 1.75F, 20.0F},
      TextElement{"This line should clip at the viewport edge.", "default", 11.0F, {0.98F, 0.86F, 0.72F, 1.0F}});
  uiLayer.endPanel();
  uiLayer.addTextFlow(20.0F, TextElement{"Bottom marker", "default", 10.0F, {0.72F, 0.84F, 0.94F, 1.0F}});
  uiLayer.endFlowColumn();
  uiLayer.endScrollPanel();
  uiLayer.endPanel();
}

void buildTypographyCard(UILayer& uiLayer, const Rect& bounds, AppUiState& uiState) {
  uiLayer.beginPanel(bounds, PanelElement{"type_test_card", {0.10F, 0.12F, 0.16F, 1.0F}, 8.0F, false});
  addCardHeader(uiLayer, bounds, "Typography Test", "Check baseline, size changes, and clipping.");

  const Rect viewport{bounds.x + 8.0F, bounds.y + 38.0F, bounds.width - 16.0F, bounds.height - 46.0F};
  uiLayer.beginScrollPanel(
      viewport,
      PanelElement{"type_test_viewport", {0.08F, 0.10F, 0.13F, 0.92F}, 6.0F},
      236.0F,
      uiState.typographyTestOffsetY);
  uiLayer.beginFlowColumn({viewport.x, viewport.y, viewport.width, 236.0F}, 6.0F, 8.0F);
  uiLayer.addTextFlow(16.0F, TextElement{"10 px  The quick brown fox", "default", 10.0F, {0.84F, 0.90F, 0.97F, 1.0F}});
  uiLayer.addTextFlow(18.0F, TextElement{"12 px  The quick brown fox", "default", 12.0F, {0.84F, 0.90F, 0.97F, 1.0F}});
  uiLayer.addTextFlow(20.0F, TextElement{"14 px  The quick brown fox", "default", 14.0F, {0.84F, 0.90F, 0.97F, 1.0F}});
  uiLayer.addTextFlow(24.0F, TextElement{"18 px  Sphinx of black quartz", "default", 18.0F, {0.96F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addTextFlow(30.0F, TextElement{"24 px  Oa  RGB 123", "default", 24.0F, {0.96F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addTextFlow(38.0F, TextElement{"32 px  Volt", "default", 32.0F, {0.98F, 0.92F, 0.74F, 1.0F}});
  uiLayer.beginPanel(
      {viewport.x + 8.0F, viewport.y + 152.0F, viewport.width - 16.0F, 26.0F},
      PanelElement{"type_clip_probe", {0.13F, 0.16F, 0.20F, 1.0F}, 4.0F});
  uiLayer.addText(
      {viewport.x + 12.0F, viewport.y + 152.0F, (viewport.width - 16.0F) * 1.6F, 24.0F},
      TextElement{"Large clipped text sample", "default", 20.0F, {0.96F, 0.90F, 0.80F, 1.0F}});
  uiLayer.endPanel();
  uiLayer.endFlowColumn();
  uiLayer.endScrollPanel();
  uiLayer.endPanel();
}

void buildControlsCard(UILayer& uiLayer, const Rect& bounds, AppUiState& uiState) {
  uiLayer.beginPanel(bounds, PanelElement{"controls_test_card", {0.10F, 0.12F, 0.16F, 1.0F}, 8.0F, false});
  addCardHeader(uiLayer, bounds, "Controls Test", "Verify click state, toggle visuals, and focused text entry.");
  uiLayer.beginFlowColumn({bounds.x + 8.0F, bounds.y + 38.0F, bounds.width - 16.0F, bounds.height - 46.0F}, 8.0F, 8.0F);
  uiLayer.addCheckboxFlow(
      28.0F,
      volt::ui::CheckboxElement{"Show clip guides", &uiState.smokeCheckboxValue, true, {0.17F, 0.19F, 0.24F, 1.0F}, {0.90F, 0.82F, 0.50F, 1.0F}, {0.94F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addToggleFlow(
      28.0F,
      volt::ui::ToggleElement{"GPU atlas path", &uiState.smokeToggleValue, true, {0.22F, 0.24F, 0.28F, 1.0F}, {0.22F, 0.48F, 0.72F, 1.0F}, {0.96F, 0.97F, 1.0F, 1.0F}, {0.94F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addTextInputFlow(
      32.0F,
      volt::ui::TextInputElement{&uiState.smokeInputValue, "Type a smoke-test label", 40U, true});
  uiLayer.addTextFlow(
      18.0F,
      TextElement{
          std::string("Checkbox=") + (uiState.smokeCheckboxValue ? "on" : "off") +
              "  Toggle=" + (uiState.smokeToggleValue ? "on" : "off") +
              "  Input='" + uiState.smokeInputValue + "'",
          "default",
          10.0F,
          {0.76F, 0.84F, 0.92F, 1.0F}});
  uiLayer.endFlowColumn();
  uiLayer.endPanel();
}

void buildScaffoldCard(UILayer& uiLayer, const Rect& bounds, const std::string& title, const std::string& subtitle) {
  uiLayer.beginPanel(bounds, PanelElement{"scaffold_card", {0.09F, 0.11F, 0.14F, 1.0F}, 10.0F, false});
  addCardHeader(uiLayer, bounds, title, subtitle);
  uiLayer.endPanel();
}

void buildLayoutDemoCard(UILayer& uiLayer, const Rect& bounds, AppUiState& uiState) {
  uiLayer.beginPanel(bounds, PanelElement{"layout_demo_card", {0.09F, 0.11F, 0.14F, 1.0F}, 10.0F, false});
  uiLayer.beginDock(bounds, 10.0F, 12.0F);
  const Rect headerRect = uiLayer.dockTopRect(34.0F);
  const Rect sidebarRect = uiLayer.dockLeftRect(184.0F);
  const Rect contentRect = uiLayer.dockFillRect();
  uiLayer.endDock();

  uiLayer.addText(
      {headerRect.x + 4.0F, headerRect.y, headerRect.width - 8.0F, headerRect.height},
      TextElement{"Layout Containers", "default", 14.0F, {0.95F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addText(
      {headerRect.x + 180.0F, headerRect.y, std::max(0.0F, headerRect.width - 184.0F), headerRect.height},
      TextElement{"Stack sidebar, docked header, and grid content share the same card.", "default", 10.0F, {0.68F, 0.76F, 0.86F, 1.0F}});

  uiLayer.beginStack(sidebarRect, volt::ui::StackAxis::kVertical, 8.0F, 0.0F);
  uiLayer.addButton(
      uiLayer.nextStackRect(28.0F),
      volt::ui::ButtonElement{"Primary action", true, false, {0.20F, 0.31F, 0.45F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.addCheckbox(
      uiLayer.nextStackRect(28.0F),
      volt::ui::CheckboxElement{"Use layout guides", &uiState.smokeCheckboxValue, true, {0.17F, 0.19F, 0.24F, 1.0F}, {0.90F, 0.82F, 0.50F, 1.0F}, {0.94F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addToggle(
      uiLayer.nextStackRect(28.0F),
      volt::ui::ToggleElement{"Dock preview", &uiState.smokeToggleValue, true, {0.22F, 0.24F, 0.28F, 1.0F}, {0.22F, 0.48F, 0.72F, 1.0F}, {0.96F, 0.97F, 1.0F, 1.0F}, {0.94F, 0.97F, 1.0F, 1.0F}});
  uiLayer.addTextInput(
      uiLayer.nextStackRect(32.0F),
      volt::ui::TextInputElement{&uiState.smokeInputValue, "Layout note", 40U, true});
  uiLayer.addText(
      uiLayer.nextStackRect(22.0F),
      TextElement{"Sidebar uses the new stack container.", "default", 10.0F, {0.72F, 0.80F, 0.90F, 1.0F}});
  uiLayer.endStack();

  uiLayer.beginGrid(contentRect, 3U, 44.0F, 10.0F, 10.0F, 0.0F);
  const Rect metricRect = uiLayer.nextGridRect(1U);
  const Rect spanRect = uiLayer.nextGridRect(2U);
  const Rect infoRect = uiLayer.nextGridRect(1U);
  const Rect actionsRect = uiLayer.nextGridRect(1U);
  const Rect previewRect = uiLayer.nextGridRect(1U);
  const Rect summaryRect = uiLayer.nextGridRect(1U);
  uiLayer.endGrid();

  const auto addTile = [&](const Rect& tileBounds, const std::string& title, const std::string& body, const Color& background) {
    uiLayer.addPanel(tileBounds, PanelElement{"layout_tile", background, 8.0F});
    uiLayer.addText(
        {tileBounds.x + 8.0F, tileBounds.y + 6.0F, tileBounds.width - 16.0F, 16.0F},
        TextElement{title, "default", 11.0F, {0.96F, 0.98F, 1.0F, 1.0F}});
    uiLayer.addText(
        {tileBounds.x + 8.0F, tileBounds.y + 22.0F, tileBounds.width - 16.0F, tileBounds.height - 28.0F},
        TextElement{body, "default", 9.0F, {0.76F, 0.84F, 0.92F, 1.0F}});
  };

  addTile(metricRect, "Dock Header", "The top strip was carved out first, then the sidebar, then the fill area.", {0.12F, 0.16F, 0.22F, 1.0F});
  addTile(spanRect, "Grid Span", "This tile spans two columns to show larger content blocks without hard-coded widths.", {0.14F, 0.18F, 0.24F, 1.0F});
  addTile(infoRect, "Cell 1", "Grid cells can host regular panels, text, and controls.", {0.11F, 0.15F, 0.20F, 1.0F});
  addTile(actionsRect, "Cell 2", std::string("Checkbox=") + (uiState.smokeCheckboxValue ? "on" : "off"), {0.11F, 0.15F, 0.20F, 1.0F});
  addTile(previewRect, "Cell 3", std::string("Toggle=") + (uiState.smokeToggleValue ? "on" : "off"), {0.11F, 0.15F, 0.20F, 1.0F});
  addTile(summaryRect, "Input", uiState.smokeInputValue.empty() ? std::string("Type in the sidebar field.") : uiState.smokeInputValue, {0.11F, 0.15F, 0.20F, 1.0F});
  uiLayer.endPanel();
}

}  // namespace

AppUiFrameBindings buildAppUi(
    UILayer& uiLayer,
    const Window& window,
    const UILayer::FrameArgs& frameArgs,
    const AppUiFrameStats& frameStats,
    AppUiState& uiState) {
  AppUiFrameBindings bindings{};

  const std::string activeFontLabel = buildActiveFontLabel();
  const std::string displayMetricsLabel = buildDisplayMetricsLabel(window);
  const std::string frameTimingLabel = buildFrameTimingLabel(frameStats);
  const std::string frameOverlayHeadline = buildFrameOverlayHeadline(frameStats);
  const std::string frameOverlayLoopLabel = buildFrameOverlayLoopLabel(frameStats);
  const std::string frameOverlayStageLabel = buildFrameOverlayStageLabel(frameStats);
  const std::string frameOverlayVectorTextLabel = buildFrameOverlayVectorTextLabel(frameStats);
  const std::string frameOverlayRendererSyncLabel = buildFrameOverlayRendererSyncLabel(frameStats);

  uiLayer.beginFrame(window.inputSnapshot(), frameArgs);

  const auto chromeLayout = window.buildChromeLayout(frameArgs.width, frameArgs.height);
  const float contentTop = chromeLayout.titleBar.height + chromeLayout.titleBarPadding;
  const float logicalWidthF = static_cast<float>(frameArgs.width);

  const PanelElement titleBarPanel{"window_titlebar", {0.09F, 0.11F, 0.14F, 1.0F}, 0.0F};
  const PanelElement hitRegionPanel{"window_hit_region", {0.0F, 0.0F, 0.0F, 0.0F}, 0.0F};

  uiLayer.addPanel(
      {chromeLayout.titleBar.x, chromeLayout.titleBar.y, chromeLayout.titleBar.width, chromeLayout.titleBar.height},
      titleBarPanel);
  uiLayer.addImage(
      {chromeLayout.titleIcon.x, chromeLayout.titleIcon.y, chromeLayout.titleIcon.width, chromeLayout.titleIcon.height},
      volt::ui::ImageElement{window.titleIconTextureId(), {1.0F, 1.0F, 1.0F, 1.0F}});
  uiLayer.addText(
      {chromeLayout.titleText.x, chromeLayout.titleTextBaselineY, chromeLayout.titleText.width, chromeLayout.titleText.height},
      TextElement{window.title(), "default", 14.0F, {0.92F, 0.94F, 0.98F, 1.0F}});

  bindings.chromeWidgetIds.dragRegion = uiLayer.addPanel(
      {chromeLayout.dragRegion.x, chromeLayout.dragRegion.y, chromeLayout.dragRegion.width, chromeLayout.dragRegion.height},
      hitRegionPanel);
  bindings.chromeWidgetIds.minimizeButton = uiLayer.addButton(
      {chromeLayout.minimizeButton.x, chromeLayout.minimizeButton.y, chromeLayout.minimizeButton.width, chromeLayout.minimizeButton.height},
      volt::ui::ButtonElement{"", true, false, {0.12F, 0.15F, 0.20F, 1.0F}, {0.86F, 0.89F, 0.94F, 1.0F}});
  bindings.chromeWidgetIds.maximizeButton = uiLayer.addButton(
      {chromeLayout.maximizeButton.x, chromeLayout.maximizeButton.y, chromeLayout.maximizeButton.width, chromeLayout.maximizeButton.height},
      volt::ui::ButtonElement{"", true, false, {0.12F, 0.15F, 0.20F, 1.0F}, {0.86F, 0.89F, 0.94F, 1.0F}});
  bindings.chromeWidgetIds.closeButton = uiLayer.addButton(
      {chromeLayout.closeButton.x, chromeLayout.closeButton.y, chromeLayout.closeButton.width, chromeLayout.closeButton.height},
      volt::ui::ButtonElement{"", true, false, {0.48F, 0.14F, 0.18F, 1.0F}, {0.97F, 0.97F, 0.98F, 1.0F}});

  const float buttonTextInset = chromeLayout.titleBarPadding;
  uiLayer.addText(
      {chromeLayout.minimizeButton.x + buttonTextInset, chromeLayout.buttonTextBaselineY, std::max(0.0F, chromeLayout.minimizeButton.width - (buttonTextInset * 2.0F)), 14.0F},
      TextElement{std::string(window.minimizeLabel()), "default", 12.0F, {0.86F, 0.89F, 0.94F, 1.0F}});
  uiLayer.addText(
      {chromeLayout.maximizeButton.x + buttonTextInset, chromeLayout.buttonTextBaselineY, std::max(0.0F, chromeLayout.maximizeButton.width - (buttonTextInset * 2.0F)), 14.0F},
      TextElement{std::string(window.maximizeLabel()), "default", 12.0F, {0.86F, 0.89F, 0.94F, 1.0F}});
  uiLayer.addText(
      {chromeLayout.closeButton.x + buttonTextInset, chromeLayout.buttonTextBaselineY, std::max(0.0F, chromeLayout.closeButton.width - (buttonTextInset * 2.0F)), 14.0F},
      TextElement{std::string(window.closeLabel()), "default", 12.0F, {0.97F, 0.97F, 0.98F, 1.0F}});

  if (chromeLayout.hasResizeRegions) {
    bindings.chromeWidgetIds.leftResize = uiLayer.addPanel(
        {chromeLayout.leftResize.x, chromeLayout.leftResize.y, chromeLayout.leftResize.width, chromeLayout.leftResize.height},
        hitRegionPanel);
    bindings.chromeWidgetIds.rightResize = uiLayer.addPanel(
        {chromeLayout.rightResize.x, chromeLayout.rightResize.y, chromeLayout.rightResize.width, chromeLayout.rightResize.height},
        hitRegionPanel);
    bindings.chromeWidgetIds.topResize = uiLayer.addPanel(
        {chromeLayout.topResize.x, chromeLayout.topResize.y, chromeLayout.topResize.width, chromeLayout.topResize.height},
        hitRegionPanel);
    bindings.chromeWidgetIds.bottomResize = uiLayer.addPanel(
        {chromeLayout.bottomResize.x, chromeLayout.bottomResize.y, chromeLayout.bottomResize.width, chromeLayout.bottomResize.height},
        hitRegionPanel);
    bindings.chromeWidgetIds.topLeftResize = uiLayer.addPanel(
        {chromeLayout.topLeftResize.x, chromeLayout.topLeftResize.y, chromeLayout.topLeftResize.width, chromeLayout.topLeftResize.height},
        hitRegionPanel);
    bindings.chromeWidgetIds.topRightResize = uiLayer.addPanel(
        {chromeLayout.topRightResize.x, chromeLayout.topRightResize.y, chromeLayout.topRightResize.width, chromeLayout.topRightResize.height},
        hitRegionPanel);
    bindings.chromeWidgetIds.bottomLeftResize = uiLayer.addPanel(
        {chromeLayout.bottomLeftResize.x, chromeLayout.bottomLeftResize.y, chromeLayout.bottomLeftResize.width, chromeLayout.bottomLeftResize.height},
        hitRegionPanel);
    bindings.chromeWidgetIds.bottomRightResize = uiLayer.addPanel(
        {chromeLayout.bottomRightResize.x, chromeLayout.bottomRightResize.y, chromeLayout.bottomRightResize.width, chromeLayout.bottomRightResize.height},
        hitRegionPanel);
  }

  const Rect contentBounds{16.0F, contentTop, std::max(0.0F, logicalWidthF - 32.0F), std::max(0.0F, static_cast<float>(frameArgs.height) - contentTop - 16.0F)};
  uiLayer.beginDock(contentBounds, 12.0F, 0.0F);
  const Rect leftPanelRect = uiLayer.dockLeftRect(340.0F);
  const Rect fontInfoRect = uiLayer.dockTopRect(64.0F);
  const Rect bodyRect = uiLayer.dockFillRect();
  uiLayer.endDock();

  buildSidebarPanel(uiLayer, leftPanelRect, &bindings);
  buildFontInfoPanel(uiLayer, fontInfoRect, activeFontLabel, displayMetricsLabel, uiState.atlasDumpStatus);

  uiLayer.beginGrid(bodyRect, 2U, 170.0F, 12.0F, 12.0F, 0.0F);
  const Rect scrollCardRect = uiLayer.nextGridRect(1U, 170.0F);
  const Rect typographyCardRect = uiLayer.nextGridRect(1U, 170.0F);
  const Rect controlsCardRect = uiLayer.nextGridRect(2U, 148.0F);
  const Rect chartCardRect = uiLayer.nextGridRect(1U, 160.0F);
  const Rect schematicCardRect = uiLayer.nextGridRect(1U, 220.0F);
  const Rect layoutDemoRect = uiLayer.nextGridRect(2U, 220.0F);
  uiLayer.endGrid();

  buildScrollTestCard(uiLayer, scrollCardRect, uiState);
  buildTypographyCard(uiLayer, typographyCardRect, uiState);
  buildControlsCard(uiLayer, controlsCardRect, uiState);

  buildScaffoldCard(uiLayer, chartCardRect, "Chart Scaffold", "Placeholder chart content inside a grid cell.");
  uiLayer.addChartScaffold(
      insetRect({chartCardRect.x + 0.0F, chartCardRect.y + 36.0F, chartCardRect.width, chartCardRect.height - 36.0F}, 8.0F),
      volt::ui::ChartScaffoldElement{"line", {0.2F, 0.5F, 0.7F, 0.45F, 0.9F}});

  buildScaffoldCard(uiLayer, schematicCardRect, "Schematic Scaffold", "Placeholder schematic content inside a grid cell.");
  uiLayer.addSchematicScaffold(
      insetRect({schematicCardRect.x + 0.0F, schematicCardRect.y + 36.0F, schematicCardRect.width, schematicCardRect.height - 36.0F}, 8.0F),
      volt::ui::SchematicScaffoldElement{"power_stage_topology", 24U, 38U});

  buildLayoutDemoCard(uiLayer, layoutDemoRect, uiState);

  constexpr float kOverlayMargin = 16.0F;
  constexpr float kOverlayButtonWidth = 104.0F;
  constexpr float kOverlayButtonHeight = 28.0F;
  constexpr float kOverlayPanelWidth = 280.0F;
  constexpr float kOverlayPanelHeight = 114.0F;
  const float overlayButtonX = std::max(kOverlayMargin, logicalWidthF - kOverlayButtonWidth - kOverlayMargin);
  const float overlayButtonY = contentTop;
  bindings.frameOverlayToggleId = uiLayer.addButton(
      {overlayButtonX, overlayButtonY, kOverlayButtonWidth, kOverlayButtonHeight},
      volt::ui::ButtonElement{
          uiState.frameOverlayVisible ? "Hide Perf" : "Show Perf",
          true,
          false,
          {0.16F, 0.22F, 0.30F, 0.96F},
          {0.96F, 0.98F, 1.0F, 1.0F}});

  if (uiState.frameOverlayVisible) {
    const float overlayPanelX = std::max(kOverlayMargin, logicalWidthF - kOverlayPanelWidth - kOverlayMargin);
    const float overlayPanelY = overlayButtonY + kOverlayButtonHeight + 8.0F;
    uiLayer.addPanel(
        {overlayPanelX, overlayPanelY, kOverlayPanelWidth, kOverlayPanelHeight},
        PanelElement{"frame_overlay", {0.10F, 0.13F, 0.17F, 0.96F}, 8.0F});
    uiLayer.addText(
        {overlayPanelX + 12.0F, overlayPanelY + 10.0F, kOverlayPanelWidth - 24.0F, 18.0F},
        TextElement{frameOverlayHeadline, "default", 12.0F, {0.96F, 0.98F, 1.0F, 1.0F}});
    uiLayer.addText(
        {overlayPanelX + 12.0F, overlayPanelY + 30.0F, kOverlayPanelWidth - 24.0F, 14.0F},
        TextElement{frameOverlayLoopLabel, "default", 10.0F, {0.78F, 0.84F, 0.92F, 1.0F}});
    uiLayer.addText(
        {overlayPanelX + 12.0F, overlayPanelY + 46.0F, kOverlayPanelWidth - 24.0F, 14.0F},
        TextElement{frameOverlayStageLabel, "default", 10.0F, {0.78F, 0.84F, 0.92F, 1.0F}});
    uiLayer.addText(
      {overlayPanelX + 12.0F, overlayPanelY + 62.0F, kOverlayPanelWidth - 24.0F, 14.0F},
      TextElement{frameOverlayVectorTextLabel, "default", 9.0F, {0.70F, 0.78F, 0.88F, 1.0F}});
    uiLayer.addText(
      {overlayPanelX + 12.0F, overlayPanelY + 78.0F, kOverlayPanelWidth - 24.0F, 14.0F},
      TextElement{frameOverlayRendererSyncLabel, "default", 9.0F, {0.70F, 0.78F, 0.88F, 1.0F}});
    uiLayer.addText(
      {overlayPanelX + 12.0F, overlayPanelY + 94.0F, kOverlayPanelWidth - 24.0F, 14.0F},
        TextElement{frameTimingLabel, "default", 9.0F, {0.60F, 0.68F, 0.78F, 1.0F}});
  }

  return bindings;
}

bool handleAppUiInteractions(
    Window& window,
    const AppUiFrameBindings& bindings,
    AppUiState& uiState,
    std::uint64_t hoveredWidgetId,
    bool leftMousePressed) {
  bool stateChanged = false;

  if (leftMousePressed && hoveredWidgetId == bindings.dumpAtlasButtonId) {
    std::string dumpPath;
    std::string dumpError;
    if (dumpDefaultFontAtlasImage(dumpPath, dumpError)) {
      uiState.atlasDumpStatus = "Atlas dump: " + dumpPath;
      stateChanged = true;
      VOLT_LOG_INFO_CAT(
          volt::core::logging::Category::kIO,
          "Font atlas dump written to ",
          dumpPath);
    } else {
      uiState.atlasDumpStatus = "Atlas dump failed: " + dumpError;
      stateChanged = true;
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Font atlas dump failed: ",
          dumpError);
    }
  }

  if (leftMousePressed && hoveredWidgetId == bindings.frameOverlayToggleId) {
    uiState.frameOverlayVisible = !uiState.frameOverlayVisible;
    stateChanged = true;
  }

  window.handleChromePointerPress(leftMousePressed, hoveredWidgetId, bindings.chromeWidgetIds);
  return stateChanged;
}

}  // namespace volt::app
