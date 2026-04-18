#include "volt/core/AppConfig.hpp"
#include "volt/io/assets/Manifest.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[app-config-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

bool writeManifest(const std::filesystem::path& manifestPath, const std::string& jsonText) {
  std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << jsonText;
  return out.good();
}

}  // namespace

int main() {
  bool ok = true;

  const std::filesystem::path sandbox =
      std::filesystem::temp_directory_path() / "volt-app-config-tests";
  std::error_code ec;
  std::filesystem::remove_all(sandbox, ec);
  std::filesystem::create_directories(sandbox / "assets", ec);
  ok = expect(!ec, "create sandbox assets directory") && ok;
  if (!ok) {
    return 1;
  }

  const std::filesystem::path manifestPath = sandbox / "assets" / "manifest.json";

  const std::filesystem::path oldCwd = std::filesystem::current_path(ec);
  ok = expect(!ec, "capture current working directory") && ok;
  if (!ok) {
    return 1;
  }

  std::filesystem::current_path(sandbox, ec);
  ok = expect(!ec, "switch to sandbox working directory") && ok;
  if (!ok) {
    return 1;
  }

  const std::string firstManifest =
      "{\n"
      "  \"appname\": \"Volt Test\",\n"
      "  \"default-window-title\": \"Volt Test Window\",\n"
      "  \"window-resizable\": false,\n"
      "  \"default-window-x\": 111,\n"
      "  \"default-window-y\": 222,\n"
      "  \"default-window-width\": 1280,\n"
      "  \"default-window-height\": 720,\n"
      "  \"window-resize-border-thickness\": 7,\n"
      "  \"window-titlebar-height\": 33,\n"
      "  \"window-titlebar-padding\": 6,\n"
      "  \"window-titlebar-button-size\": 18,\n"
      "  \"window-titlebar-button-spacing\": 4,\n"
      "  \"icon\": \"./images/icon-one.png\"\n"
      "}\n";

  ok = expect(writeManifest(manifestPath, firstManifest), "write first manifest") && ok;
  volt::io::manifestService().refresh(true);

  {
    const volt::core::AppConfig config{};
    ok = expect(config.appName == "Volt Test", "appname parsed") && ok;
    ok = expect(config.windowTitle == "Volt Test Window", "default-window-title parsed") && ok;
    ok = expect(!config.windowResizable, "window-resizable bool parsed") && ok;
    ok = expect(config.windowX == 111, "default-window-x parsed") && ok;
    ok = expect(config.windowY == 222, "default-window-y parsed") && ok;
    ok = expect(config.windowWidth == 1280U, "default-window-width parsed") && ok;
    ok = expect(config.windowHeight == 720U, "default-window-height parsed") && ok;
    ok = expect(config.windowResizeBorderThickness == 7, "window-resize-border-thickness parsed") && ok;
    ok = expect(config.windowTitleBarHeight == 33, "window-titlebar-height parsed") && ok;
    ok = expect(config.windowTitleBarPadding == 6, "window-titlebar-padding parsed") && ok;
    ok = expect(config.windowTitleBarButtonSize == 18, "window-titlebar-button-size parsed") && ok;
    ok = expect(config.windowTitleBarButtonSpacing == 4, "window-titlebar-button-spacing parsed") && ok;
    ok = expect(config.windowIconPath == "./images/icon-one.png", "icon fallback parsed") && ok;
  }

  const std::string secondManifest =
      "{\n"
      "  \"appname\": \"Volt String Bool\",\n"
      "  \"window-resizable\": \"true\",\n"
      "  \"default-window-x\": -1,\n"
      "  \"default-window-y\": -1,\n"
      "  \"default-window-width\": -1,\n"
      "  \"default-window-height\": 0,\n"
      "  \"window-titlebar-height\": 0,\n"
      "  \"window-icon\": \"./images/icon-two.png\"\n"
      "}\n";

  ok = expect(writeManifest(manifestPath, secondManifest), "write second manifest") && ok;
  volt::io::manifestService().refresh(true);

  {
    const volt::core::AppConfig config{};
    ok = expect(config.appName == "Volt String Bool", "updated appname parsed") && ok;
    ok = expect(config.windowResizable, "string bool window-resizable parsed") && ok;
    ok = expect(config.windowX == -1, "negative default-window-x parsed") && ok;
    ok = expect(config.windowY == -1, "negative default-window-y parsed") && ok;
    ok = expect(config.windowWidth == 0U, "non-positive default-window-width coerced") && ok;
    ok = expect(config.windowHeight == 0U, "non-positive default-window-height coerced") && ok;
    ok = expect(config.windowTitleBarHeight == 38, "non-positive titlebar height uses default") && ok;
    ok = expect(config.windowIconPath == "./images/icon-two.png", "window-icon overrides icon fallback") && ok;
  }

  std::filesystem::current_path(oldCwd, ec);

  if (!ok) {
    std::cerr << "[app-config-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[app-config-test] All AppConfig tests passed." << '\n';
  return 0;
}
