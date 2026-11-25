#include <schematic/stroke_font.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>

namespace volt::schematic
{

    namespace
    {
        // The glyphs are drawn on a grid of their own: the cap height is 10,
        // lowercase letters 7, descenders reach -3 and brackets 11, x from 0
        // to the glyph's width. finish() adds the space either side and
        // scales to a height of 1.
        constexpr float design_height = 10.0f;
        constexpr float side_space = 1.5f; // on each side of every glyph

        class glyph_builder
        {
        public:
            explicit glyph_builder(float width) : m_width(width) {}

            // Starts a polyline at a point.
            glyph_builder &move(float x, float y)
            {
                m_strokes.emplace_back();
                m_strokes.back().push_back(math::vec2f{x, y});
                return *this;
            }

            // Continues the polyline to a point.
            glyph_builder &line(float x, float y)
            {
                add(math::vec2f{x, y});
                return *this;
            }

            // Starts an empty polyline, for one that begins with an arc.
            glyph_builder &begin()
            {
                m_strokes.emplace_back();
                return *this;
            }

            // Continues the polyline along an elliptical arc about (cx, cy),
            // from `from` to `to` degrees (counter-clockwise when to > from),
            // joined by a straight piece to where it was.
            glyph_builder &arc(float cx, float cy, float rx, float ry, float from, float to)
            {
                if (m_strokes.empty())
                    m_strokes.emplace_back();
                const float sweep = to - from;
                const int pieces = std::max(2, static_cast<int>(std::ceil(std::fabs(sweep) / 20.0f)));
                for (int i = 0; i <= pieces; ++i)
                {
                    const float degrees = from + sweep * static_cast<float>(i) / static_cast<float>(pieces);
                    const float radians = degrees * std::numbers::pi_v<float> / 180.0f;
                    add(math::vec2f{cx + rx * std::cos(radians), cy + ry * std::sin(radians)});
                }
                return *this;
            }

            // A closed ellipse of its own.
            glyph_builder &ellipse(float cx, float cy, float rx, float ry)
            {
                begin();
                arc(cx, cy, rx, ry, 0.0f, 360.0f);
                m_strokes.back().back() = m_strokes.back().front(); // closed exactly
                return *this;
            }

            // A dot: a polyline of one point.
            glyph_builder &dot(float x, float y) { return move(x, y); }

            stroke_glyph finish()
            {
                stroke_glyph g;
                g.advance = (m_width + 2.0f * side_space) / design_height;
                for (std::vector<math::vec2f> &stroke : m_strokes)
                {
                    if (stroke.empty())
                        continue;
                    for (math::vec2f &p : stroke)
                        p = math::vec2f{(p.x() + side_space) / design_height, p.y() / design_height};
                    g.strokes.push_back(std::move(stroke));
                }
                return g;
            }

        private:
            void add(math::vec2f p)
            {
                if (m_strokes.empty())
                    m_strokes.emplace_back();
                std::vector<math::vec2f> &stroke = m_strokes.back();
                if (!stroke.empty() && std::fabs(stroke.back().x() - p.x()) < 1e-4f &&
                    std::fabs(stroke.back().y() - p.y()) < 1e-4f)
                    return;
                stroke.push_back(p);
            }

            float m_width;
            std::vector<std::vector<math::vec2f>> m_strokes;
        };

        glyph_builder g(float width)
        {
            return glyph_builder(width);
        }

        // ---- the glyphs ------------------------------------------------------------

        stroke_glyph unknown_glyph()
        {
            return g(6).move(0.5f, 0).line(5.5f, 0).line(5.5f, 9).line(0.5f, 9).line(0.5f, 0).finish();
        }

