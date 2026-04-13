#pragma once

#include "volt/io/import/IModelImporter.hpp"

namespace volt::io {

class StepImporter final : public IModelImporter {
 public:
  [[nodiscard]] bool supportsExtension(std::string_view extension) const override;
  [[nodiscard]] ImportResult importFile(const ImportRequest& request) const override;
};

}  // namespace volt::io
