#include "volt/io/ImporterRegistry.hpp"

#include "volt/io/ImportPipeline.hpp"
#include "volt/io/StepImporter.hpp"
#include "volt/io/ThreeMfImporter.hpp"

#include <algorithm>
#include <cctype>

namespace volt::io {

namespace {
std::string normalizeExtension(std::string ext) {
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return ext;
}
}  // namespace

void ImporterRegistry::registerImporter(std::unique_ptr<IModelImporter> importer) {
  importers_.push_back(std::move(importer));
}

void ImporterRegistry::registerDefaultImporters() {
  registerImporter(std::make_unique<StepImporter>());
  registerImporter(std::make_unique<ThreeMfImporter>());
}

const IModelImporter* ImporterRegistry::findForPath(const std::filesystem::path& path) const {
  const std::string ext = normalizeExtension(path.extension().string());
  for (const auto& importer : importers_) {
    if (importer->supportsExtension(ext)) {
      return importer.get();
    }
  }
  return nullptr;
}

ImportResult ImporterRegistry::import(const ImportRequest& request) const {
  const IModelImporter* importer = findForPath(request.path);
  if (importer == nullptr) {
    return {
        .success = false,
        .message = "No importer available for extension: " + request.path.extension().string(),
        .detectedFormat = ModelFormat::kUnknown,
        .issues = {{
            .severity = IssueSeverity::kError,
            .code = "IO_UNSUPPORTED_EXTENSION",
            .message = "No importer registered for extension: " + request.path.extension().string(),
        }},
    };
  }

  const ImportPipeline pipeline{};
  return pipeline.run(request, *importer);
}

ImportResult ImporterRegistry::import(
    const std::filesystem::path& path,
    const ImportOptions& options) const {
  return import(ImportRequest{
      .path = path,
      .options = options,
  });
}

}  // namespace volt::io
