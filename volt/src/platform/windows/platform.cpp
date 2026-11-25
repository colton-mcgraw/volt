#include <platform/platform.hpp>
#include <platform/windows/window.hpp>

#include <objbase.h>  // CoInitializeEx, for ShellExecuteW
#include <shellapi.h> // ShellExecuteW; left out by WIN32_LEAN_AND_MEAN

#include <cassert>
#include <format>

namespace platform::windows
{
    namespace
    {
        std::unexpected<std::string> last_error(std::string_view what)
        {
            const DWORD code = GetLastError();
            return std::unexpected(std::format("{} failed ({}): {}", what, code, error_message(code)));
        }
    } // namespace

    std::wstring to_wide(std::string_view utf8)
    {
        if (utf8.empty())
            return {};

        const int source_size = static_cast<int>(utf8.size());
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), source_size, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), source_size, wide.data(), size);
        return wide;
    }

    std::string to_utf8(std::wstring_view wide)
    {
        if (wide.empty())
            return {};

        const int source_size = static_cast<int>(wide.size());
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), source_size, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), source_size, utf8.data(), size, nullptr, nullptr);
        return utf8;
    }

    std::string error_message(DWORD code)
    {
        wchar_t buffer[512];
        const DWORD size = FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            code,
            0,
            buffer,
            static_cast<DWORD>(std::size(buffer)),
            nullptr);

        std::wstring_view text(buffer, size);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.remove_suffix(1);

        return text.empty() ? "unknown error" : to_utf8(text);
    }

} // namespace platform::windows

namespace platform
{

    std::expected<state, std::string> initialize()
    {
        // Must happen before the first window exists. It fails with access
        // denied when the awareness was already fixed (by an application
        // manifest, for example), which is fine: the process keeps that setting.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        state result;
        result.instance = GetModuleHandleW(nullptr);
        if (result.instance == nullptr)
            return windows::last_error("GetModuleHandleW");

        // One class serves every window; what differs between windows hangs
        // off the HWND instead (see window_procedure).
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(WNDCLASSEXW);
        window_class.lpfnWndProc = windows::window_procedure;
        window_class.hInstance = result.instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // The executable's icon, at the sizes the system shows in title bars
        // and Alt+Tab. Shared: the system owns them and frees them with the
        // module. Null (the default icon) where the executable has none.
        const UINT system_dpi = GetDpiForSystem();
        const auto load_icon = [&](int width_metric, int height_metric) {
            return static_cast<HICON>(LoadImageW(result.instance, MAKEINTRESOURCEW(windows::app_icon_resource),
                                                 IMAGE_ICON, GetSystemMetricsForDpi(width_metric, system_dpi),
                                                 GetSystemMetricsForDpi(height_metric, system_dpi), LR_SHARED));
        };
        window_class.hIcon = load_icon(SM_CXICON, SM_CYICON);
        window_class.hIconSm = load_icon(SM_CXSMICON, SM_CYSMICON);
        // Paints the content area black until a renderer draws into it.
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        window_class.lpszClassName = windows::window_class_name;

        result.window_class = RegisterClassExW(&window_class);
        if (result.window_class == 0)
            return windows::last_error("RegisterClassExW");

        // Manual reset: poll_events() clears it itself, whether the event or
        // a message is what woke it (see state::wake_event).
        result.wake_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (result.wake_event == nullptr)
        {
            auto error = windows::last_error("CreateEventW");
            shutdown(result);
            return error;
        }

        result.thread_id = GetCurrentThreadId();
        return result;
    }

    void shutdown(state &state)
    {
        if (state.wake_event != nullptr)
            CloseHandle(state.wake_event);

        if (state.window_class != 0)
            UnregisterClassW(windows::window_class_name, state.instance);

        state = {};
    }

    void wake(const state &state)
    {
        if (state.wake_event != nullptr)
            SetEvent(state.wake_event);
    }

    bool poll_events(const state &state)
    {
        assert(GetCurrentThreadId() == state.thread_id && "poll_events must run on the thread that called initialize");

        // Sleep until there is a message or a wake. MWMO_INPUTAVAILABLE wakes
        // for any message in the queue, also one that somebody has already
        // looked at without taking it; WaitMessage would only wake for new
        // ones. Sent messages from other threads count too, and are delivered
        // by the PeekMessageW below.
        MsgWaitForMultipleObjectsEx(1, &state.wake_event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        // Taken whatever woke the thread: this call returns true either way,
        // and the caller then looks at its work. A wake that comes after this
        // sets the event again and ends the next wait at once, so none is
        // lost, including one that comes while a message dispatched below
        // holds the thread in a nested loop for the length of a drag: this
        // call returns when the loop is over, and the next one at once.
        ResetEvent(state.wake_event);

        // This loop only routes: each message goes to the window procedure of
        // the window it was sent to. Nothing is interpreted here, because
        // Windows runs its own nested loops on this queue (while a window is
        // being dragged or resized, inside menus and message boxes) and those
        // dispatch to the same window procedures without passing through here.
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            // WM_QUIT is the one message addressed to the thread rather than a
            // window, so no window procedure ever sees it.
            if (message.message == WM_QUIT)
                return false;

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        return true;
    }

    std::expected<void, std::string> open_url(std::string_view url)
    {
        if (!is_web_url(url))
            return std::unexpected(std::string("not a web address (http or https)"));

        // ShellExecuteW may hand the request to shell extensions, which
        // want COM on the calling thread. Initialized here if this thread
        // has not done it, and undone after; a thread that has it in the
        // other threading model keeps that, which works as well.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        // "open" on an http(s) address is the default browser. The result
        // is an error code when 32 or less.
        const std::wstring wide = windows::to_wide(url);
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        const DWORD code = GetLastError();

        if (SUCCEEDED(com))
            CoUninitialize();
        if (result <= 32)
            return std::unexpected(std::format("ShellExecuteW failed ({}): {}", code, windows::error_message(code)));
        return {};
    }

} // namespace platform
