#pragma once

#include <events/concepts.hpp>
#include <events/detail/registry.hpp>
#include <events/tag.hpp>
#include <events/task.hpp>
#include <events/token.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace volt::events
{

    // ------------------------------------------------------------
    // Event Bus
    // ------------------------------------------------------------
    //
    // Events: any type is an event. There is nothing to derive from and no id
    // to assign; see tag.hpp for how the compiler supplies the identity.
    //
    // Ordering: listeners run in descending priority; equal priorities run in
    // registration order.
    //
    // Handling: a listener that returns bool can stop a dispatch. Returning
    // true means "handled": no lower-priority listener sees the event, and
    // dispatch itself returns true. Listeners that return nothing never stop
    // it. Give the listeners that may consume an event (a modal dialog, an
    // active tool) a higher priority than the ones they should shadow.
    //
    // Listeners receive the caller's event itself, never a copy, and cannot
    // modify it.
    //
    // Synchronous and coroutine listeners are separate populations: dispatch
    // runs only the former, dispatch_async only the latter.
    //
    // Threading: decided at construction; see `threading`. The default,
    // threading::multi, allows every member function, and every token
    // operation, to be called concurrently from any thread. With
    // threading::single the caller promises one thread at a time, and a
    // dispatch gets cheaper by the cost of two locked increments. Everything
    // below holds for both.
    //
    // No lock is held while a listener runs, so listeners are free to register
    // or remove anything, or to dispatch again. A dispatch works on a snapshot
    // taken when it starts: registrations added during it are first seen by the
    // next dispatch, and a removal stops the listener from being started again,
    // even later in the same dispatch. An invocation already running on another
    // thread when remove() returns may still be in progress; the callable object
    // itself stays alive until every in-flight dispatch that captured it has
    // finished. Concurrent dispatches may invoke the same listener concurrently,
    // so listeners that share state must synchronise it themselves.
    //
    // Dispatch takes no lock and performs no reference counting. The cost moves
    // to the writers: add and remove take a mutex, and a registration removed
    // while any dispatch on this bus is in flight is freed by a later add or
    // remove, or by the bus's destructor, rather than the instant that dispatch
    // ends. See detail/registry.hpp for the mechanism.
    //
    // Lifetime: tokens may outlive the bus; they simply become invalid. The bus
    // must outlive its own in-flight dispatches.

    /**
     * @class bus
     * @brief Event bus: typed listeners, ordered by priority, with optional early-out.
     */
    class bus
    {
    public:
        explicit bus(threading mode = threading::multi) : state_(std::make_shared<detail::state>(mode)) {}
        bus(const bus &) = delete;
        bus &operator=(const bus &) = delete;

        // --------------------------------------------------------
        // Listeners
        // --------------------------------------------------------

        /**
         * @brief Registers a listener for `Event`.
         * @param f Called with `const Event &`. Its return type selects its role:
         *          `bool` handles (true stops the dispatch), `task<void>` and `task<bool>`
         *          are the coroutine equivalents run by dispatch_async, and anything else
         *          is a plain listener.
         * @param priority Higher runs first.
         */
        template <typename Event, typename F>
            requires listener_for<F, Event>
        token add_listener(F &&f, int priority = 0)
        {
            if constexpr (handling_coroutine_listener_for<F, Event>)
            {
                // The one shape that needs a frame of ours: something has to await the
                // listener's task<bool> to carry its answer out.
                return install<detail::async_listener_sig>(
                    detail::kind::async_listener, event_id<Event>(), priority,
                    [fn = std::forward<F>(f)](const void *ptr, bool *handled) mutable -> task<void>
                    { *handled = co_await fn(*static_cast<const Event *>(ptr)); });
            }
            else if constexpr (coroutine_listener_for<F, Event>)
            {
                return install<detail::async_listener_sig>(
                    detail::kind::async_listener, event_id<Event>(), priority,
                    [fn = std::forward<F>(f)](const void *ptr, bool *) mutable -> task<void>
                    { return fn(*static_cast<const Event *>(ptr)); });
            }
            else if constexpr (handling_listener_for<F, Event>)
            {
                return install<detail::sync_listener_sig>(detail::kind::sync_listener, event_id<Event>(), priority,
                                                          [fn = std::forward<F>(f)](const void *ptr) mutable -> bool
                                                          { return fn(*static_cast<const Event *>(ptr)); });
            }
            else
            {
                return install<detail::sync_listener_sig>(detail::kind::sync_listener, event_id<Event>(), priority,
                                                          [fn = std::forward<F>(f)](const void *ptr) mutable -> bool
                                                          {
                                                              fn(*static_cast<const Event *>(ptr));
                                                              return false;
                                                          });
            }
        }

        // --------------------------------------------------------
        // Dispatch
        // --------------------------------------------------------

        /**
         * @brief Runs the synchronous listeners for `Event` on the calling thread.
         * @return True if a listener handled the event and so cut the dispatch short.
         */
        template <typename Event>
        bool dispatch(const Event &e)
        {
            detail::read_guard guard{*state_};

            const detail::entry *en = state_->find(event_id<Event>());
            if (!en)
                return false;

            const auto *listeners = en->sync_listeners.load(std::memory_order_seq_cst);
            if (!listeners)
                return false;

            for (const auto &s : listeners->slots)
                if (detail::live(*s) && (*s)(std::addressof(e)))
                    return true;
            return false;
        }

        /**
         * @brief Runs the coroutine listeners for `Event`, awaiting each in turn.
         * @note Takes the event by value: the returned task owns it until it completes.
         * @return A task resolving to true if a listener handled the event.
         */
        template <typename Event>
        task<bool> dispatch_async(Event e)
        {
            detail::pinned<detail::async_listener_sig> listeners;
            {
                // The guard cannot be held across a suspension, so pin the list
                // with its own reference count and let the guard go.
                detail::read_guard guard{*state_};
                if (const detail::entry *en = state_->find(event_id<Event>()))
                    listeners = detail::pinned<detail::async_listener_sig>{
                        en->async_listeners.load(std::memory_order_seq_cst), state_->single};
            }

            if (const auto *list = listeners.get())
            {
                for (const auto &s : list->slots)
                {
                    if (!detail::live(*s))
                        continue;
                    bool handled = false;
                    co_await (*s)(std::addressof(e), &handled);
                    if (handled)
                        co_return true;
                }
            }
            co_return false;
        }

    private:
        template <typename Signature, typename Callable>
        token install(detail::kind k, event_key type, int priority, Callable &&callable)
        {
            auto s = detail::make_slot<Signature>(state_->next_id.fetch_add(1, std::memory_order_relaxed), priority,
                                                  std::forward<Callable>(callable));
            {
                std::lock_guard lock(state_->mutex);
                state_->add<Signature>(type, s);
            }
            return token{state_, s, k, type};
        }

        std::shared_ptr<detail::state> state_;
    };

} // namespace volt::events
