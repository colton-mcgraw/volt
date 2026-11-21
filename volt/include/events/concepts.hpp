#pragma once

#include <events/task.hpp>

#include <concepts>
#include <type_traits>

namespace volt::events
{

    /**
     * @concept listener_for
     * @brief Anything the bus accepts as a listener for `Event`: a callable taking `const Event &`.
     * @note What it returns decides how the bus treats it; see the refinements below. A
     *       return type that matches none of them is ignored and the listener never handles.
     * @tparam F The type of the listener function.
     * @tparam Event The type of event.
     */
    template <typename F, typename Event>
    concept listener_for = std::invocable<F &, const Event &>;

    /**
     * @concept handling_listener_for
     * @brief A synchronous listener that can stop the dispatch.
     * @note Returns true if it handled the event, in which case no lower-priority listener
     *       sees it; false to let the event carry on. The return type must be exactly bool.
     */
    template <typename F, typename Event>
    concept handling_listener_for =
        listener_for<F, Event> && std::same_as<std::invoke_result_t<F &, const Event &>, bool>;

    /**
     * @concept coroutine_listener_for
     * @brief A listener that is a coroutine; it is run by dispatch_async rather than dispatch.
     * @note Returns task<void>, or task<bool> with the same meaning as handling_listener_for.
     */
    template <typename F, typename Event>
    concept coroutine_listener_for =
        listener_for<F, Event> && (std::same_as<std::invoke_result_t<F &, const Event &>, task<void>> ||
                                   std::same_as<std::invoke_result_t<F &, const Event &>, task<bool>>);

    /**
     * @concept handling_coroutine_listener_for
     * @brief A coroutine listener that can stop the dispatch by resolving to true.
     */
    template <typename F, typename Event>
    concept handling_coroutine_listener_for =
        listener_for<F, Event> && std::same_as<std::invoke_result_t<F &, const Event &>, task<bool>>;

} // namespace volt::events
