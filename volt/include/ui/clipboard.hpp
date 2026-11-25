#pragma once

#include <string>
#include <utility>

// -------------------------------------------------------------------------
// clipboard
// -------------------------------------------------------------------------
//
// Where cut, copy and paste go. The platform layer has no clipboard yet, so
// the interface takes one through this interface (screen::set_clipboard()),
// and until the platform provides one, memory_clipboard keeps the text
// within the process. A platform clipboard implements the same two calls:
// on Windows OpenClipboard / GetClipboardData(CF_UNICODETEXT) converted to
// UTF-8, on Wayland a wl_data_device selection.
//
// Text is UTF-8. Called on the interface's thread.

namespace volt::ui
{

    class clipboard
    {
    public:
        virtual ~clipboard() = default;

        virtual std::string text() const = 0;
        virtual void set_text(std::string text) = 0;
    };

    class memory_clipboard final : public clipboard
    {
    public:
        std::string text() const override { return m_text; }
        void set_text(std::string text) override { m_text = std::move(text); }

    private:
        std::string m_text;
    };

} // namespace volt::ui
