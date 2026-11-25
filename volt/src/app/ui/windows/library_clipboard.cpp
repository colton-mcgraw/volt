#include <app/ui/windows/library_clipboard.hpp>

#include <string>
#include <utility>

#ifdef _WIN32
#include <platform/windows/win32.hpp>
#endif

namespace volt::ui
{
    namespace
    {
#ifdef _WIN32
        std::wstring widen(const std::string &text)
        {
            if (text.empty())
                return {};
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
            if (length > 0)
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
            return wide;
        }

        std::string narrow(const wchar_t *text)
        {
            const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (length <= 1)
                return {};
            std::string utf8(static_cast<std::size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), length, nullptr, nullptr);
            utf8.pop_back(); // the terminator
            return utf8;
        }

        // The clipboard is opened without an owner window: the
        // application's windows belong to the platform thread, and setting
        // text needs none (the data is owned by the system once set).
        class win32_clipboard final : public clipboard
        {
        public:
            std::string text() const override
            {
                if (!OpenClipboard(nullptr))
                    return {};
                std::string result;
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr)
                {
                    if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(data)); wide != nullptr)
                    {
                        result = narrow(wide);
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
                return result;
            }

            void set_text(std::string text) override
            {
                const std::wstring wide = widen(text);
                const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (memory == nullptr)
                    return;
                if (auto *target = static_cast<wchar_t *>(GlobalLock(memory)); target != nullptr)
                {
                    std::copy(wide.begin(), wide.end(), target);
                    target[wide.size()] = L'\0';
                    GlobalUnlock(memory);
                }
                if (!OpenClipboard(nullptr))
                {
                    GlobalFree(memory);
                    return;
                }
                EmptyClipboard();
                if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
                    GlobalFree(memory); // still ours when the system did not take it
                CloseClipboard();
            }
        };
#endif
    } // namespace

    std::unique_ptr<clipboard> make_system_clipboard()
    {
#ifdef _WIN32
        return std::make_unique<win32_clipboard>();
#else
        return std::make_unique<memory_clipboard>();
#endif
    }

    bool reaches_other_applications(const clipboard &board) noexcept
    {
        return dynamic_cast<const memory_clipboard *>(&board) == nullptr;
    }

} // namespace volt::ui
