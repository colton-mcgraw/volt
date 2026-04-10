#pragma once

#include "volt/ui/UIRenderTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace volt::ui {

enum class WidgetKind {
  kText,
  kButton,
  kSlider,
  kIcon,
  kImage,
  kPanel,
  kChartScaffold,
  kSchematicScaffold,
};

struct PanelElement {
  std::string panelName{"panel"};
  Color background{0.08F, 0.10F, 0.12F, 1.0F};
  float cornerRadiusPx{8.0F};
};

struct TextElement {
  std::string text;
  std::string fontFamily{"default"};
  float fontSizePx{14.0F};
  Color color{};
};

struct ButtonElement {
  std::string label;
  bool enabled{true};
  bool pressed{false};
  Color background{0.18F, 0.23F, 0.31F, 1.0F};
  Color textColor{1.0F, 1.0F, 1.0F, 1.0F};
};

struct SliderElement {
  float minValue{0.0F};
  float maxValue{1.0F};
  float value{0.0F};
  Color trackColor{0.20F, 0.20F, 0.24F, 1.0F};
  Color knobColor{0.82F, 0.82F, 0.86F, 1.0F};
};

struct IconElement {
  std::string iconKey;
  Color tint{};
};

struct ImageElement {
  std::string imageKey;
  Color tint{};
};

struct ChartScaffoldElement {
  std::string chartKind{"line"};
  std::vector<float> sampleValues;
};

struct SchematicScaffoldElement {
  std::string modelName{"default"};
  std::uint32_t symbolCount{0};
  std::uint32_t netCount{0};
};

}  // namespace volt::ui
