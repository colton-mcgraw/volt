#include "volt/platform/details/Win32WindowBackend.hpp"
#include "Win32WindowBackendStartup.hpp"

#include "volt/core/Logging.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#endif

namespace volt::platform::details {

namespace {

#if defined(_WIN32)
constexpr wchar_t kVoltWindowClassName[] = L"VoltWindowClass";
std::atomic_uint32_t gWindowClassUsers{0};
constexpr UINT kDefaultDpi = 96U;
constexpr int kDefaultCustomTitleBarHeightPx = 38;
constexpr int kDefaultCaptionButtonSizePx = 20;
constexpr int kDefaultCaptionButtonPaddingPx = 12;
constexpr int kDefaultCaptionButtonSpacingPx = 0;
constexpr int kMinimumCaptionButtonSlotWidthPx = 78;
constexpr int kCaptionButtonCount = 3;

struct FrameMetrics {
  int frameX{0};
  int frameY{0};
  int paddedBorder{0};
};

struct DpiValues {
  UINT effectiveX{kDefaultDpi};
  UINT effectiveY{kDefaultDpi};
  UINT rawX{kDefaultDpi};
  UINT rawY{kDefaultDpi};
};

void enableProcessDpiAwareness() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;

  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto setProcessDpiAwarenessContext =
        reinterpret_cast<SetProcessDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setProcessDpiAwarenessContext != nullptr &&
        setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
      return;
    }
  }

  const HRESULT awarenessResult = SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
  if (SUCCEEDED(awarenessResult) || awarenessResult == E_ACCESSDENIED) {
    return;
  }

  SetProcessDPIAware();
}

DpiValues queryMonitorDpi(HMONITOR monitor) {
  DpiValues dpi{};
  if (monitor == nullptr) {
    return dpi;
  }

  UINT effectiveX = kDefaultDpi;
  UINT effectiveY = kDefaultDpi;
  if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &effectiveX, &effectiveY))) {
    dpi.effectiveX = effectiveX;
    dpi.effectiveY = effectiveY;
  }

  UINT rawX = dpi.effectiveX;
  UINT rawY = dpi.effectiveY;
  if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_RAW_DPI, &rawX, &rawY))) {
    dpi.rawX = rawX;
    dpi.rawY = rawY;
  } else {
    dpi.rawX = dpi.effectiveX;
    dpi.rawY = dpi.effectiveY;
  }

  return dpi;
}

FrameMetrics queryFrameMetrics(HWND hwnd) {
  const UINT dpi = GetDpiForWindow(hwnd);
  return {
      .frameX = GetSystemMetricsForDpi(SM_CXFRAME, dpi),
      .frameY = GetSystemMetricsForDpi(SM_CYFRAME, dpi),
      .paddedBorder = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi),
  };
}

int scaleMetricPx(int logicalPx, float scale) {
  return std::max(1, static_cast<int>(std::lround(static_cast<double>(logicalPx) * std::max(0.01F, scale))));
}

int resolvedTitleBarHeight(const WindowCreateOptions& options, float scale = 1.0F) {
  return scaleMetricPx(options.titleBarHeight > 0 ? options.titleBarHeight : kDefaultCustomTitleBarHeightPx, scale);
}

int resolvedCaptionButtonSize(const WindowCreateOptions& options, float scale = 1.0F) {
  return scaleMetricPx(options.titleBarButtonSize > 0 ? options.titleBarButtonSize : kDefaultCaptionButtonSizePx, scale);
}

int resolvedCaptionButtonPadding(const WindowCreateOptions& options, float scale = 1.0F) {
  return scaleMetricPx(options.titleBarPadding > 0 ? options.titleBarPadding : kDefaultCaptionButtonPaddingPx, scale);
}

int resolvedCaptionButtonSpacing(const WindowCreateOptions& options, float scale = 1.0F) {
  return options.titleBarButtonSpacing > 0 ? scaleMetricPx(options.titleBarButtonSpacing, scale) : kDefaultCaptionButtonSpacingPx;
}

int resolvedCaptionButtonSlotWidth(const WindowCreateOptions& options, float scale = 1.0F) {
  const int baseWidth = resolvedCaptionButtonSize(options, scale) + (resolvedCaptionButtonPadding(options, scale) * 2);
  return std::max(baseWidth, scaleMetricPx(kMinimumCaptionButtonSlotWidthPx, scale));
}

