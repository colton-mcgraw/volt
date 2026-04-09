#include "volt/io/ImportPipeline.hpp"

namespace volt::io {

ImportResult ImportPipeline::run(const ImportRequest& request, const IModelImporter& importer) const {
  ImportResult probeResult = runProbeStage(request, importer);
  if (!probeResult.success) {
    return probeResult;
  }

  ImportResult result = runParseStage(request, importer);
  runNormalizeStage(result, request);
  runValidateStage(result);
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
