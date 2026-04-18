#pragma once

#include "volt/ui/UIRenderTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace volt::ui {

enum class WidgetKind {
  kText,
  kButton,
  kCheckbox,
  kToggle,
  kSlider,
  kTextInput,
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
  bool cacheRenderTarget{false};
  bool cacheAllowImmediateFallback{true};
  bool cacheDirty{false};
  bool* cacheDirtyBinding{nullptr};
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

struct CheckboxElement {
  std::string label;
  bool* checked{nullptr};
  bool enabled{true};
  Color boxColor{0.17F, 0.19F, 0.24F, 1.0F};
  Color checkColor{0.86F, 0.91F, 0.98F, 1.0F};
  Color textColor{0.95F, 0.97F, 1.0F, 1.0F};
};

struct ToggleElement {
  std::string label;
  bool* value{nullptr};
  bool enabled{true};
  Color trackOffColor{0.22F, 0.24F, 0.28F, 1.0F};
  Color trackOnColor{0.24F, 0.46F, 0.70F, 1.0F};
  Color knobColor{0.96F, 0.97F, 1.0F, 1.0F};
  Color textColor{0.95F, 0.97F, 1.0F, 1.0F};
};

struct SliderElement {
  float minValue{0.0F};
  float maxValue{1.0F};
  float value{0.0F};
  Color trackColor{0.20F, 0.20F, 0.24F, 1.0F};
  Color knobColor{0.82F, 0.82F, 0.86F, 1.0F};
  bool enabled{true};
  float* valueBinding{nullptr};
};

struct TextInputElement {
  std::string* value{nullptr};
  std::string placeholder;
  std::size_t maxLength{256};
  bool enabled{true};
  Color background{0.10F, 0.13F, 0.17F, 1.0F};
  Color borderColor{0.28F, 0.35F, 0.44F, 1.0F};
  Color textColor{0.95F, 0.97F, 1.0F, 1.0F};
  Color placeholderColor{0.62F, 0.69F, 0.78F, 1.0F};
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
