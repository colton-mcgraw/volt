#include "volt/ui/UIStyle.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace volt::ui {
namespace {

std::string trim(std::string value) {
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };

  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) { return !isSpace(c); }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) { return !isSpace(c); }).base(), value.end());
  return value;
}

bool parseColor(const std::string& value, Color* outColor) {
  if (outColor == nullptr) {
    return false;
  }

  std::istringstream stream(value);
  Color parsed{};
  if (!(stream >> parsed.r >> parsed.g >> parsed.b >> parsed.a)) {
    return false;
  }

  *outColor = parsed;
  return true;
}

}  // namespace

bool StyleSheet::loadFromFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }

  applyDefaults();

  std::string line;
  while (std::getline(file, line)) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    const std::size_t sep = trimmed.find('=');
    if (sep == std::string::npos) {
      continue;
    }

    const std::string key = trim(trimmed.substr(0, sep));
    const std::string value = trim(trimmed.substr(sep + 1));

    Color parsedColor{};
    if (!parseColor(value, &parsedColor)) {
      continue;
    }

    applyColorToken(key, parsedColor);
  }

  return true;
}

void StyleSheet::applyDefaults() {
  theme_ = ThemeTokens{};
}

bool StyleSheet::applyColorToken(std::string_view key, const Color& color) {
  if (key == "text.primary") {
    theme_.textPrimary = color;
    return true;
  }
  if (key == "text.muted") {
    theme_.textMuted = color;
    return true;
  }
  if (key == "button.background") {
    theme_.buttonBackground = color;
    return true;
  }
  if (key == "button.hover") {
    theme_.buttonHover = color;
    return true;
  }
  if (key == "button.pressed") {
    theme_.buttonPressed = color;
    return true;
  }
  if (key == "slider.track") {
    theme_.sliderTrack = color;
    return true;
  }
  if (key == "slider.knob") {
    theme_.sliderKnob = color;
    return true;
  }
  if (key == "panel.background") {
    theme_.panelBackground = color;
    return true;
  }

  return false;
}

}  // namespace volt::ui
