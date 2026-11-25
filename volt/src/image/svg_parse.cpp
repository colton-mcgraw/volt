#include <image/svg.hpp>

#include <image/compress.hpp>
#include <image/xml.hpp>

#include "svg_style.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace volt::image::svg
{

    namespace
    {
        using namespace detail;
        using failure = std::unexpected<std::string>;

        // More than any drawing needs; guards against <use> trees that
        // multiply into millions of shapes.
        constexpr std::size_t max_items = 200000;
        constexpr int max_use_depth = 32;

        // ---- paint and the computed style -------------------------------------

        struct paint_value
        {
            enum class kind : std::uint8_t
            {
                none,
                color,
                current_color,
                url,
            };

            kind type = kind::none;
            math::vec4f color{0, 0, 0, 1};
            std::string url; // the id, without '#'

            // For url(): what to use if the reference is no good.
            kind fallback = kind::none;
            math::vec4f fallback_color{0, 0, 0, 1};
        };

        std::optional<paint_value> parse_paint(std::string_view text)
        {
            text = trim(text);
            paint_value result;
            if (text == "none")
                return result;
            if (text == "currentColor" || text == "currentcolor")
            {
                result.type = paint_value::kind::current_color;
                return result;
            }
            if (text.starts_with("url("))
            {
                const std::size_t close = text.find(')');
                if (close == std::string_view::npos)
                    return std::nullopt;
                std::string_view reference = trim(text.substr(4, close - 4));
                if (reference.size() >= 2 && (reference.front() == '"' || reference.front() == '\''))
                    reference = reference.substr(1, reference.size() - 2);
                const std::size_t hash = reference.find('#');
                result.type = paint_value::kind::url;
                result.url = std::string(hash == std::string_view::npos ? reference : reference.substr(hash + 1));
                const std::string_view rest = trim(text.substr(close + 1));
                if (rest == "currentColor" || rest == "currentcolor")
                    result.fallback = paint_value::kind::current_color;
                else if (const auto color = parse_color(rest))
                {
                    result.fallback = paint_value::kind::color;
                    result.fallback_color = *color;
                }
                return result;
            }
            if (const auto color = parse_color(text))
            {
                result.type = paint_value::kind::color;
                result.color = *color;
                return result;
            }
            return std::nullopt;
        }

        // The inherited properties, as computed for an element.
        struct style
        {
            paint_value fill = [] {
                paint_value black;
                black.type = paint_value::kind::color;
                return black;
            }();
            paint_value stroke;
            float fill_opacity = 1.0f;
            float stroke_opacity = 1.0f;
            fill_rule rule = fill_rule::nonzero;
            length stroke_width{1.0f, length::unit::none};
            line_cap cap = line_cap::butt;
            line_join join = line_join::miter;
            float miter_limit = 4.0f;
            std::vector<length> dashes;
            length dash_offset{};
            math::vec4f color{0, 0, 0, 1};
            bool visible = true;
        };

        float parse_opacity(std::string_view text, float fallback)
        {
            const auto l = parse_length(text);
            if (!l)
                return fallback;
            const float value = l->type == length::unit::percent ? l->value / 100.0f : l->value;
            return std::clamp(value, 0.0f, 1.0f);
        }

        // The values that are not inherited, for one element.
        struct own_properties
        {
            float opacity = 1.0f;
            bool display = true;
            std::optional<std::string> stop_color;
            float stop_opacity = 1.0f;
        };

        // Applies one declaration; unknown properties and invalid values are
        // ignored, as CSS does.
        void apply(style &s, own_properties &own, std::string_view name, std::string_view value)
        {
            value = trim(value);
            if (value == "inherit")
                return; // already inherited
            if (name == "fill")
            {
                if (const auto p = parse_paint(value))
                    s.fill = *p;
            }
            else if (name == "stroke")
            {
                if (const auto p = parse_paint(value))
                    s.stroke = *p;
            }
            else if (name == "fill-opacity")
                s.fill_opacity = parse_opacity(value, s.fill_opacity);
            else if (name == "stroke-opacity")
                s.stroke_opacity = parse_opacity(value, s.stroke_opacity);
            else if (name == "opacity")
                own.opacity = parse_opacity(value, own.opacity);
            else if (name == "fill-rule")
            {
                if (value == "evenodd")
                    s.rule = fill_rule::even_odd;
                else if (value == "nonzero")
                    s.rule = fill_rule::nonzero;
            }
            else if (name == "stroke-width")
            {
                if (const auto l = parse_length(value); l && l->value >= 0.0f)
                    s.stroke_width = *l;
            }
            else if (name == "stroke-linecap")
            {
                if (value == "butt")
                    s.cap = line_cap::butt;
                else if (value == "round")
                    s.cap = line_cap::round;
                else if (value == "square")
                    s.cap = line_cap::square;
            }
            else if (name == "stroke-linejoin")
            {
                if (value == "miter" || value == "miter-clip" || value == "arcs")
                    s.join = line_join::miter;
                else if (value == "round")
                    s.join = line_join::round;
                else if (value == "bevel")
                    s.join = line_join::bevel;
            }
            else if (name == "stroke-miterlimit")
            {
                if (const auto l = parse_length(value); l && l->type == length::unit::none && l->value >= 1.0f)
                    s.miter_limit = l->value;
            }
            else if (name == "stroke-dasharray")
            {
                if (value == "none")
                    s.dashes.clear();
                else
                {
                    // Lengths separated by commas and/or spaces.
                    std::vector<length> dashes;
                    std::string token;
                    bool valid = true;
                    const auto flush = [&] {
                        if (token.empty())
                            return;
                        const auto l = parse_length(token);
                        if (!l || l->value < 0.0f)
                            valid = false;
                        else
                            dashes.push_back(*l);
                        token.clear();
                    };
                    for (const char c : value)
                    {
                        if (c == ',' || is_space(c))
                            flush();
                        else
                            token += c;
                    }
                    flush();
                    if (valid)
                        s.dashes = std::move(dashes);
                }
            }
            else if (name == "stroke-dashoffset")
            {
                if (const auto l = parse_length(value))
                    s.dash_offset = *l;
            }
            else if (name == "color")
            {
                if (const auto c = parse_color(value))
                    s.color = *c;
            }
            else if (name == "visibility")
                s.visible = value == "visible";
            else if (name == "display")
                own.display = value != "none";
            else if (name == "stop-color")
                own.stop_color = std::string(value);
            else if (name == "stop-opacity")
                own.stop_opacity = parse_opacity(value, own.stop_opacity);
        }

        bool is_property(std::string_view name)
        {
            static constexpr std::string_view names[] = {
                "fill",          "stroke",           "fill-opacity",     "stroke-opacity",    "opacity",
                "fill-rule",     "stroke-width",     "stroke-linecap",   "stroke-linejoin",   "stroke-miterlimit",
                "stroke-dasharray", "stroke-dashoffset", "color",        "visibility",        "display",
                "stop-color",    "stop-opacity",
            };
            return std::find(std::begin(names), std::end(names), name) != std::end(names);
        }

        // ---- viewports ---------------------------------------------------------------

        struct viewport
        {
            float width = 0.0f;
            float height = 0.0f;

            float diagonal() const { return std::sqrt((width * width + height * height) / 2.0f); }
        };

        enum class axis
        {
            x,
            y,
            other,
        };

        float resolve(const length &l, const viewport &vp, axis a)
        {
            const float reference = a == axis::x ? vp.width : a == axis::y ? vp.height : vp.diagonal();
            return l.resolve(reference);
        }

        // viewBox onto a viewport of the given size, as preserveAspectRatio
        // says.
        math::mat3f view_box_transform(const std::vector<float> &box, float width, float height,
                                       std::string_view aspect)
        {
            if (box.size() != 4 || !(box[2] > 0.0f) || !(box[3] > 0.0f))
                return math::mat3f::identity();
            const float sx = width / box[2];
            const float sy = height / box[3];

            aspect = trim(aspect);
            if (aspect.starts_with("defer"))
                aspect = trim(aspect.substr(5));
            if (aspect.starts_with("none"))
                return scaling(sx, sy) * translation(-box[0], -box[1]);

            std::string_view align = aspect.substr(0, aspect.find(' '));
            if (align.empty())
                align = "xMidYMid";
            const bool slice = aspect.find("slice") != std::string_view::npos;
            const float s = slice ? std::max(sx, sy) : std::min(sx, sy);
            float tx = -box[0] * s;
            float ty = -box[1] * s;
            const float extra_x = width - box[2] * s;
            const float extra_y = height - box[3] * s;
            if (align.find("xMid") != std::string_view::npos)
                tx += extra_x / 2.0f;
            else if (align.find("xMax") != std::string_view::npos)
                tx += extra_x;
            if (align.find("YMid") != std::string_view::npos)
                ty += extra_y / 2.0f;
            else if (align.find("YMax") != std::string_view::npos)
                ty += extra_y;
            return translation(tx, ty) * scaling(s, s);
        }

        // ---- path data ------------------------------------------------------------------

        // The path grammar of SVG 2 (8.3). On an error, the path keeps what
        // came before it.
        void parse_path_data(std::string_view text, path &out)
        {
            number_reader in(text);
            char command = 0;
            math::vec2f current{};
            math::vec2f start{};
            math::vec2f last_control{};
            char last_command = 0;

            while (!in.at_end())
            {
                const char c = in.peek();
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                {
                    command = c;
                    in.advance();
                }
                else if (command == 0 || command == 'z' || command == 'Z')
                    return; // numbers with no command to take them
                // After a moveto, further pairs are linetos.
                else if (command == 'M')
                    command = 'L';
                else if (command == 'm')
                    command = 'l';

                const bool relative = command >= 'a' && command <= 'z';
                const math::vec2f base = relative ? current : math::vec2f{0, 0};
                float v[7];
                const auto read = [&](int count) {
                    for (int i = 0; i < count; ++i)
                        if (!in.number(v[i]))
                            return false;
                    return true;
                };

                switch (command)
                {
                case 'M':
                case 'm':
                    if (!read(2))
                        return;
                    current = base + math::vec2f{v[0], v[1]};
                    start = current;
                    out.move_to(current);
                    break;
                case 'L':
                case 'l':
                    if (!read(2))
                        return;
                    current = base + math::vec2f{v[0], v[1]};
                    out.line_to(current);
                    break;
                case 'H':
                case 'h':
                    if (!read(1))
                        return;
                    current = {relative ? current[0] + v[0] : v[0], current[1]};
                    out.line_to(current);
                    break;
                case 'V':
                case 'v':
                    if (!read(1))
                        return;
                    current = {current[0], relative ? current[1] + v[0] : v[0]};
                    out.line_to(current);
                    break;
                case 'C':
                case 'c':
                {
                    if (!read(6))
                        return;
                    const math::vec2f c1 = base + math::vec2f{v[0], v[1]};
                    const math::vec2f c2 = base + math::vec2f{v[2], v[3]};
                    current = base + math::vec2f{v[4], v[5]};
                    out.cubic_to(c1, c2, current);
                    last_control = c2;
                    break;
                }
                case 'S':
                case 's':
                {
                    if (!read(4))
                        return;
                    const bool follows = last_command == 'C' || last_command == 'c' || last_command == 'S' ||
                                         last_command == 's';
                    const math::vec2f c1 = follows ? 2.0f * current - last_control : current;
                    const math::vec2f c2 = base + math::vec2f{v[0], v[1]};
                    current = base + math::vec2f{v[2], v[3]};
                    out.cubic_to(c1, c2, current);
                    last_control = c2;
                    break;
                }
                case 'Q':
                case 'q':
                {
                    if (!read(4))
                        return;
                    const math::vec2f control = base + math::vec2f{v[0], v[1]};
                    current = base + math::vec2f{v[2], v[3]};
                    out.quad_to(control, current);
                    last_control = control;
                    break;
                }
                case 'T':
                case 't':
                {
                    if (!read(2))
                        return;
                    const bool follows = last_command == 'Q' || last_command == 'q' || last_command == 'T' ||
                                         last_command == 't';
                    const math::vec2f control = follows ? 2.0f * current - last_control : current;
                    current = base + math::vec2f{v[0], v[1]};
                    out.quad_to(control, current);
                    last_control = control;
                    break;
                }
                case 'A':
                case 'a':
                {
                    bool large = false;
                    bool sweep = false;
                    if (!in.number(v[0]) || !in.number(v[1]) || !in.number(v[2]) || !in.flag(large) ||
                        !in.flag(sweep) || !in.number(v[3]) || !in.number(v[4]))
                        return;
                    const math::vec2f end = base + math::vec2f{v[3], v[4]};
                    // arc_to starts from the path's current point.
                    if (out.empty())
                        out.move_to(current);
                    out.arc_to({v[0], v[1]}, v[2], large, sweep, end);
                    current = end;
                    break;
                }
                case 'Z':
                case 'z':
                    out.close();
                    current = start;
                    break;
                default:
                    return; // not a path command
                }
                last_command = command;
            }
        }

        // ---- the document -----------------------------------------------------------------

        class builder
        {
        public:
            builder(const xml::element &root, const parse_options &options) : m_root(root), m_options(options)
            {
                index(root);
            }

            std::expected<document, std::string> run()
            {
                if (m_root.local_name() != "svg")
                    return failure(std::format("the root element is <{}>, not <svg>", m_root.name));

                // The intrinsic size: width and height, or the viewBox's.
                const std::vector<float> box = parse_numbers(m_root.find("viewBox").value_or(""));
                const bool has_box = box.size() == 4 && box[2] > 0.0f && box[3] > 0.0f;
                const auto size = [&](const char *name, float from_box, float fallback) {
                    const auto l = parse_length(m_root.find(name).value_or(""));
                    if (l && l->type != length::unit::percent && l->value > 0.0f)
                        return l->resolve(0.0f);
                    return has_box ? from_box : fallback;
                };
                float width = size("width", has_box ? box[2] : 0.0f, m_options.default_width);
                float height = size("height", has_box ? box[3] : 0.0f, m_options.default_height);
                // One given, the other from the viewBox's aspect ratio.
                if (has_box)
                {
                    const bool has_width = parse_length(m_root.find("width").value_or("")).has_value() &&
                                           parse_length(m_root.find("width").value_or(""))->type != length::unit::percent;
                    const bool has_height = parse_length(m_root.find("height").value_or("")).has_value() &&
                                            parse_length(m_root.find("height").value_or(""))->type != length::unit::percent;
                    if (has_width && !has_height)
                        height = width * box[3] / box[2];
                    else if (has_height && !has_width)
                        width = height * box[2] / box[3];
                }
                m_document.width = width;
                m_document.height = height;

                const style initial;
                walk_svg(m_root, initial, math::mat3f::identity(), {width, height}, true);
                return std::move(m_document);
            }

        private:
            // Every element with an id, and every style sheet.
            void index(const xml::element &e)
            {
                if (const auto id = e.find("id"); id && !m_ids.contains(std::string(*id)))
                    m_ids.emplace(std::string(*id), &e);
                if (e.local_name() == "style")
                    m_sheet.add(e.text);
                for (const xml::element &child : e.children)
                    index(child);
            }

            const xml::element *referenced(const xml::element &e) const
            {
                std::optional<std::string_view> href = e.find("href");
                if (!href)
                    href = e.find("xlink:href");
                if (!href)
                    return nullptr;
                std::string_view id = trim(*href);
                if (!id.starts_with('#'))
                    return nullptr;
                const auto found = m_ids.find(std::string(id.substr(1)));
                return found == m_ids.end() ? nullptr : found->second;
            }

            // The element's style: inherited, then its presentation
            // attributes, matching style sheet rules, and style attribute.
            void compute(const xml::element &e, style &s, own_properties &own) const
            {
                for (const xml::attribute &a : e.attributes)
                    if (is_property(a.name))
                        apply(s, own, a.name, a.value);
                if (!m_sheet.empty())
                    for (const auto &[name, value] : m_sheet.match(e))
                        apply(s, own, name, value);
                if (const auto inline_style = e.find("style"))
                    for (const auto &[name, value] : parse_declarations(*inline_style))
                        apply(s, own, name, value);
            }

            float attribute_length(const xml::element &e, const char *name, const viewport &vp, axis a,
                                   float fallback = 0.0f) const
            {
                const auto l = parse_length(e.find(name).value_or(""));
                return l ? resolve(*l, vp, a) : fallback;
            }

            bool full() const { return m_document.items.size() >= max_items; }

            void begin_group(float opacity)
            {
                item i;
                i.type = item::kind::begin_group;
                i.opacity = opacity;
                m_document.items.push_back(std::move(i));
            }

            void end_group()
            {
                item i;
                i.type = item::kind::end_group;
                m_document.items.push_back(std::move(i));
            }

            // <svg>: a new viewport. The root's was set up by run().
            void walk_svg(const xml::element &e, const style &inherited, const math::mat3f &transform,
                          const viewport &outer, bool root)
            {
                style s = inherited;
                own_properties own;
                compute(e, s, own);
                if (!own.display)
                    return;

                math::mat3f t = transform;
                viewport vp = outer;
                if (!root)
                {
                    const float x = attribute_length(e, "x", outer, axis::x);
                    const float y = attribute_length(e, "y", outer, axis::y);
                    vp.width = attribute_length(e, "width", outer, axis::x, outer.width);
                    vp.height = attribute_length(e, "height", outer, axis::y, outer.height);
                    if (!(vp.width > 0.0f) || !(vp.height > 0.0f))
                        return;
                    t = t * translation(x, y);
                }
                const std::vector<float> box = parse_numbers(e.find("viewBox").value_or(""));
                if (box.size() == 4 && box[2] > 0.0f && box[3] > 0.0f)
                {
                    t = t * view_box_transform(box, vp.width, vp.height, e.find("preserveAspectRatio").value_or(""));
                    vp = {box[2], box[3]};
                }
                group(e, s, own, t, vp);
            }

            // The children of a container, as a group if it has opacity.
            void group(const xml::element &e, const style &s, const own_properties &own, const math::mat3f &t,
                       const viewport &vp)
            {
                const bool layered = own.opacity < 1.0f;
                if (layered)
                    begin_group(own.opacity);
                for (const xml::element &child : e.children)
                    walk(child, s, t, vp);
                if (layered)
                    end_group();
            }

            void walk(const xml::element &e, const style &inherited, const math::mat3f &parent, const viewport &vp)
            {
                if (full())
                    return;
                const std::string_view name = e.local_name();

                // Never drawn where they stand.
                static constexpr std::string_view skipped[] = {
                    "defs",  "symbol",  "linearGradient", "radialGradient", "clipPath", "mask",   "pattern",
                    "marker", "style",  "title",          "desc",           "metadata", "script", "filter",
                    "text",  "image",   "foreignObject",  "view",           "font",     "cursor", "animate",
                };
                if (std::find(std::begin(skipped), std::end(skipped), name) != std::end(skipped))
                    return;

                math::mat3f t = parent;
                if (const auto attribute = e.find("transform"))
                    if (const auto m = parse_transform(*attribute))
                        t = t * *m;

                if (name == "svg")
                {
                    walk_svg(e, inherited, t, vp, false);
                    return;
                }

                style s = inherited;
                own_properties own;
                compute(e, s, own);
                if (!own.display)
                    return;

                if (name == "g" || name == "a")
                    group(e, s, own, t, vp);
                else if (name == "switch")
                {
                    // The first child that would be drawn; conditions
                    // (systemLanguage and the like) are taken as met.
                    const bool layered = own.opacity < 1.0f;
                    if (layered)
                        begin_group(own.opacity);
                    for (const xml::element &child : e.children)
                    {
                        const std::string_view n = child.local_name();
                        if (n == "g" || n == "a" || n == "svg" || n == "use" || n == "path" || n == "rect" ||
                            n == "circle" || n == "ellipse" || n == "line" || n == "polyline" || n == "polygon" ||
                            n == "switch")
                        {
                            walk(child, s, t, vp);
                            break;
                        }
                    }
                    if (layered)
                        end_group();
                }
                else if (name == "use")
                    use(e, s, own, t, vp);
                else
                    draw_shape(e, s, own, t, vp);
            }

            void use(const xml::element &e, const style &s, const own_properties &own, const math::mat3f &t,
                     const viewport &vp)
            {
                const xml::element *target = referenced(e);
                if (target == nullptr || m_active.contains(target) || m_use_depth >= max_use_depth)
                    return;
                // A <use> inside what it refers to would recurse.
                m_active.insert(target);
                ++m_use_depth;

                const float x = attribute_length(e, "x", vp, axis::x);
                const float y = attribute_length(e, "y", vp, axis::y);
                const math::mat3f placed = t * translation(x, y);
                const bool layered = own.opacity < 1.0f;
                if (layered)
                    begin_group(own.opacity);

                const std::string_view kind = target->local_name();
                if (kind == "symbol" || kind == "svg")
                {
                    style inner = s;
                    own_properties target_own;
                    compute(*target, inner, target_own);
                    if (target_own.display)
                    {
                        // Sized by the use element, 100% by default.
                        viewport inner_vp{attribute_length(e, "width", vp, axis::x, vp.width),
                                          attribute_length(e, "height", vp, axis::y, vp.height)};
                        if (kind == "svg")
                        {
                            inner_vp.width = attribute_length(e, "width", vp, axis::x,
                                                              attribute_length(*target, "width", vp, axis::x, vp.width));
                            inner_vp.height = attribute_length(
                                e, "height", vp, axis::y, attribute_length(*target, "height", vp, axis::y, vp.height));
                        }
                        math::mat3f inner_t = placed;
                        if (const auto attribute = target->find("transform"))
                            if (const auto m = parse_transform(*attribute))
                                inner_t = inner_t * *m;
                        const std::vector<float> box = parse_numbers(target->find("viewBox").value_or(""));
                        if (box.size() == 4 && box[2] > 0.0f && box[3] > 0.0f)
                        {
                            inner_t = inner_t * view_box_transform(box, inner_vp.width, inner_vp.height,
                                                                   target->find("preserveAspectRatio").value_or(""));
                            inner_vp = {box[2], box[3]};
                        }
                        group(*target, inner, target_own, inner_t, inner_vp);
                    }
                }
                else
                    walk(*target, s, placed, vp);

                if (layered)
                    end_group();
                --m_use_depth;
                m_active.erase(target);
            }

            // ---- shapes ----------------------------------------------------------------

            path geometry(const xml::element &e, const viewport &vp) const
            {
                path p;
                const std::string_view name = e.local_name();
                const auto len = [&](const char *attribute, axis a, float fallback = 0.0f) {
                    return attribute_length(e, attribute, vp, a, fallback);
                };

                if (name == "path")
                    parse_path_data(e.find("d").value_or(""), p);
                else if (name == "rect")
                {
                    const float w = len("width", axis::x);
                    const float h = len("height", axis::y);
                    if (!(w > 0.0f) || !(h > 0.0f))
                        return p;
                    const auto rx_attribute = parse_length(e.find("rx").value_or(""));
                    const auto ry_attribute = parse_length(e.find("ry").value_or(""));
                    float rx = rx_attribute ? resolve(*rx_attribute, vp, axis::x) : -1.0f;
                    float ry = ry_attribute ? resolve(*ry_attribute, vp, axis::y) : -1.0f;
                    // One radius given: the other is the same.
                    if (rx < 0.0f)
                        rx = ry;
                    if (ry < 0.0f)
                        ry = rx;
                    p.add_rounded_rectangle({len("x", axis::x), len("y", axis::y)}, {w, h},
                                            {std::max(rx, 0.0f), std::max(ry, 0.0f)});
                }
                else if (name == "circle")
                {
                    const float r = len("r", axis::other);
                    if (r > 0.0f)
                        p.add_ellipse({len("cx", axis::x), len("cy", axis::y)}, {r, r});
                }
                else if (name == "ellipse")
                {
                    const auto rx_attribute = parse_length(e.find("rx").value_or(""));
                    const auto ry_attribute = parse_length(e.find("ry").value_or(""));
                    float rx = rx_attribute ? resolve(*rx_attribute, vp, axis::x) : -1.0f;
                    float ry = ry_attribute ? resolve(*ry_attribute, vp, axis::y) : -1.0f;
                    if (rx < 0.0f)
                        rx = ry;
                    if (ry < 0.0f)
                        ry = rx;
                    if (rx > 0.0f && ry > 0.0f)
                        p.add_ellipse({len("cx", axis::x), len("cy", axis::y)}, {rx, ry});
                }
                else if (name == "line")
                {
                    p.move_to({len("x1", axis::x), len("y1", axis::y)});
                    p.line_to({len("x2", axis::x), len("y2", axis::y)});
                }
                else if (name == "polyline" || name == "polygon")
                {
                    const std::vector<float> v = parse_numbers(e.find("points").value_or(""));
                    for (std::size_t i = 0; i + 1 < v.size(); i += 2)
                    {
                        if (i == 0)
                            p.move_to({v[i], v[i + 1]});
                        else
                            p.line_to({v[i], v[i + 1]});
                    }
                    if (name == "polygon" && v.size() >= 4)
                        p.close();
                }
                return p;
            }

            void draw_shape(const xml::element &e, const style &s, const own_properties &own, const math::mat3f &t,
                            const viewport &vp)
            {
                static constexpr std::string_view shapes[] = {"path", "rect", "circle", "ellipse",
                                                              "line", "polyline", "polygon"};
                const std::string_view name = e.local_name();
                if (std::find(std::begin(shapes), std::end(shapes), name) == std::end(shapes))
                    return; // unknown elements are skipped
                if (!s.visible)
                    return;

                shape out;
                out.path = geometry(e, vp);
                if (out.path.empty())
                    return;
                out.transform = t;

                const path::bounding_box box = out.path.bounds();
                out.fill = resolve_paint(s.fill, s, box, vp);
                out.fill_opacity = s.fill_opacity;
                out.fill_rule = s.rule;
                out.stroke = resolve_paint(s.stroke, s, box, vp);
                out.stroke_opacity = s.stroke_opacity;
                out.stroke_style.width = resolve(s.stroke_width, vp, axis::other);
                out.stroke_style.cap = s.cap;
                out.stroke_style.join = s.join;
                out.stroke_style.miter_limit = s.miter_limit;
                for (const length &l : s.dashes)
                    out.stroke_style.dashes.push_back(resolve(l, vp, axis::other));
                out.stroke_style.dash_offset = resolve(s.dash_offset, vp, axis::other);
                if (!(out.stroke_style.width > 0.0f))
                    out.stroke.type = paint::kind::none;

                const bool fills = out.fill.type != paint::kind::none;
                const bool strokes = out.stroke.type != paint::kind::none;
                if (!fills && !strokes)
                    return;

                // Opacity on a shape that is only filled or only stroked is
                // the same as on its paint; with both, the two overlap and
                // must be composited together.
                const bool layered = own.opacity < 1.0f && fills && strokes;
                if (!layered)
                {
                    out.fill_opacity *= own.opacity;
                    out.stroke_opacity *= own.opacity;
                }
                if (layered)
                    begin_group(own.opacity);
                item i;
                i.type = item::kind::shape;
                i.shape = std::move(out);
                m_document.items.push_back(std::move(i));
                if (layered)
                    end_group();
            }

            // ---- paint servers -------------------------------------------------------------

            paint resolve_paint(const paint_value &value, const style &s, const path::bounding_box &box,
                                const viewport &vp) const
            {
                switch (value.type)
                {
                case paint_value::kind::none:
                    return {};
                case paint_value::kind::color:
                    return paint::solid(value.color);
                case paint_value::kind::current_color:
                    return paint::solid(s.color);
                case paint_value::kind::url:
                    break;
                }

                const auto found = m_ids.find(value.url);
                if (found != m_ids.end())
                {
                    const std::string_view kind = found->second->local_name();
                    if (kind == "linearGradient" || kind == "radialGradient")
                        return gradient_paint(*found->second, box, vp);
                }
                // Not something that paints (a pattern, or nothing at all):
                // the fallback, if any.
                if (value.fallback == paint_value::kind::color)
                    return paint::solid(value.fallback_color);
                if (value.fallback == paint_value::kind::current_color)
                    return paint::solid(s.color);
                return {};
            }

            paint gradient_paint(const xml::element &first, const path::bounding_box &box, const viewport &vp) const
            {
                // The chain of gradients linked by href, which lend each other
                // attributes and stops.
                std::vector<const xml::element *> chain{&first};
                while (chain.size() < 16)
                {
                    const xml::element *next = referenced(*chain.back());
                    if (next == nullptr || std::find(chain.begin(), chain.end(), next) != chain.end())
                        break;
                    const std::string_view kind = next->local_name();
                    if (kind != "linearGradient" && kind != "radialGradient")
                        break;
                    chain.push_back(next);
                }
                const auto attribute = [&](const char *name) -> std::optional<std::string_view> {
                    for (const xml::element *g : chain)
                        if (const auto v = g->find(name))
                            return v;
                    return std::nullopt;
                };

                auto result = std::make_shared<gradient>();
                const bool bounding_box = attribute("gradientUnits").value_or("objectBoundingBox") != "userSpaceOnUse";
                const auto coordinate = [&](const char *name, const char *fallback, axis a) {
                    const auto l = parse_length(attribute(name).value_or(fallback));
                    if (!l)
                        return parse_length(fallback)->value / (bounding_box ? 100.0f : 1.0f);
                    if (bounding_box)
                        return l->type == length::unit::percent ? l->value / 100.0f : l->value;
                    return resolve(*l, vp, a);
                };

                if (first.local_name() == "linearGradient")
                {
                    result->type = gradient::kind::linear;
                    result->start = {coordinate("x1", "0%", axis::x), coordinate("y1", "0%", axis::y)};
                    result->end = {coordinate("x2", "100%", axis::x), coordinate("y2", "0%", axis::y)};
                }
                else
                {
                    result->type = gradient::kind::radial;
                    const math::vec2f center{coordinate("cx", "50%", axis::x), coordinate("cy", "50%", axis::y)};
                    result->end = center;
                    result->radius = coordinate("r", "50%", axis::other);
                    const auto fx = attribute("fx");
                    const auto fy = attribute("fy");
                    result->start = {fx ? coordinate("fx", "50%", axis::x) : center[0],
                                     fy ? coordinate("fy", "50%", axis::y) : center[1]};
                    result->focal_radius = coordinate("fr", "0%", axis::other);
                }

                const std::string_view spread = trim(attribute("spreadMethod").value_or("pad"));
                result->spread = spread == "reflect"  ? spread_method::reflect
                                 : spread == "repeat" ? spread_method::repeat
                                                      : spread_method::pad;

                math::mat3f transform = math::mat3f::identity();
                if (const auto text = attribute("gradientTransform"))
                    if (const auto m = parse_transform(*text))
                        transform = *m;
                if (bounding_box)
                {
                    const math::vec2f size = box.max - box.min;
                    // A box with no width or height cannot place the gradient.
                    if (!(size[0] > 0.0f) || !(size[1] > 0.0f))
                        return {};
                    transform = translation(box.min[0], box.min[1]) * scaling(size[0], size[1]) * transform;
                }
                result->transform = transform;

                // Stops from the first gradient in the chain that has any.
                for (const xml::element *g : chain)
                {
                    float previous = 0.0f;
                    for (const xml::element &child : g->children)
                    {
                        if (child.local_name() != "stop")
                            continue;
                        style stop_style;
                        own_properties own;
                        compute(child, stop_style, own);
                        gradient_stop stop;
                        const auto offset = parse_length(child.find("offset").value_or("0"));
                        float value = offset ? (offset->type == length::unit::percent ? offset->value / 100.0f
                                                                                       : offset->value)
                                             : 0.0f;
                        value = std::max(previous, std::clamp(value, 0.0f, 1.0f));
                        previous = value;
                        stop.offset = value;
                        math::vec4f color{0, 0, 0, 1};
                        if (own.stop_color)
                        {
                            const std::string_view text = trim(*own.stop_color);
                            if (text == "currentColor" || text == "currentcolor")
                                color = stop_style.color;
                            else if (const auto parsed = parse_color(text))
                                color = *parsed;
                        }
                        color[3] *= own.stop_opacity;
                        stop.color = color;
                        result->stops.push_back(stop);
                    }
                    if (!result->stops.empty())
                        break;
                }
                if (result->stops.empty())
                    return {};
                return paint::with(std::move(result));
            }

            const xml::element &m_root;
            parse_options m_options;
            std::unordered_map<std::string, const xml::element *> m_ids;
            style_sheet m_sheet;
            document m_document;
            std::set<const xml::element *> m_active;
            int m_use_depth = 0;
        };
    } // namespace

    bool is_svg(std::span<const std::uint8_t> data) noexcept
    {
        if (compress::is_gzip(data))
            return true;
        const std::size_t limit = std::min<std::size_t>(data.size(), 4096);
        const std::string_view head(reinterpret_cast<const char *>(data.data()), limit);
        // Past the BOM, XML declaration, comments and DOCTYPE, the first
        // element is <svg.
        std::size_t i = head.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        while (i < head.size())
        {
            while (i < head.size() && is_space(head[i]))
                ++i;
            if (i >= head.size() || head[i] != '<')
                return false;
            const std::string_view rest = head.substr(i);
            std::string_view terminator;
            if (rest.starts_with("<?"))
                terminator = "?>";
            else if (rest.starts_with("<!--"))
                terminator = "-->";
            else if (rest.starts_with("<!"))
                terminator = ">";
            else
                return rest.starts_with("<svg") || rest.starts_with("<svg:svg");
            const std::size_t end = head.find(terminator, i);
            if (end == std::string_view::npos)
                return false;
            i = end + terminator.size();
        }
        return false;
    }

    std::expected<document, std::string> parse(std::string_view text, const parse_options &options)
    {
        auto root = xml::parse(text);
        if (!root)
            return failure(std::move(root.error()));
        return builder(*root, options).run();
    }

    std::expected<document, std::string> parse(std::span<const std::uint8_t> data, const parse_options &options)
    {
        if (compress::is_gzip(data))
        {
            compress::inflate_options limits;
            limits.max_output = std::size_t{256} << 20;
            auto text = compress::gzip_decompress(data, limits);
            if (!text)
                return failure("the compressed SVG is corrupt: " + text.error());
            return parse(std::string_view(reinterpret_cast<const char *>(text->data()), text->size()), options);
        }
        return parse(std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), options);
    }

} // namespace volt::image::svg