RECT queryWorkAreaOrPrimaryScreen() {
  RECT area{};
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0) != FALSE) {
    return area;
  }

  area.left = 0;
  area.top = 0;
  area.right = GetSystemMetrics(SM_CXSCREEN);
  area.bottom = GetSystemMetrics(SM_CYSCREEN);
  return area;
}

RECT queryMonitorWorkArea(HMONITOR monitor) {
  if (monitor != nullptr) {
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
      return monitorInfo.rcWork;
    }
  }

  return queryWorkAreaOrPrimaryScreen();
}

POINT queryCursorPositionOrOrigin() {
  POINT cursor{};
  if (GetCursorPos(&cursor) != FALSE) {
    return cursor;
  }

  return POINT{0, 0};
}

HMONITOR chooseStartupMonitor(const WindowCreateOptions& options) {
  POINT referencePoint = queryCursorPositionOrOrigin();

  if (options.initialX > 0) {
    referencePoint.x = options.initialX;
  }
  if (options.initialY > 0) {
    referencePoint.y = options.initialY;
  }

  return MonitorFromPoint(referencePoint, MONITOR_DEFAULTTONEAREST);
}

void configureCustomChrome(HWND hwnd) {
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

  // `0xFFFFFFFE` means "no border color" and removes the accent outline on supported systems.
  const COLORREF borderColorNone = 0xFFFFFFFE;
  (void)DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColorNone, sizeof(borderColorNone));
}
#endif

char32_t decodeWin32CharCodepoint(wchar_t value, wchar_t* pendingHighSurrogate) {
  if (pendingHighSurrogate == nullptr) {
    return 0;
  }

  if (value >= 0xD800 && value <= 0xDBFF) {
    *pendingHighSurrogate = value;
    return 0;
  }

  if (value >= 0xDC00 && value <= 0xDFFF) {
    if (*pendingHighSurrogate >= 0xD800 && *pendingHighSurrogate <= 0xDBFF) {
      const std::uint32_t high = static_cast<std::uint32_t>(*pendingHighSurrogate - 0xD800);
      const std::uint32_t low = static_cast<std::uint32_t>(value - 0xDC00);
      *pendingHighSurrogate = 0;
      return static_cast<char32_t>(0x10000U + ((high << 10U) | low));
    }

    *pendingHighSurrogate = 0;
    return 0xFFFD;
  }

  *pendingHighSurrogate = 0;
  return static_cast<char32_t>(value);
}

}  // namespace

