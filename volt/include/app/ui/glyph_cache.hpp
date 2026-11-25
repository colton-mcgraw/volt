#pragma once

#include <gpu/presenter.hpp>
#include <gpu/texture.hpp>

#include <font/atlas.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace volt::gpu
{
    class context;
} // namespace volt::gpu

namespace volt::ui
{

    // ---------------------------------------------------------------------
    // glyph_cache
    // ---------------------------------------------------------------------
    //
    // The GPU's copy of a glyph atlas (<font/atlas.hpp>): one R8 texture per
    // page, which the draw list's glyph page ids (<ui/draw_list.hpp>) name by
    // index. The atlas itself lives with the text engine, on the CPU, and
    // grows as text is drawn; upload() brings the textures up to date with
    // it once per frame, copying only the rectangle of each page that
    // changed (atlas::take_dirty()).
    //
    // When the atlas is cleared, its pages start over entirely dirty, so the
    // next upload() replaces them whole; the textures themselves are kept
    // and reused, since destroying one waits for the GPU. A page the atlas
    // no longer has keeps its texture, unused, until destroy().
    //
    // Render thread, like the renderer that owns it.
    class glyph_cache
    {
    public:
        void create(gpu::context &context) { m_context = &context; }
        void destroy() { m_pages.clear(); }

        // Records the uploads for everything that changed in `atlas` since
        // the last call into `frame`. Before the pass that draws the glyphs.
        std::expected<void, std::string> upload(const gpu::frame &frame, font::atlas &atlas);

        // The page's view, or null when it has none yet.
        VkImageView view(std::size_t page) const noexcept;

    private:
        gpu::context *m_context = nullptr;
        std::vector<std::unique_ptr<gpu::texture>> m_pages;
    };

} // namespace volt::ui
