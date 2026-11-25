#pragma once

#include <app/ui/windows/library_model.hpp>
#include <app/ui/worker.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// part_search
// -------------------------------------------------------------------------
//
// Searching a large catalog off the interface thread, newest query wins:
// what the Library Browser and the parts panel both do with a catalog too
// big to search between two key presses (in a Debug build 50 000 parts take
// tens of milliseconds, the first letter's search, which most parts match,
// over a hundred).
//
// start() hands the search to a thread of its own (a worker, <app/ui/
// worker.hpp>); the hits come back through `post` to the interface thread,
// where `done` gets them, unless a newer search was started meanwhile, or
// cancel() was called, or the part_search has gone: then they are dropped,
// and a search still waiting behind the running one is not even run. A
// catalog is read-only, so searching it on another thread is safe; the
// shared_ptr keeps it alive while it is searched.
//
// Interface thread, apart from what runs on the worker. The destructor
// waits for a search that is running (it cannot be interrupted).

namespace volt::ui
{

    class part_search
    {
    public:
        // Catalogs with more parts than this are searched in the
        // background; smaller ones are quicker searched where they are.
        static constexpr std::size_t background_parts = 20000;

        // The hits of `filter` in `catalog`, ranked, and how long the
        // search took, milliseconds.
        using finished = std::function<void(const std::shared_ptr<const library::catalog> &catalog,
                                            part_filter filter, std::vector<library::part_ref> ranked,
                                            double milliseconds)>;

        // `post` runs a function on the interface thread, from any thread
        // (app_services::post()).
        explicit part_search(std::function<void(std::function<void()>)> post) : m_post(std::move(post)) {}

        ~part_search() { cancel(); }

        part_search(const part_search &) = delete;
        part_search &operator=(const part_search &) = delete;

        // Whether `list`'s catalog is big enough to be searched here.
        static bool wanted(const part_list &list) noexcept { return list.total() > background_parts; }

        void start(std::shared_ptr<const library::catalog> catalog, part_filter filter, finished done)
        {
            const std::uint64_t generation = ++*m_generation;
            m_worker.push([post = m_post, catalog = std::move(catalog), filter = std::move(filter),
                           done = std::move(done), generation, latest = m_generation]() mutable {
                // Typing has moved on: a newer search is queued behind this one.
                if (catalog == nullptr || latest->load() != generation)
                    return;
                const auto start = std::chrono::steady_clock::now();
                std::vector<library::part_ref> ranked = part_list::run_search(*catalog, filter);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                post([catalog = std::move(catalog), filter = std::move(filter), ranked = std::move(ranked), ms,
                      done = std::move(done), generation, latest = std::move(latest)]() mutable {
                    if (latest->load() != generation)
                        return;
                    done(catalog, std::move(filter), std::move(ranked), ms);
                });
            });
        }

        // The searches started so far report nothing.
        void cancel() noexcept { ++*m_generation; }

    private:
        std::function<void(std::function<void()>)> m_post;

        // A result is used only if no newer search was started meanwhile.
        // Shared with the jobs and the posted functions, which may outlive
        // this object.
        std::shared_ptr<std::atomic<std::uint64_t>> m_generation = std::make_shared<std::atomic<std::uint64_t>>(0);

        // Last, so that it stops (joins) before the rest goes.
        worker m_worker;
    };

} // namespace volt::ui