Win32WindowBackend::Win32WindowBackend(
    std::uint32_t width,
    std::uint32_t height,
    const std::string& title,
    const WindowCreateOptions& options) {
#if defined(_WIN32)
  options_ = options;
  enableProcessDpiAwareness();

  DWORD windowStyle = WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX;
  const bool allowResize = options_.windowResizable;
  if (options_.useSystemTitleBar) {
    windowStyle |= WS_CAPTION;
  }

  // Keep the sizing frame even in custom-chrome mode so OS interactive sizing remains functional.
  const bool needsSizingFrame = allowResize && (options_.useSystemResizeHandles || !options_.useSystemTitleBar);
  if (needsSizingFrame) {
    windowStyle |= WS_THICKFRAME | WS_MAXIMIZEBOX;
  }

  if (!options_.useSystemTitleBar) {
    windowStyle &= ~WS_CAPTION;
    windowStyle |= WS_POPUP;
  }

  hinstance_ = GetModuleHandleW(nullptr);
  if (hinstance_ == nullptr) {
    throw std::runtime_error("Failed to acquire Win32 module handle");
  }

  if (gWindowClassUsers.fetch_add(1, std::memory_order_acq_rel) == 0U) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Win32WindowBackend::windowProcThunk;
    windowClass.hInstance = hinstance_;
    windowClass.lpszClassName = kVoltWindowClassName;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (RegisterClassExW(&windowClass) == 0) {
      gWindowClassUsers.fetch_sub(1, std::memory_order_acq_rel);
      throw std::runtime_error("Failed to register Win32 window class");
    }
  }

  const std::wstring windowTitle(title.begin(), title.end());
  const int requestedClientWidth = static_cast<int>(width);
  const int requestedClientHeight = static_cast<int>(height);
  const bool useDefaultWidth = requestedClientWidth <= 0;
  const bool useDefaultHeight = requestedClientHeight <= 0;

  const HMONITOR startupMonitor = chooseStartupMonitor(options_);
  const RECT workArea = queryMonitorWorkArea(startupMonitor);
    const int workWidth = workArea.right > workArea.left
      ? static_cast<int>(workArea.right - workArea.left)
      : 1;
    const int workHeight = workArea.bottom > workArea.top
      ? static_cast<int>(workArea.bottom - workArea.top)
      : 1;

  const DpiValues startupDpi = queryMonitorDpi(startupMonitor);
  const float startupScaleX = static_cast<float>(startupDpi.effectiveX) / static_cast<float>(kDefaultDpi);
  const float startupScaleY = static_cast<float>(startupDpi.effectiveY) / static_cast<float>(kDefaultDpi);
  const StartupClientSize startupClientSize = resolveStartupClientSize(
      requestedClientWidth,
      requestedClientHeight,
      workWidth,
      workHeight,
      startupScaleX,
      startupScaleY);
  const int physicalClientWidth = startupClientSize.physicalWidth;
  const int physicalClientHeight = startupClientSize.physicalHeight;

  RECT desiredRect{
    0,
    0,
    physicalClientWidth,
    physicalClientHeight,
  };
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  BOOL adjusted = FALSE;
  if (user32 != nullptr) {
    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto adjustWindowRectExForDpi =
        reinterpret_cast<AdjustWindowRectExForDpiFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    if (adjustWindowRectExForDpi != nullptr) {
      adjusted = adjustWindowRectExForDpi(&desiredRect, windowStyle, FALSE, 0, startupDpi.effectiveX);
    }
  }
  if (adjusted == FALSE) {
    AdjustWindowRectEx(&desiredRect, windowStyle, FALSE, 0);
  }

    const int windowWidth = desiredRect.right > desiredRect.left
      ? static_cast<int>(desiredRect.right - desiredRect.left)
      : 1;
    const int windowHeight = desiredRect.bottom > desiredRect.top
      ? static_cast<int>(desiredRect.bottom - desiredRect.top)
      : 1;

  const int defaultWindowX = workArea.left + std::max(0, (workWidth - windowWidth) / 2);
  const int defaultWindowY = workArea.top + std::max(0, (workHeight - windowHeight) / 2);

  const int windowX = options_.initialX > 0 ? options_.initialX : defaultWindowX;
  const int windowY = options_.initialY > 0 ? options_.initialY : defaultWindowY;

  hwnd_ = CreateWindowExW(
      0,
      kVoltWindowClassName,
      windowTitle.c_str(),
      windowStyle,
      windowX,
      windowY,
      windowWidth,
      windowHeight,
      nullptr,
      nullptr,
      hinstance_,
      this);

  if (hwnd_ == nullptr) {
    if (gWindowClassUsers.fetch_sub(1, std::memory_order_acq_rel) == 1U) {
      UnregisterClassW(kVoltWindowClassName, hinstance_);
    }
    throw std::runtime_error("Failed to create Win32 window");
  }

  if (!options_.useSystemTitleBar) {
    configureCustomChrome(hwnd_);
    SetWindowPos(
        hwnd_,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  if (!options_.windowIconPath.empty()) {
    const std::wstring iconPath = std::filesystem::path(options_.windowIconPath).lexically_normal().wstring();

    iconBig_ = reinterpret_cast<HICON>(LoadImageW(
        nullptr,
        iconPath.c_str(),
        IMAGE_ICON,
        0,
        0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE));

    iconSmall_ = reinterpret_cast<HICON>(LoadImageW(
        nullptr,
        iconPath.c_str(),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_LOADFROMFILE));

    if (iconBig_ != nullptr) {
      SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig_));
    }

    if (iconSmall_ != nullptr) {
      SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall_));
    } else if (iconBig_ != nullptr) {
      iconSmall_ = iconBig_;
      SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall_));
    }

    if (iconBig_ == nullptr && iconSmall_ == nullptr) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kPlatform,
          "Failed to load window icon from ",
          options_.windowIconPath,
          "; ensure the file is a valid .ico/.bmp image");
    }
  }

  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  updateDisplayMetrics();

  POINT cursor{};
  if (GetCursorPos(&cursor) != FALSE && ScreenToClient(hwnd_, &cursor) != FALSE) {
    inputState_.mouse.x = physicalToLogicalX(static_cast<double>(cursor.x));
    inputState_.mouse.y = physicalToLogicalY(static_cast<double>(cursor.y));
  }

  VOLT_LOG_INFO_CAT(
      volt::core::logging::Category::kPlatform,
      "Win32 window backend initialized: ",
      title,
      " (",
      useDefaultWidth ? 0 : startupClientSize.logicalWidth,
      "x",
      useDefaultHeight ? 0 : startupClientSize.logicalHeight,
      ")");
