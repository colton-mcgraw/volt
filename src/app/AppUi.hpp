#pragma once

#include "volt/platform/Window.hpp"
#include "volt/ui/UILayer.hpp"

#include <cstdint>
#include <string>

namespace volt::app {

struct AppUiState {
  bool frameOverlayVisible{true};
  float scrollTestOffsetY{0.0F};
  float typographyTestOffsetY{0.0F};
  bool smokeCheckboxValue{true};
  bool smokeToggleValue{false};
  std::string smokeInputValue{"Atlas QA"};
  std::string atlasDumpStatus{"Atlas dump: idle"};
};

struct AppUiFrameStats {
  std::int32_t loopMs{0};
  std::int32_t waitMs{0};
  std::int32_t uiMs{0};
  std::int32_t rendererMs{0};
  std::int32_t renderCpuMs{0};
  bool rendererSyncValid{false};
  std::int32_t rendererFenceWaitMs{0};
  std::int32_t rendererAcquireMs{0};
  std::int32_t rendererRecordMs{0};
  std::int32_t rendererRecordUploadMs{0};
  std::int32_t rendererRecordDrawMs{0};
  std::int32_t rendererUiVectorTextPrepMs{0};
  std::int32_t rendererSubmitMs{0};
  std::int32_t rendererPresentMs{0};
  bool vectorTextGpuValid{false};
  std::int32_t vectorTextFlattenCountMs{0};
  std::int32_t vectorTextCurveScanMs{0};
  std::int32_t vectorTextFlattenEmitBinCountMs{0};
  std::int32_t vectorTextTileScanMs{0};
  std::int32_t vectorTextBinEmitFineMs{0};
  std::int32_t uiDrawCallCount{0};
  double uiDrawCallAvgMs{0.0};
  double uiDrawCallMaxMs{0.0};
  std::int32_t uiDrawCallMaxBatchIndex{-1};
  std::string uiDrawCallMaxTextureKey;
  double uiDescriptorResolveTotalMs{0.0};
  double uiDescriptorResolveMaxMs{0.0};
  std::string uiDescriptorResolveMaxTextureKey;
};

struct AppUiFrameBindings {
  volt::platform::WindowChromeWidgetIds chromeWidgetIds{};
  std::uint64_t dumpAtlasButtonId{0};
  std::uint64_t frameOverlayToggleId{0};
};

[[nodiscard]] AppUiFrameBindings buildAppUi(
    volt::ui::UILayer& uiLayer,
    const volt::platform::Window& window,
    const volt::ui::UILayer::FrameArgs& frameArgs,
    const AppUiFrameStats& frameStats,
    AppUiState& uiState);

[[nodiscard]] bool handleAppUiInteractions(
    volt::platform::Window& window,
    const AppUiFrameBindings& bindings,
    AppUiState& uiState,
    std::uint64_t hoveredWidgetId,
    bool leftMousePressed);

}  // namespace volt::app
