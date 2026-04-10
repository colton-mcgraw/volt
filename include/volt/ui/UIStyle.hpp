#pragma once

#include "volt/ui/UIRenderTypes.hpp"

#include <string>
#include <string_view>

namespace volt::ui {

struct ThemeTokens {
  Color textPrimary{0.95F, 0.97F, 1.0F, 1.0F};
  Color textMuted{0.70F, 0.74F, 0.80F, 1.0F};
  Color buttonBackground{0.20F, 0.31F, 0.45F, 1.0F};
  Color buttonHover{0.25F, 0.36F, 0.52F, 1.0F};
  Color buttonPressed{0.16F, 0.24F, 0.34F, 1.0F};
  Color sliderTrack{0.17F, 0.19F, 0.22F, 1.0F};
  Color sliderKnob{0.85F, 0.88F, 0.93F, 1.0F};
  Color panelBackground{0.08F, 0.10F, 0.12F, 1.0F};
};

class StyleSheet {
 public:
  [[nodiscard]] const ThemeTokens& theme() const { return theme_; }

  [[nodiscard]] bool loadFromFile(const std::string& path);
  void applyDefaults();

 private:
  bool applyColorToken(std::string_view key, const Color& color);

  ThemeTokens theme_{};
};

}  // namespace volt::ui