#else
  (void)width;
  (void)height;
  (void)title;
  (void)options;
  throw std::runtime_error("Win32 window backend is unavailable on this platform");
#endif
}

Win32WindowBackend::~Win32WindowBackend() {
#if defined(_WIN32)
  if (iconBig_ != nullptr && iconBig_ != iconSmall_) {
    DestroyIcon(iconBig_);
    iconBig_ = nullptr;
  }

  if (iconSmall_ != nullptr) {
    DestroyIcon(iconSmall_);
    iconSmall_ = nullptr;
  }

  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  if (hinstance_ != nullptr && gWindowClassUsers.fetch_sub(1, std::memory_order_acq_rel) == 1U) {
    UnregisterClassW(kVoltWindowClassName, hinstance_);
  }
#endif
}

bool Win32WindowBackend::shouldClose() const {
  return shouldClose_;
}

void Win32WindowBackend::requestClose() {
  shouldClose_ = true;
#if defined(_WIN32)
  if (hwnd_ != nullptr) {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }
#endif
}

void Win32WindowBackend::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  eventDispatcher_ = dispatcher;
}

void Win32WindowBackend::setResizeRepaintCallback(std::function<void()> callback) {
  resizeRepaintCallback_ = std::move(callback);
}

void Win32WindowBackend::pollEvents() {
#if defined(_WIN32)
  resetTransientInputState();
  processPendingMessages();
#endif
}

void Win32WindowBackend::waitEvents() {
#if defined(_WIN32)
  resetTransientInputState();

  MSG message{};
  const BOOL getMessageResult = GetMessageW(&message, nullptr, 0, 0);
  if (getMessageResult <= 0) {
    shouldClose_ = true;
    return;
  }

  TranslateMessage(&message);
  DispatchMessageW(&message);
  processPendingMessages();
#endif
}

void Win32WindowBackend::waitEventsTimeout(double timeoutSeconds) {
#if defined(_WIN32)
  resetTransientInputState();

  const double clampedTimeout = timeoutSeconds > 0.0 ? timeoutSeconds : 0.0;
  const auto timeoutMilliseconds = static_cast<DWORD>(clampedTimeout * 1000.0);
  const DWORD waitResult = MsgWaitForMultipleObjectsEx(
      0,
      nullptr,
      timeoutMilliseconds,
      QS_ALLINPUT,
      MWMO_INPUTAVAILABLE);

  if (waitResult == WAIT_FAILED) {
    throw std::runtime_error("Win32 event wait failed");
  }

  processPendingMessages();
#else
  (void)timeoutSeconds;
#endif
}

bool Win32WindowBackend::wasResized() const {
  return framebufferResized_;
}

void Win32WindowBackend::acknowledgeResize() {
  framebufferResized_ = false;
}

bool Win32WindowBackend::isMinimized() const {
  return minimized_;
}

std::pair<std::uint32_t, std::uint32_t> Win32WindowBackend::framebufferExtent() const {
#if defined(_WIN32)
  if (hwnd_ == nullptr) {
    return {0U, 0U};
  }

  RECT clientRect{};
  if (GetClientRect(hwnd_, &clientRect) == FALSE) {
    return {0U, 0U};
  }

  const auto width = static_cast<std::uint32_t>(
      clientRect.right > clientRect.left ? clientRect.right - clientRect.left : 0);
  const auto height = static_cast<std::uint32_t>(
      clientRect.bottom > clientRect.top ? clientRect.bottom - clientRect.top : 0);
  return {width, height};
#else
  return {0U, 0U};
#endif
}

std::pair<std::uint32_t, std::uint32_t> Win32WindowBackend::logicalExtent() const {
#if defined(_WIN32)
  const auto [width, height] = framebufferExtent();
  const double scaleX = std::max(0.01F, displayMetrics_.contentScaleX);
  const double scaleY = std::max(0.01F, displayMetrics_.contentScaleY);
  return {
      static_cast<std::uint32_t>(std::max(0.0, std::round(static_cast<double>(width) / scaleX))),
      static_cast<std::uint32_t>(std::max(0.0, std::round(static_cast<double>(height) / scaleY))),
  };
#else
  return {0U, 0U};
#endif
}

