#pragma once

#include "volt/io/IModelImporter.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace volt::io {

class ImporterRegistry {
 public:
  void registerImporter(std::unique_ptr<IModelImporter> importer);
  void registerDefaultImporters();

  [[nodiscard]] const IModelImporter* findForPath(const std::filesystem::path& path) const;

  [[nodiscard]] ImportResult import(const ImportRequest& request) const;

  [[nodiscard]] ImportResult import(
      const std::filesystem::path& path,
      const ImportOptions& options = {}) const;

 private:
  std::vector<std::unique_ptr<IModelImporter>> importers_;
};

}  // namespace volt::io
