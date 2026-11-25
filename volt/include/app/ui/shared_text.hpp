#pragma once

#include <ui/text.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// shared_text
// -------------------------------------------------------------------------
//
// The one text engine every window's screen measures and draws with, and so
// the one glyph atlas the renderer mirrors (<ui/text.hpp>, <app/ui/
// renderer.hpp>): one set of glyph pages for all windows, uploaded once per
// frame. It forwards to the real engine and counts the atlas resets.
//
// Why count. screen::paint() resets the atlas when it overflows and paints
// its own screen again, so its own list is whole. But with several windows
// painted in one frame, a reset while painting the second window empties
// the pages the first window's list, painted moments before, points into.
// The render loop therefore notes resets() after painting each window and,
// once all are painted, paints again every window painted before the last
// reset (paint_consistently() below; volt_app::draw()
// calls it).
//
// Interface thread only, like the engine.

namespace volt::ui
{

    class shared_text final : public text_engine
    {
    public:
        explicit shared_text(text_engine &engine) noexcept : m_engine(engine) {}

        // How many times the atlas has been reset through this engine.
        std::uint64_t resets() const noexcept { return m_resets; }

        text_metrics metrics(const text_style &style) override { return m_engine.metrics(style); }

        font::shaped_run shape(std::string_view text, const text_style &style) override
        {
            return m_engine.shape(text, style);
        }

        float measure(std::string_view text, const text_style &style) override { return m_engine.measure(text, style); }

        font::text_layout layout(std::string_view text, const text_style &style,
                                 const font::layout_options &options) override
        {
            return m_engine.layout(text, style, options);
        }

        void draw(draw_list &list, const font::text_layout &layout, const text_style &style,
                  const math::vec2f &origin, float scale, const math::vec4f &color) override
        {
            m_engine.draw(list, layout, style, origin, scale, color);
        }

        void draw(draw_list &list, const font::shaped_run &run, const text_style &style, const math::vec2f &baseline,
                  float scale, const math::vec4f &color) override
        {
            m_engine.draw(list, run, style, baseline, scale, color);
        }

        bool overflowed() const noexcept override { return m_engine.overflowed(); }

        void reset() override
        {
            m_engine.reset();
            ++m_resets;
        }

    private:
        text_engine &m_engine;
        std::uint64_t m_resets = 0;
    };

    // Paints `count` windows, `paint(i)` painting window i (screen::paint()
    // into its list), so that every list refers to the atlas as it is at the
    // end: a window painted before a reset is painted again, until a round
    // goes by without one or `rounds` are used up. Returns whether every
    // list is good; false means one frame's glyphs of all windows together
    // do not fit the atlas, and some text shows wrong for that frame.
    template <typename Paint>
    bool paint_consistently(shared_text &text, std::size_t count, Paint &&paint, int rounds = 3)
    {
        constexpr std::uint64_t never = ~std::uint64_t{0};
        std::vector<std::uint64_t> painted(count, never);
        for (int round = 0; round <= rounds; ++round)
        {
            const std::uint64_t now = text.resets();
            bool stale = false;
            for (std::size_t i = 0; i < count; ++i)
                stale = stale || painted[i] != now;
            if (!stale)
                return true;
            if (round == rounds)
                break;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (painted[i] == text.resets())
                    continue;
                paint(i);
                painted[i] = text.resets();
            }
        }
        return false;
    }

} // namespace volt::ui