DisplayMetrics Win32WindowBackend::displayMetrics() const {
  return displayMetrics_;
}

const InputState& Win32WindowBackend::inputSnapshot() const {
  return inputState_;
}

WindowBackendType Win32WindowBackend::backendType() const {
  return WindowBackendType::kWin32;
}

void* Win32WindowBackend::nativeWindowHandle() const {
#if defined(_WIN32)
  return hwnd_;
#else
  return nullptr;
#endif
}

void* Win32WindowBackend::nativeDisplayHandle() const {
#if defined(_WIN32)
  return hinstance_;
#else
  return nullptr;
#endif
}

void Win32WindowBackend::beginInteractiveMove() {
#if defined(_WIN32)
  if (hwnd_ == nullptr) {
    return;
  }

  ReleaseCapture();
  SendMessageW(hwnd_, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
#endif
}

void Win32WindowBackend::beginInteractiveResize(WindowResizeEdge edge) {
#if defined(_WIN32)
  if (!options_.windowResizable) {
    return;
  }

  if (hwnd_ == nullptr) {
    return;
  }

  UINT hitTest = HTNOWHERE;
  switch (edge) {
    case WindowResizeEdge::kLeft:
      hitTest = HTLEFT;
      break;
    case WindowResizeEdge::kTop:
      hitTest = HTTOP;
      break;
    case WindowResizeEdge::kRight:
      hitTest = HTRIGHT;
      break;
    case WindowResizeEdge::kBottom:
      hitTest = HTBOTTOM;
      break;
    case WindowResizeEdge::kTopLeft:
      hitTest = HTTOPLEFT;
      break;
    case WindowResizeEdge::kTopRight:
      hitTest = HTTOPRIGHT;
      break;
    case WindowResizeEdge::kBottomLeft:
      hitTest = HTBOTTOMLEFT;
      break;
    case WindowResizeEdge::kBottomRight:
      hitTest = HTBOTTOMRIGHT;
      break;
    default:
      return;
  }

  POINT cursor{};
  if (GetCursorPos(&cursor) == FALSE) {
    return;
  }

  const LPARAM cursorPosition = MAKELPARAM(
      static_cast<std::uint16_t>(cursor.x & 0xFFFF),
      static_cast<std::uint16_t>(cursor.y & 0xFFFF));

  ReleaseCapture();
  SendMessageW(hwnd_, WM_NCLBUTTONDOWN, hitTest, cursorPosition);
#else
  (void)edge;
#endif
}

bool Win32WindowBackend::isMaximized() const {
#if defined(_WIN32)
  return hwnd_ != nullptr && IsZoomed(hwnd_) != FALSE;
#else
  return false;
#endif
}

void Win32WindowBackend::toggleMaximized() {
#if defined(_WIN32)
  if (!options_.windowResizable) {
    return;
  }

  if (hwnd_ == nullptr) {
    return;
  }

  ShowWindow(hwnd_, isMaximized() ? SW_RESTORE : SW_MAXIMIZE);
#endif
}

void Win32WindowBackend::minimize() {
#if defined(_WIN32)
  if (hwnd_ == nullptr) {
    return;
  }

  ShowWindow(hwnd_, SW_MINIMIZE);
#endif
}

void Win32WindowBackend::resetTransientInputState() {
  inputState_.mouse.deltaX = 0.0;
  inputState_.mouse.deltaY = 0.0;
  inputState_.mouse.scrollX = 0.0;
  inputState_.mouse.scrollY = 0.0;
}

void Win32WindowBackend::processPendingMessages() {
#if defined(_WIN32)
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    if (message.message == WM_QUIT) {
      shouldClose_ = true;
      continue;
    }

    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
#endif
}

void Win32WindowBackend::publishResizeEvent(std::uint32_t width, std::uint32_t height) {
  if (eventDispatcher_ == nullptr) {
    return;
  }

  eventDispatcher_->enqueue({
      .type = volt::event::EventType::kWindowResized,
      .payload = volt::event::WindowResizeEvent{
          .width = width,
          .height = height,
      },
  });
}

void Win32WindowBackend::publishMinimizedEvent(bool minimized) {
  if (eventDispatcher_ == nullptr) {
    return;
  }

  eventDispatcher_->enqueue({
      .type = volt::event::EventType::kWindowMinimized,
      .payload = volt::event::WindowMinimizedEvent{.minimized = minimized},
  });
}

int Win32WindowBackend::modifierMask() const {
#if defined(_WIN32)
  int mask = 0;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
    mask |= 0x0001;
  }
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
    mask |= 0x0002;
  }
  if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    mask |= 0x0004;
  }
  if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) {
    mask |= 0x0008;
  }
  return mask;
