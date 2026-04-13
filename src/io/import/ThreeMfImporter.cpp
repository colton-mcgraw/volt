#include "volt/io/import/ThreeMfImporter.hpp"

namespace volt::io {

bool ThreeMfImporter::supportsExtension(std::string_view extension) const {
  return extension == ".3mf";
}

ImportResult ThreeMfImporter::importFile(const ImportRequest& request) const {
  return {
      .success = false,
      .message = "3MF importer not implemented yet for: " + request.path.string(),
      .detectedFormat = ModelFormat::kThreeMf,
      .issues = {{
          .severity = IssueSeverity::kWarning,
          .code = "IO_3MF_NOT_IMPLEMENTED",
          .message = "3MF parser pipeline is not implemented yet.",
      }},
  };
}

}  // namespace volt::io
