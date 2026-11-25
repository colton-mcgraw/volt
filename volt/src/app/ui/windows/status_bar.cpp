#include <app/ui/windows/status_bar.hpp>

#include <ui/widgets/label.hpp>
#include <ui/widgets/progress_bar.hpp>

namespace volt::ui
{

    window_status::~window_status()
    {
        // The timer's callback points here; the screen may outlive this by
        // a moment.
        if (m_timer != 0)
            m_screen.cancel(m_timer);
    }

    std::unique_ptr<status_bar> window_status::build(const std::function<void(status_bar &)> &own)
    {
        auto bar = std::make_unique<status_bar>();
        bar->set_name("status bar");
        if (own)
        {
            own(*bar);
            bar->add_separator();
        }

        // The message takes what is left, and is cut short when that is not
        // enough.
        m_message = &bar->add_segment("");
        m_message->set_name("status message");
        m_message->set_role(text_role::normal);
        m_message->layout().flex = 1.0f;

        auto &progress = bar->emplace<progress_bar>();
        progress.layout().width = 70.0f;
        progress.layout().cross = align::center;
        progress.set_visible(false);
        m_progress = &progress;
        bar->add_separator();

        m_frame = &bar->add_segment("");
        m_frame->set_name("frame status");
        return bar;
    }

    void window_status::show_message(std::string text, std::chrono::seconds duration)
    {
        if (m_message == nullptr)
            return;
        m_message->set_text(std::move(text));
        if (m_timer != 0)
            m_screen.cancel(m_timer);

        // From the clock, not the screen's time: input (a drop) is handled
        // before update() brings that up to date, and after a while of
        // sleeping it is long past.
        m_timer = m_screen.schedule(clock::now() + duration, [this] {
            m_timer = 0;
            if (m_message != nullptr)
                m_message->set_text("");
        });
    }

    void window_status::set_frame_info(std::string text)
    {
        // Setting the same text changes nothing, so this may run every frame.
        if (m_frame != nullptr)
            m_frame->set_text(std::move(text));
    }

    void window_status::set_busy(bool busy)
    {
        if (m_progress == nullptr || m_progress->visible() == busy)
            return;
        m_progress->set_visible(busy);
        m_progress->set_indeterminate(busy);
    }

} // namespace volt::ui