#else
  return 0;
#endif
}

void Win32WindowBackend::updateDisplayMetrics() {
#if defined(_WIN32)
  if (hwnd_ == nullptr) {
    displayMetrics_ = {};
    return;
  }

  const DisplayMetrics previous = displayMetrics_;
  const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  const DpiValues monitorDpi = queryMonitorDpi(monitor);
  const UINT windowDpi = GetDpiForWindow(hwnd_);
  displayMetrics_.dpiX = static_cast<float>(windowDpi == 0U ? monitorDpi.effectiveX : windowDpi);
  displayMetrics_.dpiY = static_cast<float>(windowDpi == 0U ? monitorDpi.effectiveY : windowDpi);
  displayMetrics_.ppiX = static_cast<float>(monitorDpi.rawX == 0U ? static_cast<UINT>(displayMetrics_.dpiX) : monitorDpi.rawX);
  displayMetrics_.ppiY = static_cast<float>(monitorDpi.rawY == 0U ? static_cast<UINT>(displayMetrics_.dpiY) : monitorDpi.rawY);
  displayMetrics_.contentScaleX = displayMetrics_.dpiX / static_cast<float>(kDefaultDpi);
  displayMetrics_.contentScaleY = displayMetrics_.dpiY / static_cast<float>(kDefaultDpi);

  const bool changed =
      std::abs(displayMetrics_.dpiX - previous.dpiX) > 0.01F ||
      std::abs(displayMetrics_.dpiY - previous.dpiY) > 0.01F ||
      std::abs(displayMetrics_.ppiX - previous.ppiX) > 0.01F ||
      std::abs(displayMetrics_.ppiY - previous.ppiY) > 0.01F;
  if (changed) {
    VOLT_LOG_INFO_CAT(
        volt::core::logging::Category::kPlatform,
        "Win32 display metrics updated dpi=",
        displayMetrics_.dpiX,
        "x",
        displayMetrics_.dpiY,
        " ppi=",
        displayMetrics_.ppiX,
        "x",
        displayMetrics_.ppiY,
        " scale=",
        displayMetrics_.contentScaleX,
        "x",
        displayMetrics_.contentScaleY);
  }
#endif
}

double Win32WindowBackend::physicalToLogicalX(double value) const {
  return value / std::max(0.01F, displayMetrics_.contentScaleX);
}

double Win32WindowBackend::physicalToLogicalY(double value) const {
  return value / std::max(0.01F, displayMetrics_.contentScaleY);
}