        stroke_glyph make_glyph(char32_t c)
        {
            switch (c)
            {
            // ---- punctuation and symbols ------------------------------------------
            case U' ':
                return g(3.5f).finish();
            case U'!':
                return g(0).move(0, 10).line(0, 3).dot(0, 0).finish();
            case U'"':
                return g(2).move(0, 10).line(0, 7.5f).move(2, 10).line(2, 7.5f).finish();
            case U'#':
                return g(7)
                    .move(3, 9.5f)
                    .line(2, 0.5f)
                    .move(5.5f, 9.5f)
                    .line(4.5f, 0.5f)
                    .move(0.5f, 6.5f)
                    .line(7, 6.5f)
                    .move(0, 3.5f)
                    .line(6.5f, 3.5f)
                    .finish();
            case U'$':
                return g(6)
                    .begin()
                    .arc(3, 7.5f, 2.8f, 2.5f, 15, 270)
                    .arc(3, 2.5f, 3, 2.5f, 90, -165)
                    .move(3, 11)
                    .line(3, -1)
                    .finish();
            case U'%':
                return g(8)
                    .move(0.5f, 0)
                    .line(7.5f, 10)
                    .ellipse(1.5f, 8, 1.5f, 2)
                    .ellipse(6.5f, 2, 1.5f, 2)
                    .finish();
            case U'&':
                return g(7)
                    .move(7, 0)
                    .line(1.94f, 6.74f)
                    .arc(3, 8.2f, 1.8f, 1.8f, 234, -54)
                    .line(0.35f, 3.3f)
                    .arc(2.6f, 2.2f, 2.6f, 2.2f, 150, 330)
                    .line(6.6f, 3.6f)
                    .finish();
            case U'\'':
                return g(0).move(0, 10).line(0, 7.5f).finish();
            case U'(':
                return g(2.5f).begin().arc(3.5f, 4.5f, 3.5f, 6.5f, 106.6f, 253.4f).finish();
            case U')':
                return g(2.5f).begin().arc(-1, 4.5f, 3.5f, 6.5f, 73.4f, -73.4f).finish();
            case U'*':
                return g(5.2f)
                    .move(2.6f, 9.5f)
                    .line(2.6f, 4.5f)
                    .move(0.4f, 8.25f)
                    .line(4.8f, 5.75f)
                    .move(0.4f, 5.75f)
                    .line(4.8f, 8.25f)
                    .finish();
            case U'+':
                return g(6).move(3, 8).line(3, 2).move(0, 5).line(6, 5).finish();
            case U',':
                return g(1).move(1, 0.6f).line(1, 0).line(0, -2).finish();
            case U'-':
                return g(4).move(0, 5).line(4, 5).finish();
            case U'.':
                return g(0).dot(0, 0).finish();
            case U'/':
                return g(5).move(0, -1).line(5, 11).finish();
            case U':':
                return g(0).dot(0, 6).dot(0, 0).finish();
            case U';':
                return g(1).dot(1, 6).move(1, 0.6f).line(1, 0).line(0, -2).finish();
            case U'<':
                return g(6).move(6, 8.5f).line(0, 5).line(6, 1.5f).finish();
            case U'=':
                return g(6).move(0, 6.5f).line(6, 6.5f).move(0, 3.5f).line(6, 3.5f).finish();
            case U'>':
                return g(6).move(0, 8.5f).line(6, 5).line(0, 1.5f).finish();
            case U'?':
                return g(6).begin().arc(3, 7.5f, 3, 2.5f, 160, -90).line(3, 3).dot(3, 0).finish();
            case U'@':
                return g(9)
                    .ellipse(4.2f, 4.9f, 1.6f, 1.9f)
                    .move(5.8f, 6.8f)
                    .line(5.8f, 4.4f)
                    .arc(7.4f, 4.4f, 1.6f, 1.4f, 180, 360)
                    .arc(4.5f, 4.4f, 4.5f, 5.4f, 0, 300)
                    .finish();
            case U'[':
                return g(2.5f).move(2.5f, 11).line(0, 11).line(0, -2).line(2.5f, -2).finish();
            case U'\\':
                return g(5).move(0, 11).line(5, -1).finish();
            case U']':
                return g(2.5f).move(0, 11).line(2.5f, 11).line(2.5f, -2).line(0, -2).finish();
            case U'^':
                return g(6).move(0, 7).line(3, 10).line(6, 7).finish();
            case U'_':
                return g(6).move(0, -1.5f).line(6, -1.5f).finish();
            case U'`':
                return g(1.5f).move(0, 10).line(1.5f, 8.5f).finish();
            case U'{':
                return g(3)
                    .move(3, 11)
                    .line(1.8f, 10.5f)
                    .line(1.5f, 9.5f)
                    .line(1.5f, 5.5f)
                    .line(0, 4.5f)
                    .line(1.5f, 3.5f)
                    .line(1.5f, -0.5f)
                    .line(1.8f, -1.5f)
                    .line(3, -2)
                    .finish();
            case U'|':
                return g(0).move(0, 11).line(0, -2).finish();
            case U'}':
                return g(3)
                    .move(0, 11)
                    .line(1.2f, 10.5f)
                    .line(1.5f, 9.5f)
                    .line(1.5f, 5.5f)
                    .line(3, 4.5f)
                    .line(1.5f, 3.5f)
                    .line(1.5f, -0.5f)
                    .line(1.2f, -1.5f)
                    .line(0, -2)
                    .finish();
            case U'~':
                return g(6).begin().arc(1.5f, 4.5f, 1.5f, 1, 180, 0).arc(4.5f, 4.5f, 1.5f, 1, 180, 360).finish();

            // ---- digits, all of one width -------------------------------------------
            case U'0':
                return g(6).ellipse(3, 5, 3, 5).finish();
            case U'1':
                return g(6).move(1, 8).line(3, 10).line(3, 0).finish();
            case U'2':
                return g(6).begin().arc(3, 7, 3, 3, 160, -35).line(0, 0).line(6, 0).finish();
            case U'3':
                return g(6)
                    .begin()
                    .arc(3, 7.6f, 2.6f, 2.4f, 150, -90)
                    .move(2, 5.2f)
                    .line(3, 5.2f)
                    .arc(3, 2.6f, 3, 2.6f, 90, -150)
                    .finish();
            case U'4':
                return g(6).move(4.5f, 0).line(4.5f, 10).line(0, 3).line(6, 3).finish();
            case U'5':
                return g(6)
                    .move(5.5f, 10)
                    .line(1, 10)
                    .line(0.7f, 5.42f)
                    .arc(3, 3.3f, 3, 3.3f, 140, -150)
                    .finish();
            case U'6':
                return g(6).begin().arc(3, 5, 3, 5, 70, 180).line(0, 3).arc(3, 3, 3, 3, 180, 540).finish();
            case U'7':
                return g(6).move(0, 10).line(6, 10).line(2, 0).finish();
            case U'8':
                return g(6).ellipse(3, 7.6f, 2.6f, 2.4f).ellipse(3, 2.6f, 3, 2.6f).finish();
            case U'9':
                return g(6).begin().arc(3, 7, 3, 3, 0, 360).line(6, 5).arc(3, 5, 3, 5, 0, -110).finish();

            // ---- capitals -----------------------------------------------------------
            case U'A':
                return g(7).move(0, 0).line(3.5f, 10).line(7, 0).move(1.2f, 3.5f).line(5.8f, 3.5f).finish();
            case U'B':
                return g(6.5f)
                    .move(0, 0)
                    .line(0, 10)
                    .line(3.5f, 10)
                    .arc(3.5f, 7.5f, 2.5f, 2.5f, 90, -90)
                    .line(4, 5)
                    .arc(4, 2.5f, 2.5f, 2.5f, 90, -90)
                    .line(0, 0)
                    .move(0, 5)
                    .line(3.5f, 5)
                    .finish();
            case U'C':
                return g(7).begin().arc(3.5f, 5, 3.5f, 5, 45, 315).finish();
            case U'D':
                return g(6.5f).move(0, 0).line(0, 10).line(2.5f, 10).arc(2.5f, 5, 4, 5, 90, -90).line(0, 0).finish();
            case U'E':
                return g(5.5f).move(5.5f, 10).line(0, 10).line(0, 0).line(5.5f, 0).move(0, 5).line(4.5f, 5).finish();
            case U'F':
                return g(5.5f).move(5.5f, 10).line(0, 10).line(0, 0).move(0, 5).line(4.5f, 5).finish();
            case U'G':
                return g(7).begin().arc(3.5f, 5, 3.5f, 5, 45, 360).line(4, 5).finish();
            case U'H':
                return g(6).move(0, 0).line(0, 10).move(6, 0).line(6, 10).move(0, 5).line(6, 5).finish();
            case U'I':
                return g(3).move(0, 10).line(3, 10).move(1.5f, 10).line(1.5f, 0).move(0, 0).line(3, 0).finish();
            case U'J':
                return g(5).move(5, 10).line(5, 2.5f).arc(2.5f, 2.5f, 2.5f, 2.5f, 0, -180).finish();
            case U'K':
                return g(6).move(0, 0).line(0, 10).move(6, 10).line(0, 4).move(2, 6).line(6, 0).finish();
            case U'L':
                return g(5.5f).move(0, 10).line(0, 0).line(5.5f, 0).finish();
            case U'M':
                return g(8).move(0, 0).line(0, 10).line(4, 2).line(8, 10).line(8, 0).finish();
            case U'N':
                return g(6).move(0, 0).line(0, 10).line(6, 0).line(6, 10).finish();
            case U'O':
                return g(7).ellipse(3.5f, 5, 3.5f, 5).finish();
            case U'P':
                return g(6).move(0, 0).line(0, 10).line(3.5f, 10).arc(3.5f, 7.5f, 2.5f, 2.5f, 90, -90).line(0, 5).finish();
            case U'Q':
                return g(7).ellipse(3.5f, 5, 3.5f, 5).move(4.2f, 2.5f).line(7, 0).finish();
            case U'R':
                return g(6)
                    .move(0, 0)
                    .line(0, 10)
                    .line(3.5f, 10)
                    .arc(3.5f, 7.5f, 2.5f, 2.5f, 90, -90)
                    .line(0, 5)
                    .move(3.2f, 5)
                    .line(6, 0)
                    .finish();
            case U'S':
                return g(6).begin().arc(3, 7.5f, 2.8f, 2.5f, 15, 270).arc(3, 2.5f, 3, 2.5f, 90, -165).finish();
            case U'T':
                return g(6).move(0, 10).line(6, 10).move(3, 10).line(3, 0).finish();
            case U'U':
                return g(6).move(0, 10).line(0, 3).arc(3, 3, 3, 3, 180, 360).line(6, 10).finish();
            case U'V':
                return g(7).move(0, 10).line(3.5f, 0).line(7, 10).finish();
            case U'W':
                return g(9).move(0, 10).line(2, 0).line(4.5f, 8).line(7, 0).line(9, 10).finish();
            case U'X':
                return g(6).move(0, 0).line(6, 10).move(0, 10).line(6, 0).finish();
            case U'Y':
                return g(7).move(0, 10).line(3.5f, 5).line(7, 10).move(3.5f, 5).line(3.5f, 0).finish();
            case U'Z':
                return g(6).move(0, 10).line(6, 10).line(0, 0).line(6, 0).finish();

            // ---- lowercase ----------------------------------------------------------
            case U'a':
                return g(5)
                    .begin()
                    .arc(2.6f, 5, 2.3f, 2, 150, 0)
                    .line(4.9f, 0)
                    .move(4.9f, 3.8f)
                    .line(2.45f, 3.8f)
                    .arc(2.45f, 1.9f, 2.45f, 1.9f, 90, 360)
                    .finish();
            case U'b':
                return g(5.5f).move(0, 10).line(0, 0).ellipse(2.75f, 3.5f, 2.75f, 3.5f).finish();
            case U'c':
                return g(5.5f).begin().arc(2.75f, 3.5f, 2.75f, 3.5f, 45, 315).finish();
            case U'd':
                return g(5.5f).move(5.5f, 10).line(5.5f, 0).ellipse(2.75f, 3.5f, 2.75f, 3.5f).finish();
            case U'e':
                return g(5.5f).move(0, 3.5f).line(5.5f, 3.5f).arc(2.75f, 3.5f, 2.75f, 3.5f, 0, 315).finish();
            case U'f':
                return g(4.9f)
                    .move(1.8f, 0)
                    .line(1.8f, 8.2f)
                    .arc(3.6f, 8.2f, 1.8f, 1.8f, 180, 45)
                    .move(0, 7)
                    .line(4, 7)
                    .finish();
            case U'g':
                return g(5.5f)
                    .ellipse(2.75f, 3.5f, 2.75f, 3.5f)
                    .move(5.5f, 7)
                    .line(5.5f, -0.5f)
                    .arc(2.75f, -0.5f, 2.75f, 2.5f, 0, -160)
                    .finish();
            case U'h':
                return g(5.5f).move(0, 10).line(0, 0).move(0, 4).arc(2.75f, 4, 2.75f, 3, 180, 0).line(5.5f, 0).finish();
            case U'i':
                return g(0).move(0, 0).line(0, 7).dot(0, 9.5f).finish();
            case U'j':
                return g(2.5f)
                    .move(2.5f, 7)
                    .line(2.5f, -1.5f)
                    .arc(1.25f, -1.5f, 1.25f, 1.5f, 0, -180)
                    .dot(2.5f, 9.5f)
                    .finish();
            case U'k':
                return g(5).move(0, 10).line(0, 0).move(5, 7).line(0, 2.5f).move(1.8f, 4.12f).line(5, 0).finish();
            case U'l':
                return g(0).move(0, 10).line(0, 0).finish();
            case U'm':
                return g(8)
                    .move(0, 7)
                    .line(0, 0)
                    .move(0, 4.5f)
                    .arc(2, 4.5f, 2, 2.5f, 180, 0)
                    .line(4, 0)
                    .move(4, 4.5f)
                    .arc(6, 4.5f, 2, 2.5f, 180, 0)
                    .line(8, 0)
                    .finish();
            case U'n':
                return g(5.5f).move(0, 7).line(0, 0).move(0, 4).arc(2.75f, 4, 2.75f, 3, 180, 0).line(5.5f, 0).finish();
            case U'o':
                return g(5.5f).ellipse(2.75f, 3.5f, 2.75f, 3.5f).finish();
            case U'p':
                return g(5.5f).move(0, 7).line(0, -3).ellipse(2.75f, 3.5f, 2.75f, 3.5f).finish();
            case U'q':
                return g(5.5f).move(5.5f, 7).line(5.5f, -3).ellipse(2.75f, 3.5f, 2.75f, 3.5f).finish();
            case U'r':
                return g(4.1f).move(0, 7).line(0, 0).move(0, 3.8f).arc(3, 3.8f, 3, 3.2f, 180, 70).finish();
            case U's':
                return g(5).begin().arc(2.5f, 5.25f, 2.4f, 1.75f, 10, 270).arc(2.5f, 1.75f, 2.5f, 1.75f, 90, -170).finish();
            case U't':
                return g(4.1f)
                    .move(1.8f, 9.5f)
                    .line(1.8f, 1.5f)
                    .arc(3.3f, 1.5f, 1.5f, 1.5f, 180, 300)
                    .move(0, 7)
                    .line(4, 7)
                    .finish();
            case U'u':
                return g(5.5f).move(0, 7).line(0, 3).arc(2.75f, 3, 2.75f, 3, 180, 360).move(5.5f, 7).line(5.5f, 0).finish();
            case U'v':
                return g(5.5f).move(0, 7).line(2.75f, 0).line(5.5f, 7).finish();
            case U'w':
                return g(8).move(0, 7).line(1.8f, 0).line(4, 5.5f).line(6.2f, 0).line(8, 7).finish();
            case U'x':
                return g(5).move(0, 0).line(5, 7).move(0, 7).line(5, 0).finish();
            case U'y':
                return g(5.5f).move(0, 7).line(2.91f, 0).move(5.5f, 7).line(1.8f, -3).finish();
            case U'z':
                return g(5).move(0, 7).line(5, 7).line(0, 0).line(5, 0).finish();

            // ---- beyond ASCII ---------------------------------------------------------
            case U'Ω': // Ω
                return g(7)
                    .move(0, 0)
                    .line(2.3f, 0)
                    .line(1.49f, 2.72f)
                    .arc(3.5f, 6, 3.5f, 4, 235, -55)
                    .line(4.7f, 0)
                    .line(7, 0)
                    .finish();
            case U'µ': // µ, micro
            case U'μ': // μ, mu
                return g(5.5f)
                    .move(0, -3)
                    .line(0, 7)
                    .move(0, 3)
                    .arc(2.75f, 3, 2.75f, 3, 180, 360)
                    .move(5.5f, 7)
                    .line(5.5f, 0)
                    .finish();
            case U'°': // °
                return g(3).ellipse(1.5f, 8.5f, 1.5f, 1.5f).finish();
            case U'±': // ±
                return g(6).move(3, 9).line(3, 4).move(0, 6.5f).line(6, 6.5f).move(0, 1.5f).line(6, 1.5f).finish();
            default:
                return unknown_glyph();
            }
        }

