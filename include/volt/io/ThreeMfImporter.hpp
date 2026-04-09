#pragma once

#include "volt/io/IModelImporter.hpp"

namespace volt::io {

class ThreeMfImporter final : public IModelImporter {
 public:
  [[nodiscard]] bool supportsExtension(std::string_view extension) const override;
  [[nodiscard]] ImportResult importFile(const ImportRequest& request) const override;
};

}  // namespace volt::io
