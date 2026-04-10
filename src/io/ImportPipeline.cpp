#include "volt/io/ImportPipeline.hpp"

#include "volt/core/Logging.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"

namespace volt::io {

void ImportPipeline::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  eventDispatcher_ = dispatcher;
}

ImportResult ImportPipeline::run(const ImportRequest& request, const IModelImporter& importer) const {
  VOLT_LOG_DEBUG_CAT(volt::core::logging::Category::kIO, "Import start: ", request.path.string());

  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kImportStarted,
        .payload = volt::event::ImportLifecycleEvent{
            .stage = volt::event::ImportStage::kStart,
            .path = request.path.string(),
            .success = false,
        },
    });
  }

  ImportResult probeResult = runProbeStage(request, importer);
  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kImportStageComplete,
        .payload = volt::event::ImportLifecycleEvent{
            .stage = volt::event::ImportStage::kProbe,
            .path = request.path.string(),
            .success = probeResult.success,
        },
    });
  }

  if (!probeResult.success) {
    VOLT_LOG_WARN_CAT(volt::core::logging::Category::kIO, "Import probe failed: ", probeResult.message);

    if (eventDispatcher_ != nullptr) {
      eventDispatcher_->enqueue({
          .type = volt::event::EventType::kImportFailed,
          .payload = volt::event::ImportLifecycleEvent{
              .stage = volt::event::ImportStage::kFailure,
              .path = request.path.string(),
              .success = false,
          },
      });
    }

    return probeResult;
  }

  ImportResult result = runParseStage(request, importer);
  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kImportStageComplete,
        .payload = volt::event::ImportLifecycleEvent{
            .stage = volt::event::ImportStage::kParse,
            .path = request.path.string(),
            .success = result.success,
        },
    });
  }

  runNormalizeStage(result, request);
  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kImportStageComplete,
        .payload = volt::event::ImportLifecycleEvent{
            .stage = volt::event::ImportStage::kNormalize,
            .path = request.path.string(),
            .success = result.success,
        },
    });
  }

  runValidateStage(result);
  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = volt::event::EventType::kImportStageComplete,
        .payload = volt::event::ImportLifecycleEvent{
            .stage = volt::event::ImportStage::kValidate,
            .path = request.path.string(),
            .success = result.success,
        },
    });
  }

  if (eventDispatcher_ != nullptr) {
    eventDispatcher_->enqueue({
        .type = result.success ? volt::event::EventType::kImportSucceeded
                               : volt::event::EventType::kImportFailed,
        .payload = volt::event::ImportLifecycleEvent{
            .stage = result.success ? volt::event::ImportStage::kSuccess
                                    : volt::event::ImportStage::kFailure,
            .path = request.path.string(),
            .success = result.success,
        },
    });
  }

  if (result.success) {
    VOLT_LOG_INFO_CAT(volt::core::logging::Category::kIO, "Import succeeded: ", request.path.string());
  } else {
    VOLT_LOG_ERROR_CAT(volt::core::logging::Category::kIO, "Import failed: ", result.message);
  }

  return result;
}

ImportResult ImportPipeline::runProbeStage(
    const ImportRequest& request,
    const IModelImporter& importer) const {
  const std::string ext = request.path.extension().string();
  if (!importer.supportsExtension(ext)) {
    return {
        .success = false,
        .message = "Importer does not support extension: " + ext,
        .detectedFormat = ModelFormat::kUnknown,
        .issues = {{
            .severity = IssueSeverity::kError,
            .code = "IO_PIPELINE_PROBE_UNSUPPORTED",
            .message = "Probe stage rejected extension: " + ext,
        }},
    };
  }

  return {
      .success = true,
      .message = "Probe stage succeeded.",
  };
}

ImportResult ImportPipeline::runParseStage(
    const ImportRequest& request,
    const IModelImporter& importer) const {
  return importer.importFile(request);
}

void ImportPipeline::runNormalizeStage(ImportResult& result, const ImportRequest& request) const {
  (void)request;

  if (!result.success) {
    return;
  }

  result.issues.push_back({
      .severity = IssueSeverity::kInfo,
      .code = "IO_PIPELINE_NORMALIZE_SKIPPED",
      .message = "Normalize stage is currently a pass-through.",
  });
}

void ImportPipeline::runValidateStage(ImportResult& result) const {
  if (!result.success) {
    return;
  }

  result.issues.push_back({
      .severity = IssueSeverity::kInfo,
      .code = "IO_PIPELINE_VALIDATE_SKIPPED",
      .message = "Validate stage is currently a pass-through.",
  });
}

}  // namespace volt::io
