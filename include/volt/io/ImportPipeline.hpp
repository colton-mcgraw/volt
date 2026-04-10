#pragma once

#include "volt/io/IModelImporter.hpp"

namespace volt::event {
class EventDispatcher;
}

namespace volt::io {

class ImportPipeline {
 public:
  void setEventDispatcher(volt::event::EventDispatcher* dispatcher);
  [[nodiscard]] ImportResult run(const ImportRequest& request, const IModelImporter& importer) const;

 private:
  [[nodiscard]] ImportResult runProbeStage(const ImportRequest& request, const IModelImporter& importer) const;
  [[nodiscard]] ImportResult runParseStage(const ImportRequest& request, const IModelImporter& importer) const;
  [[nodiscard]] void runNormalizeStage(ImportResult& result, const ImportRequest& request) const;
  [[nodiscard]] void runValidateStage(ImportResult& result) const;

  mutable volt::event::EventDispatcher* eventDispatcher_{nullptr};
};

}  // namespace volt::io
