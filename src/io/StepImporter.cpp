#include "volt/io/StepImporter.hpp"

namespace volt::io {

bool StepImporter::supportsExtension(std::string_view extension) const {
  return extension == ".step" || extension == ".stp";
}

ImportResult StepImporter::importFile(const ImportRequest& request) const {
  return {
      .success = false,
      .message = "STEP importer not implemented yet for: " + request.path.string(),
      .detectedFormat = ModelFormat::kStep,
      .issues = {{
          .severity = IssueSeverity::kWarning,
          .code = "IO_STEP_NOT_IMPLEMENTED",
          .message = "STEP parser pipeline is not implemented yet.",
      }},
  };
}

}  // namespace volt::io
