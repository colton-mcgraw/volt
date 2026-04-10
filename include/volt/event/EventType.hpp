#pragma once

namespace volt::event {

enum class EventType {
  kUnknown = 0,
  kFrameStarted,
  kFrameEnded,
  kWindowResized,
  kWindowMinimized,
  kKeyInput,
  kMouseMoved,
  kMouseButton,
  kMouseScrolled,
  kRenderFrameBegin,
  kRenderScenePassBegin,
  kRenderScenePassEnd,
  kRenderUiPass,
  kRenderFrameEnd,
  kUiFrameBegin,
  kUiLayoutPass,
  kUiPaintPass,
  kUiFrameEnd,
  kImportStarted,
  kImportStageComplete,
  kImportSucceeded,
  kImportFailed,
};

enum class RenderStage {
  kFrameBegin,
  kScenePassBegin,
  kScenePassEnd,
  kUiPass,
  kFrameEnd,
};

enum class UiStage {
  kFrameBegin,
  kLayoutPass,
  kPaintPass,
  kFrameEnd,
};

enum class ImportStage {
  kStart,
  kProbe,
  kParse,
  kNormalize,
  kValidate,
  kSuccess,
  kFailure,
};

}  // namespace volt::event
