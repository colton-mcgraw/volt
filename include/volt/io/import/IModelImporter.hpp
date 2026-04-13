#pragma once

#include "volt/io/import/ImportTypes.hpp"

#include <string_view>

namespace volt::io {

class IModelImporter {
 public:
  virtual ~IModelImporter() = default;

  [[nodiscard]] virtual bool supportsExtension(std::string_view extension) const = 0;

  [[nodiscard]] virtual ImportResult importFile(const ImportRequest& request) const = 0;

  [[nodiscard]] ImportResult importFile(const std::filesystem::path& path) const {
    return importFile(ImportRequest{.path = path});
  }
};

}  // namespace volt::io
