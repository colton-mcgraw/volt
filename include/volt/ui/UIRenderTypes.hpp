#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace volt::ui {

struct Color {
  float r{1.0F};
  float g{1.0F};
  float b{1.0F};
  float a{1.0F};
};

struct Rect {
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
};

struct UiTextCommand {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  std::string text;
  std::string fontFamily{"default"};
  float fontSizePx{14.0F};
  std::uint32_t glyphCount{0};
  Color color{};
};

struct UiRectCommand {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  Color fill{};
  float cornerRadiusPx{0.0F};
};

struct UiImageCommand {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  std::string imageKey;
  Color tint{};
};

struct UiIconCommand {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  std::string iconKey;
  Color tint{};
};

struct UiChartScaffoldCommand {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  std::string chartKind;
};

struct UiSchematicScaffoldCommand {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  std::string modelName;
};

using UiRenderCommand = std::variant<
    UiTextCommand,
    UiRectCommand,
    UiImageCommand,
    UiIconCommand,
    UiChartScaffoldCommand,
    UiSchematicScaffoldCommand>;

}  // namespace volt::ui
