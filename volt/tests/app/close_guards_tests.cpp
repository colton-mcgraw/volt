#include "../math/check.hpp"

#include <app/ui/close_guards.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// What a window about to close, the application about to exit, or the
// project about to go asks first (<app/ui/close_guards.hpp>): each guard in
// turn, answering at once or later (after a dialog), the first that keeps
// it stopping the rest, guards that went meanwhile skipped, and a list that
// went with its window counting as let go.

using volt::ui::close_guards;

namespace
{
    // A guard that answers when told to, as one behind a dialog does.
    struct pending_guard
    {
        std::vector<std::string> *log = nullptr;
        std::string name;
        std::vector<close_guards::answer> asked;

        close_guards::guard make()
        {
            return [this](close_guards::answer answer) {
                log->push_back(name);
                asked.push_back(std::move(answer));
            };
        }

        void answer(bool go)
        {
            const close_guards::answer a = asked.back();
            a(go);
        }
    };

    close_guards::guard answering(std::vector<std::string> &log, std::string name, bool go)
    {
        return [&log, name, go](close_guards::answer answer) {
            log.push_back(name);
            answer(go);
        };
    }

    void none()
    {
        close_guards guards;
        CHECK(guards.empty());
        int ran = 0;
        guards.confirm([&] { ++ran; });
        CHECK(ran == 1); // at once
        guards.confirm({}); // nothing to run is fine
        guards.remove(0);
        guards.remove(42);
        CHECK(guards.empty());
    }

    void at_once()
    {
        std::vector<std::string> log;
        close_guards guards;
        const std::size_t a = guards.add(answering(log, "a", true));
        const std::size_t b = guards.add(answering(log, "b", true));
        CHECK(a != 0 && b != 0 && a != b);
        CHECK(guards.size() == 2);

        int ran = 0;
        guards.confirm([&] { ++ran; });
        CHECK(ran == 1);
        CHECK((log == std::vector<std::string>{"a", "b"}));

        // The first that keeps it stops the asking.
        log.clear();
        guards.remove(a);
        guards.add(answering(log, "keeps", false));
        guards.add(answering(log, "after", true));
        guards.confirm([&] { ++ran; });
        CHECK(ran == 1);
        CHECK((log == std::vector<std::string>{"b", "keeps"}));
    }

    void later()
    {
        std::vector<std::string> log;
        close_guards guards;
        pending_guard first{&log, "first", {}};
        pending_guard second{&log, "second", {}};
        guards.add(first.make());
        guards.add(second.make());

        int ran = 0;
        guards.confirm([&] { ++ran; });
        CHECK((log == std::vector<std::string>{"first"})); // the second waits its turn
        CHECK(ran == 0);
        first.answer(true);
        CHECK((log == std::vector<std::string>{"first", "second"}));
        CHECK(ran == 0);
        second.answer(true);
        CHECK(ran == 1);

        // An answer counts once.
        second.answer(true);
        first.answer(true);
        CHECK(ran == 1);

        // Kept: nothing runs, then or later.
        log.clear();
        guards.confirm([&] { ++ran; });
        first.answer(false);
        first.answer(true);
        CHECK(ran == 1);
        CHECK((log == std::vector<std::string>{"first"}));
    }

    void changing_while_asking()
    {
        std::vector<std::string> log;
        close_guards guards;
        pending_guard first{&log, "first", {}};
        guards.add(first.make());
        const std::size_t gone = guards.add(answering(log, "gone", true));

        int ran = 0;
        guards.confirm([&] { ++ran; });

        // Removed before its turn (its editor closed): not asked. Added
        // meanwhile: not asked this time.
        guards.remove(gone);
        guards.add(answering(log, "new", false));
        first.answer(true);
        CHECK(ran == 1);
        CHECK((log == std::vector<std::string>{"first"}));

        // A guard that removes itself while answering (an editor that closes
        // when it lets go).
        close_guards own;
        std::size_t self = 0;
        self = own.add([&](close_guards::answer answer) {
            own.remove(self);
            answer(true);
        });
        own.add(answering(log, "next", true));
        own.confirm([&] { ++ran; });
        CHECK(ran == 2);
        CHECK(own.size() == 1);
    }

    void list_gone()
    {
        // The window closed while its guard was asking: what waited goes on.
        std::vector<std::string> log;
        pending_guard first{&log, "first", {}};
        int ran = 0;
        {
            auto guards = std::make_unique<close_guards>();
            guards->add(first.make());
            guards->add(answering(log, "never", true));
            guards->confirm([&] { ++ran; });
        }
        CHECK(ran == 0);
        first.answer(true);
        CHECK(ran == 1);
        CHECK((log == std::vector<std::string>{"first"}));
    }
} // namespace

int main()
{
    none();
    at_once();
    later();
    changing_while_asking();
    list_gone();
    return volt::tests::result("close guards");
}
