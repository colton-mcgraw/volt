#pragma once

#include <gpu/presenter.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace volt::gpu
{

    // ---------------------------------------------------------------------
    // retire_queue
    // ---------------------------------------------------------------------
    //
    // Frees what the frames in flight may still be using, once they are
    // through: a mesh taken off the board, an image nobody shows any more.
    // Waiting for the queue to go idle would do too, but stalls the frame
    // loop; instead the owner hands over a function that frees the object,
    // along with the number (frame::number) of the last frame that may have
    // used it, and collect() runs that function at the start of the first
    // frame for which the presenter has already waited on that one.
    //
    // What a function frees must not be reachable from anything recorded
    // afterwards: an owner retires an object only once it has stopped
    // handing it out.
    //
    // One thread, the one that records frames, between frames or at the
    // start of one.
    class retire_queue
    {
    public:
        retire_queue() = default;

        // Whatever is still queued is freed: by then the owner has waited
        // for the device (see flush()).
        ~retire_queue() { flush(); }

        retire_queue(const retire_queue &) = delete;
        retire_queue &operator=(const retire_queue &) = delete;

        // `release` runs once the frame numbered `last_use` is through the
        // GPU. 0 stands for no frame at all: the object was never used, and
        // goes at the next collect().
        void retire(std::uint64_t last_use, std::move_only_function<void()> release);

        // Runs everything that `frame`, which has just begun, makes safe to
        // free. Call before recording anything into it.
        void collect(const frame &frame);

        // Runs everything still queued. Only once the device is idle: on
        // the way out, after a wait_idle().
        void flush();

        std::size_t size() const noexcept { return m_entries.size(); }

    private:
        struct entry
        {
            std::uint64_t last_use = 0;
            std::move_only_function<void()> release;
        };

        std::vector<entry> m_entries;
    };

} // namespace volt::gpu