#if defined(_WIN32)
LRESULT Win32WindowBackend::handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_NCCALCSIZE: {
      if (!options_.useSystemTitleBar && wParam == TRUE) {
        // Extend client area to the full window bounds; custom hit testing handles resize zones.
        return 0;
      }
      break;
    }
    case WM_GETMINMAXINFO: {
      if (!options_.useSystemTitleBar) {
        auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
        if (minMaxInfo == nullptr) {
          return 0;
        }

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
          minMaxInfo->ptMaxPosition.x = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
          minMaxInfo->ptMaxPosition.y = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;
          minMaxInfo->ptMaxSize.x = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
          minMaxInfo->ptMaxSize.y = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
          return 0;
        }
      }
      break;
    }
    case WM_NCHITTEST: {
      if (!options_.useSystemTitleBar) {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(hwnd_, message, wParam, lParam, &dwmResult) != FALSE) {
          return dwmResult;
        }

        POINT cursorScreenPoint{
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam),
        };

        RECT windowRect{};
        if (GetWindowRect(hwnd_, &windowRect) == FALSE) {
          return HTCLIENT;
        }

        const int windowWidth = windowRect.right - windowRect.left;
        const int windowHeight = windowRect.bottom - windowRect.top;
        if (windowWidth <= 0 || windowHeight <= 0) {
          return HTCLIENT;
        }

        const int xInWindow = cursorScreenPoint.x - windowRect.left;
        const int yInWindow = cursorScreenPoint.y - windowRect.top;

        const FrameMetrics metrics = queryFrameMetrics(hwnd_);
        const int resizeBorderX = options_.resizeBorderThickness > 0
          ? options_.resizeBorderThickness
          : std::max(1, metrics.frameX + metrics.paddedBorder);
        const int resizeBorderY = options_.resizeBorderThickness > 0
          ? options_.resizeBorderThickness
          : std::max(1, metrics.frameY + metrics.paddedBorder);

        if (options_.windowResizable && !isMaximized()) {
          const bool onLeft = xInWindow >= 0 && xInWindow < resizeBorderX;
          const bool onRight = xInWindow < windowWidth && xInWindow >= (windowWidth - resizeBorderX);
          const bool onTop = yInWindow >= 0 && yInWindow < resizeBorderY;
          const bool onBottom = yInWindow < windowHeight && yInWindow >= (windowHeight - resizeBorderY);

          if (onTop && onLeft) {
            return HTTOPLEFT;
          }
          if (onTop && onRight) {
            return HTTOPRIGHT;
          }
          if (onBottom && onLeft) {
            return HTBOTTOMLEFT;
          }
          if (onBottom && onRight) {
            return HTBOTTOMRIGHT;
          }
          if (onLeft) {
            return HTLEFT;
          }
          if (onRight) {
            return HTRIGHT;
          }
          if (onTop) {
            return HTTOP;
          }
          if (onBottom) {
            return HTBOTTOM;
          }
        }

        POINT cursorClientPoint = cursorScreenPoint;
        if (ScreenToClient(hwnd_, &cursorClientPoint) == FALSE) {
          return HTCLIENT;
        }

        RECT clientRect{};
        if (GetClientRect(hwnd_, &clientRect) != FALSE) {
          const int captionButtonSlotWidth = resolvedCaptionButtonSlotWidth(options_, displayMetrics_.contentScaleX);
          const int captionButtonSpacing = resolvedCaptionButtonSpacing(options_, displayMetrics_.contentScaleX);
          const int captionButtonsWidth =
              (captionButtonSlotWidth * kCaptionButtonCount) +
              (captionButtonSpacing * (kCaptionButtonCount - 1));
          const int captionButtonsStartX = clientRect.right - captionButtonsWidth;
          if (cursorClientPoint.y >= 0 && cursorClientPoint.y < resolvedTitleBarHeight(options_, displayMetrics_.contentScaleY) &&
              cursorClientPoint.x >= 0 && cursorClientPoint.x < captionButtonsStartX) {
            return HTCAPTION;
          }
        }

        return HTCLIENT;
      }
      break;
    }
    case WM_CLOSE: {
      shouldClose_ = true;
      DestroyWindow(hwnd_);
      return 0;
    }
    case WM_DESTROY: {
      shouldClose_ = true;
      PostQuitMessage(0);
      return 0;
    }
    case WM_SIZE: {
      updateDisplayMetrics();
      const std::uint32_t width = static_cast<std::uint32_t>(LOWORD(lParam));
      const std::uint32_t height = static_cast<std::uint32_t>(HIWORD(lParam));
      framebufferResized_ = true;

      const bool nowMinimized = (wParam == SIZE_MINIMIZED) || width == 0U || height == 0U;
      if (nowMinimized != minimized_) {
        minimized_ = nowMinimized;
        publishMinimizedEvent(minimized_);
      }

      publishResizeEvent(width, height);

      if (!minimized_ && resizeRepaintCallback_) {
        resizeRepaintCallback_();
      }

      return 0;
    }
    case WM_DPICHANGED: {
      updateDisplayMetrics();
      const RECT* suggestedRect = reinterpret_cast<const RECT*>(lParam);
      if (suggestedRect != nullptr) {
        SetWindowPos(
            hwnd_,
            nullptr,
            suggestedRect->left,
            suggestedRect->top,
            suggestedRect->right - suggestedRect->left,
            suggestedRect->bottom - suggestedRect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
      }
      framebufferResized_ = true;
      const auto [width, height] = framebufferExtent();
      publishResizeEvent(width, height);
      if (!minimized_ && resizeRepaintCallback_) {
        resizeRepaintCallback_();
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(hwnd_, &paint);
      EndPaint(hwnd_, &paint);

      if (!minimized_ && resizeRepaintCallback_) {
        resizeRepaintCallback_();
      }
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      const int key = static_cast<int>(wParam);
      if (key >= 0 && static_cast<std::size_t>(key) < inputState_.keyboard.down.size()) {
        const bool pressed = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
        const int action = pressed ? (((lParam & (1LL << 30)) != 0) ? 2 : 1) : 0;
        inputState_.keyboard.down[static_cast<std::size_t>(key)] = (action != 0);

        if (eventDispatcher_ != nullptr) {
          eventDispatcher_->enqueue({
              .type = volt::event::EventType::kKeyInput,
              .payload = volt::event::KeyInputEvent{
                  .key = key,
                  .action = action,
                  .mods = modifierMask(),
              },
          });
        }
      }
      return 0;
    }
    case WM_CHAR: {
      if (eventDispatcher_ == nullptr) {
        return 0;
      }

      const char32_t codepoint = decodeWin32CharCodepoint(static_cast<wchar_t>(wParam), &pendingHighSurrogate_);
      if (codepoint == 0) {
        return 0;
      }

      eventDispatcher_->enqueue({
          .type = volt::event::EventType::kTextInput,
          .payload = volt::event::TextInputEvent{.codepoint = codepoint},
      });
      return 0;
    }
    case WM_MOUSEMOVE: {
      const double xPos = physicalToLogicalX(static_cast<double>(GET_X_LPARAM(lParam)));
      const double yPos = physicalToLogicalY(static_cast<double>(GET_Y_LPARAM(lParam)));

      inputState_.mouse.deltaX += (xPos - inputState_.mouse.x);
      inputState_.mouse.deltaY += (yPos - inputState_.mouse.y);
      inputState_.mouse.x = xPos;
      inputState_.mouse.y = yPos;

      if (eventDispatcher_ != nullptr) {
        eventDispatcher_->enqueue({
            .type = volt::event::EventType::kMouseMoved,
            .payload = volt::event::MouseMovedEvent{
                .x = inputState_.mouse.x,
                .y = inputState_.mouse.y,
                .deltaX = inputState_.mouse.deltaX,
                .deltaY = inputState_.mouse.deltaY,
            },
        });
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
      int button = -1;
      if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        button = 0;
      } else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) {
        button = 1;
      } else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) {
        button = 2;
      } else {
        const WORD xButton = GET_XBUTTON_WPARAM(wParam);
        button = (xButton == XBUTTON1) ? 3 : 4;
      }

      if (button >= 0 && static_cast<std::size_t>(button) < inputState_.mouse.down.size()) {
        const bool pressed =
            (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
             message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN);
        const int action = pressed ? 1 : 0;
        inputState_.mouse.down[static_cast<std::size_t>(button)] = pressed;

        if (eventDispatcher_ != nullptr) {
          eventDispatcher_->enqueue({
              .type = volt::event::EventType::kMouseButton,
              .payload = volt::event::MouseButtonEvent{
                  .button = button,
                  .action = action,
                  .mods = modifierMask(),
              },
          });
        }
      }

      if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) {
        return TRUE;
      }

      return 0;
    }
    case WM_MOUSEWHEEL: {
      inputState_.mouse.scrollY +=
          static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<double>(WHEEL_DELTA);

      if (eventDispatcher_ != nullptr) {
        eventDispatcher_->enqueue({
            .type = volt::event::EventType::kMouseScrolled,
            .payload = volt::event::MouseScrolledEvent{
                .xOffset = 0.0,
                .yOffset = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                    static_cast<double>(WHEEL_DELTA),
            },
        });
      }
      return 0;
    }
    case WM_MOUSEHWHEEL: {
      inputState_.mouse.scrollX +=
          static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<double>(WHEEL_DELTA);

      if (eventDispatcher_ != nullptr) {
        eventDispatcher_->enqueue({
            .type = volt::event::EventType::kMouseScrolled,
            .payload = volt::event::MouseScrolledEvent{
                .xOffset = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                    static_cast<double>(WHEEL_DELTA),
                .yOffset = 0.0,
            },
        });
      }
      return 0;
    }
    default:
      break;
  }

  return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK Win32WindowBackend::windowProcThunk(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
  if (message == WM_NCCREATE) {
    const auto* createInfo = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    auto* backend = static_cast<Win32WindowBackend*>(createInfo->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));
    if (backend != nullptr) {
      backend->hwnd_ = hwnd;
    }
  }

  auto* backend = reinterpret_cast<Win32WindowBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (backend != nullptr) {
    return backend->handleWindowMessage(message, wParam, lParam);
  }

  return DefWindowProcW(hwnd, message, wParam, lParam);
}
#endif

}  // namespace volt::platform::details
