#include "../math/check.hpp"

#include <app/ui/message_log.hpp>

#include <format>
#include <string>

// The log of every status message the windows have shown, which the project
// manager lists: repeats counted, levels kept, the oldest dropped.

using namespace volt::ui;

namespace
{
    void repeats_and_levels()
    {
        message_log log;
        int added = 0;
        int repeated = 0;
        log.added.connect([&](const logged_message &, bool again) { (again ? repeated : added) += 1; });

        log.add(window_kind::board_editor, "Loading a.step\xE2\x80\xA6");
        log.add(window_kind::board_editor, "Cannot open a.step", log_level::error);
        log.add(window_kind::board_editor, "Cannot open a.step");
        log.add(window_kind::schematic_editor, "Cannot open a.step"); // another window: an entry of its own
        log.add(window_kind::board_editor, "");                        // nothing to keep
        CHECK(log.messages().size() == 3);
        CHECK(added == 3 && repeated == 1);

        const logged_message &second = log.messages()[1];
        CHECK(second.repeats == 2);
        CHECK(second.level == log_level::error); // the more serious of the two
        CHECK(log.messages()[2].from == window_kind::schematic_editor);
        CHECK(log.messages()[2].level == log_level::info);

        bool cleared = false;
        log.cleared.connect([&] { cleared = true; });
        log.clear();
        CHECK(cleared && log.messages().empty());
    }

    void capacity()
    {
        message_log log;
        for (std::size_t i = 0; i < message_log::capacity + 5; ++i)
            log.add(window_kind::project_manager, std::format("Message {}", i));
        CHECK(log.messages().size() == message_log::capacity);
        CHECK(log.messages().front().text == "Message 5");
        CHECK(log.messages().back().text == std::format("Message {}", message_log::capacity + 4));
    }
} // namespace

int main()
{
    repeats_and_levels();
    capacity();
    return volt::tests::result("app message log");
}
