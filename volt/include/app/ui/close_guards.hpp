#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// close guards
// -------------------------------------------------------------------------
//
// What something about to go asks first of what it holds. A window about to
// close, or the application about to exit, asks whatever in the window holds
// work that would be lost with it (an editor with changes, <app/ui/windows/
// part_editor.hpp>); the project about to be closed or replaced asks what
// was to be saved into it (app_services::project_guards()). Each guard is
// asked in turn, in the order they were added, and answers when it is ready,
// which may be after a dialog: true lets go (nothing to lose, saved, or
// discarded), false keeps it (Keep Editing), and the asking stops there.
//
//   m_guard = host.guards().add([this](close_guards::answer answer) {
//       if (!modified())
//       {
//           answer(true);
//           return;
//       }
//       ask_save_or_discard(std::move(answer)); // answers when the user does
//   });
//   ...
//   host.guards().remove(m_guard); // when it goes, or holds nothing more
//
//   guards.confirm([this] { close(); }); // runs once every guard let go
//
// An answer counts once. A guard removed before its turn is not asked (it
// went, and its work with it); one added while asking waits for the next
// time. If the list itself goes before the last answer (its window closed
// meanwhile), the rest count as letting go: nothing they held is left.
//
// Interface thread; nothing locks.

namespace volt::ui
{

    class close_guards
    {
    public:
        // Called once with the guard's decision.
        using answer = std::function<void(bool go)>;
        using guard = std::function<void(answer)>;

        close_guards() = default;
        close_guards(const close_guards &) = delete;
        close_guards &operator=(const close_guards &) = delete;

        // Returns the id to remove() it with; never 0.
        std::size_t add(guard g)
        {
            const std::size_t id = ++m_state->last_id;
            m_state->guards.push_back({id, std::move(g)});
            return id;
        }

        // Unknown ids (0, one removed already) are ignored.
        void remove(std::size_t id) noexcept
        {
            std::erase_if(m_state->guards, [id](const entry &e) { return e.id == id; });
        }

        bool empty() const noexcept { return m_state->guards.empty(); }
        std::size_t size() const noexcept { return m_state->guards.size(); }

        // Asks every guard, in order; `then` runs when each has let go: at
        // once (during the call) when there are none or all answer at once.
        // Not at all when one keeps it.
        void confirm(std::function<void()> then)
        {
            auto ids = std::make_shared<std::vector<std::size_t>>();
            for (const entry &e : m_state->guards)
                ids->push_back(e.id);
            ask(m_state, std::move(ids), 0, std::move(then));
        }

    private:
        struct entry
        {
            std::size_t id = 0;
            guard ask;
        };

        struct state
        {
            std::vector<entry> guards;
            std::size_t last_id = 0;
        };

        static void ask(const std::weak_ptr<state> &weak, std::shared_ptr<const std::vector<std::size_t>> ids,
                        std::size_t next, std::function<void()> then)
        {
            for (; next < ids->size(); ++next)
            {
                const std::shared_ptr<state> s = weak.lock();
                if (!s)
                    break; // the list went, and what it guarded with it
                const auto there = std::ranges::find_if(s->guards, [&](const entry &e) { return e.id == (*ids)[next]; });
                if (there == s->guards.end())
                    continue; // removed meanwhile

                // A copy: the guard may remove itself while it answers.
                const guard asking = there->ask;
                auto answered = std::make_shared<bool>(false);
                asking([weak, ids, next, then, answered](bool go) {
                    if (std::exchange(*answered, true) || !go)
                        return;
                    ask(weak, ids, next + 1, then);
                });
                return;
            }
            if (then)
                then();
        }

        std::shared_ptr<state> m_state = std::make_shared<state>();
    };

} // namespace volt::ui