        constexpr std::array<char32_t, 5> extended = {U'Ω', U'µ', U'μ', U'°', U'±'};

        struct font
        {
            std::array<stroke_glyph, 95> ascii;
            std::array<stroke_glyph, extended.size()> more;
            stroke_glyph unknown;

            font()
            {
                for (std::size_t i = 0; i < ascii.size(); ++i)
                    ascii[i] = make_glyph(static_cast<char32_t>(32 + i));
                for (std::size_t i = 0; i < extended.size(); ++i)
                    more[i] = make_glyph(extended[i]);
                unknown = unknown_glyph();
            }
        };

        const font &the_font()
        {
            static const font instance;
            return instance;
        }

        // Decodes one code point at `i`, moving past it.
        char32_t next_code_point(std::string_view s, std::size_t &i) noexcept
        {
            constexpr char32_t replacement = U'�';
            const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
            const unsigned char lead = byte(i);
            if (lead < 0x80)
            {
                ++i;
                return lead;
            }
            std::size_t length = 0;
            char32_t value = 0;
            char32_t least = 0;
            if ((lead & 0xE0) == 0xC0)
            {
                length = 2;
                value = lead & 0x1Fu;
                least = 0x80;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                length = 3;
                value = lead & 0x0Fu;
                least = 0x800;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                length = 4;
                value = lead & 0x07u;
                least = 0x10000;
            }
            else
            {
                ++i;
                return replacement;
            }
            if (i + length > s.size())
            {
                ++i;
                return replacement;
            }
            for (std::size_t k = 1; k < length; ++k)
            {
                const unsigned char next = byte(i + k);
                if ((next & 0xC0) != 0x80)
                {
                    ++i;
                    return replacement;
                }
                value = (value << 6) | (next & 0x3Fu);
            }
            if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            {
                ++i;
                return replacement;
            }
            i += length;
            return value;
        }

