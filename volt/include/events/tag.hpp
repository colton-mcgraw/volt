#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace volt::events
{

    // ------------------------------------------------------------
    // Event identity
    // ------------------------------------------------------------
    //
    // Every event type is identified by the address of a one-byte variable the
    // compiler instantiates for it. Nothing is declared on the event itself: no
    // base class, no hand-numbered id, no registration call. Any type is an
    // event the moment it is named in add_listener or dispatch, and the linker
    // guarantees the identity is unique within the program and can never
    // collide.
    //
    // The address is a constant expression, so an event_key costs nothing to
    // produce: no RTTI, no type-name hashing and no initialisation guard.
    //
    // Limit: identity is per linked image. A Windows DLL and the executable
    // that loads it each get their own copy of the variable, so a bus must not
    // be shared across that boundary unless the instantiations are exported
    // from one module.

    namespace detail
    {
        /**
         * @brief The variable whose address identifies `Event`.
         * @note Deliberately not const. Linkers that fold identical read-only data
         *       (MSVC and lld-link with /OPT:ICF, the default in release builds)
         *       would be free to give every event type the same address; writable
         *       data is never folded.
         */
        template <typename Event>
        inline char type_anchor = 0;
    } // namespace detail

    class event_key;

    template <typename Event>
    constexpr event_key event_id() noexcept;

    /**
     * @class event_key
     * @brief Identifies an event type. Cheap to copy and compare; obtain one from event_id<Event>().
     * @note A default-constructed key identifies no event type.
     */
    class event_key
    {
    public:
        constexpr event_key() noexcept = default;

        /// True if this key identifies an event type.
        constexpr explicit operator bool() const noexcept { return id_ != nullptr; }

        friend constexpr bool operator==(event_key, event_key) noexcept = default;

        /// A well-mixed hash, suitable for masking down to a table index.
        std::size_t hash() const noexcept
        {
            // Anchors are packed a byte apart in one section, so the raw address
            // has almost no entropy in any fixed bit range. A Fibonacci multiply
            // spreads it across the word; folding the high half down makes the
            // low bits usable.
            auto x = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(id_));
            x *= 0x9e3779b97f4a7c15ull;
            return static_cast<std::size_t>(x ^ (x >> 32));
        }

    private:
        template <typename Event>
        friend constexpr event_key event_id() noexcept;

        constexpr explicit event_key(const char *id) noexcept : id_(id) {}

        const char *id_ = nullptr;
    };

    /**
     * @variable invalid_event_key
     * @brief A key that identifies no event type.
     */
    inline constexpr event_key invalid_event_key{};

    /**
     * @brief Retrieves the key for an event type.
     * @tparam Event The event type. cv-qualifiers and references are ignored.
     */
    template <typename Event>
    constexpr event_key event_id() noexcept
    {
        return event_key{&detail::type_anchor<std::remove_cvref_t<Event>>};
    }

} // namespace volt::events

template <>
struct std::hash<volt::events::event_key>
{
    std::size_t operator()(volt::events::event_key key) const noexcept { return key.hash(); }
};
