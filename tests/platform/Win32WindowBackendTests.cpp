#include "platform/details/Win32WindowBackendStartup.hpp"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "[win32-window-backend-test] FAIL: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  {
    const auto startupSize = volt::platform::details::resolveStartupClientSize(
        0,
        0,
        3840,
        2160,
        2.0F,
        2.0F);
    ok = expect(startupSize.logicalWidth == 960, "-1 width should resolve to half logical work width at 200% DPI") && ok;
    ok = expect(startupSize.logicalHeight == 540, "-1 height should resolve to half logical work height at 200% DPI") && ok;
    ok = expect(startupSize.physicalWidth == 1920, "fallback width should remain half of the physical work area") && ok;
    ok = expect(startupSize.physicalHeight == 1080, "fallback height should remain half of the physical work area") && ok;
  }

  {
    const auto startupSize = volt::platform::details::resolveStartupClientSize(
        1600,
        900,
        2560,
        1440,
        1.5F,
        1.5F);
    ok = expect(startupSize.logicalWidth == 1600, "explicit logical width should be preserved") && ok;
    ok = expect(startupSize.logicalHeight == 900, "explicit logical height should be preserved") && ok;
    ok = expect(startupSize.physicalWidth == 2400, "explicit width should scale once to physical pixels") && ok;
    ok = expect(startupSize.physicalHeight == 1350, "explicit height should scale once to physical pixels") && ok;
  }

  {
    const auto startupSize = volt::platform::details::resolveStartupClientSize(
        0,
        0,
        1920,
        1080,
        1.0F,
        1.0F);
    ok = expect(startupSize.logicalWidth == 960, "100% DPI fallback width should still use half work width") && ok;
    ok = expect(startupSize.logicalHeight == 540, "100% DPI fallback height should still use half work height") && ok;
    ok = expect(startupSize.physicalWidth == 960, "100% DPI fallback physical width should match logical width") && ok;
    ok = expect(startupSize.physicalHeight == 540, "100% DPI fallback physical height should match logical height") && ok;
  }

  if (!ok) {
    std::cerr << "[win32-window-backend-test] One or more tests failed." << '\n';
    return 1;
  }

  std::cout << "[win32-window-backend-test] All Win32 startup sizing tests passed." << '\n';
  return 0;
}