        int normal_turns(int quarter_turns) noexcept
        {
            return ((quarter_turns % 4) + 4) % 4;
        }
    } // namespace

    const stroke_glyph &find_glyph(char32_t code_point)
    {
        const font &f = the_font();
        if (code_point >= 32 && code_point <= 126)
            return f.ascii[code_point - 32];
        for (std::size_t i = 0; i < extended.size(); ++i)
            if (extended[i] == code_point)
                return f.more[i];
        return f.unknown;
    }

    bool has_glyph(char32_t code_point) noexcept
    {
        if (code_point >= 32 && code_point <= 126)
            return true;
        for (const char32_t c : extended)
            if (c == code_point)
                return true;
        return false;
    }

    std::u32string decode_utf8(std::string_view utf8)
    {
        std::u32string result;
        result.reserve(utf8.size());
        std::size_t i = 0;
        while (i < utf8.size())
            result.push_back(next_code_point(utf8, i));
        return result;
    }

    float text_width(std::string_view utf8, float height)
    {
        float width = 0.0f;
        std::size_t i = 0;
        while (i < utf8.size())
            width += find_glyph(next_code_point(utf8, i)).advance;
        return width * height;
    }

    text_frame layout_text(float width, math::vec2f anchor, float height, horizontal_align horizontal,
                           vertical_align vertical, int quarter_turns) noexcept
    {
        const int turns = normal_turns(quarter_turns);
        // Turned twice is unturned, read from the other side: the anchor
        // stays where the turned box would have it.
        if (turns >= 2)
        {
            if (horizontal == horizontal_align::left)
                horizontal = horizontal_align::right;
            else if (horizontal == horizontal_align::right)
                horizontal = horizontal_align::left;
            if (vertical == vertical_align::bottom)
                vertical = vertical_align::top;
            else if (vertical == vertical_align::top)
                vertical = vertical_align::bottom;
        }

        text_frame frame;
        if ((turns & 1) != 0)
        {
            frame.along = math::vec2f{0.0f, 1.0f};
            frame.up = math::vec2f{-1.0f, 0.0f};
        }
        const float dx = horizontal == horizontal_align::left     ? 0.0f
                         : horizontal == horizontal_align::center ? -0.5f * width
                                                                  : -width;
        const float dy = vertical == vertical_align::bottom   ? 0.0f
                         : vertical == vertical_align::middle ? -0.5f * height
                                                              : -height;
        frame.origin = anchor + frame.along * dx + frame.up * dy;
        return frame;
    }

