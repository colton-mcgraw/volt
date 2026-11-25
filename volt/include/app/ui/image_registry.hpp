#pragma once

#include <ui/draw_list.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// -------------------------------------------------------------------------
// image_registry
// -------------------------------------------------------------------------
//
// Who has which index of texture_id::image(): the views' render targets,
// the logo, images opened by the user. An index is handed out once and
// given back when its image goes; the lowest free one is handed out next,
// so the renderer's table of images (renderer::set_image()) stays as short
// as the most images ever shown at once.
//
// Only the numbers: the GPU side lives in <app/ui/images.hpp>. One thread,
// the interface thread.

namespace volt::ui
{

    class image_registry
    {
    public:
        texture_id allocate()
        {
            for (std::size_t i = 0; i < m_used.size(); ++i)
            {
                if (!m_used[i])
                {
                    m_used[i] = true;
                    ++m_count;
                    return texture_id::image(static_cast<std::uint32_t>(i));
                }
            }
            m_used.push_back(true);
            ++m_count;
            return texture_id::image(static_cast<std::uint32_t>(m_used.size() - 1));
        }

        // Ignores ids that are not images, or not handed out.
        void release(texture_id id)
        {
            if (!contains(id))
                return;
            m_used[id.index] = false;
            --m_count;
        }

        bool contains(texture_id id) const noexcept
        {
            return id.kind == texture_kind::image && id.index < m_used.size() && m_used[id.index];
        }

        // Indices in use, and how many the table needs to hold them.
        std::size_t size() const noexcept { return m_count; }
        std::size_t capacity() const noexcept { return m_used.size(); }

    private:
        std::vector<bool> m_used;
        std::size_t m_count = 0;
    };

} // namespace volt::ui