    void text_bounds(std::string_view utf8, math::vec2f anchor, float height, horizontal_align horizontal,
                     vertical_align vertical, int quarter_turns, math::vec2f &min, math::vec2f &max)
    {
        const float width = text_width(utf8, height);
        const text_frame frame = layout_text(width, anchor, height, horizontal, vertical, quarter_turns);
        const math::vec2f low = frame.up * (-font_descent * height);
        const math::vec2f high = frame.up * height;
        const math::vec2f far = frame.along * width;
        const math::vec2f corners[] = {frame.origin + low, frame.origin + high, frame.origin + far + low,
                                       frame.origin + far + high};
        min = max = corners[0];
        for (const math::vec2f &c : corners)
        {
            min = math::vec2f{std::min(min.x(), c.x()), std::min(min.y(), c.y())};
            max = math::vec2f{std::max(max.x(), c.x()), std::max(max.y(), c.y())};
        }
    }

    void append_text(std::vector<segment> &out, std::string_view utf8, math::vec2f anchor, float height,
                     horizontal_align horizontal, vertical_align vertical, int quarter_turns, float stroke_width,
                     const math::vec4f &color)
    {
        if (utf8.empty() || !(height > 0.0f))
            return;
        const float width = text_width(utf8, height);
        const text_frame frame = layout_text(width, anchor, height, horizontal, vertical, quarter_turns);
        const math::vec2f along = frame.along * height;
        const math::vec2f up = frame.up * height;

        float cursor = 0.0f; // in heights
        std::size_t i = 0;
        while (i < utf8.size())
        {
            const stroke_glyph &glyph = find_glyph(next_code_point(utf8, i));
            const auto place = [&](math::vec2f p) { return frame.origin + along * (cursor + p.x()) + up * p.y(); };
            for (const std::vector<math::vec2f> &stroke : glyph.strokes)
            {
                if (stroke.size() == 1)
                {
                    const math::vec2f at = place(stroke[0]);
                    out.push_back({at, at, stroke_width, color, -1});
                    continue;
                }
                math::vec2f from = place(stroke[0]);
                for (std::size_t k = 1; k < stroke.size(); ++k)
                {
                    const math::vec2f to = place(stroke[k]);
                    out.push_back({from, to, stroke_width, color, -1});
                    from = to;
                }
            }
            cursor += glyph.advance;
        }
    }

} // namespace volt::schematic
