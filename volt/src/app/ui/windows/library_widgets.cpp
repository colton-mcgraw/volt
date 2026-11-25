#include <app/ui/windows/library_widgets.hpp>

#include <library/schema.hpp>

#include <platform/platform.hpp>
#include <ui/layout/box.hpp>
#include <ui/painter.hpp>
#include <ui/theme.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/text_field.hpp>

#include <format>
#include <system_error>

namespace volt::ui
{
    namespace
    {
        // Separates a scope's library from its category in a node id;
        // neither can hold it.
        constexpr char scope_separator = '\x1f';

        void add_scope_nodes(std::vector<tree_node> &into, const std::vector<scope_node> &nodes, int depth)
        {
            for (const scope_node &n : nodes)
            {
                tree_node made;
                made.text = std::format("{}  ({})", n.label, grouped(n.parts));
                made.id = scope_id(n.library, n.category);
                made.icon = depth == 0 ? icon::folder : icon::none;
                // "All libraries" open, so that its categories are one
                // click away; the libraries closed.
                made.expanded = depth == 0 && n.library.empty();
                add_scope_nodes(made.children, n.children, depth + 1);
                into.push_back(std::move(made));
            }
        }

        // A read-only value in the grid: wrapped over a few lines rather
        // than cut, since what is cut cannot be read anywhere else.
        void add_value(property_grid &grid, std::string name, std::string value, text_role role = text_role::normal)
        {
            auto text = std::make_unique<label>(with_break_points(value), role);
            text->set_wrap(true, 3);
            grid.add_row(std::move(name), std::move(text));
        }
    } // namespace

    std::string grouped(std::size_t count)
    {
        std::string digits = std::to_string(count);
        for (std::size_t at = digits.size(); at > 3; at -= 3)
            digits.insert(at - 3, 1, ',');
        return digits;
    }

    std::string scope_id(std::string_view library, std::string_view category)
    {
        std::string id(library);
        id += scope_separator;
        id += category;
        return id;
    }

    std::pair<std::string, std::string> parse_scope_id(std::string_view id)
    {
        const std::size_t at = id.find(scope_separator);
        if (at == std::string_view::npos)
            return {std::string(id), {}};
        return {std::string(id.substr(0, at)), std::string(id.substr(at + 1))};
    }

    std::vector<tree_node> scope_nodes(const std::vector<scope_node> &tree)
    {
        std::vector<tree_node> nodes;
        add_scope_nodes(nodes, tree, 0);
        return nodes;
    }

    void paint_status_marker(painter &p, const rect &area, std::optional<library::part_status> status)
    {
        if (!status || *status == library::part_status::unlinked)
            return;
        const ui::theme &t = p.theme();
        const float size = 8.0f;
        const rect dot{area.x + (area.width - size) * 0.5f, area.y + (area.height - size) * 0.5f, size, size};
        const float round = size * 0.5f;
        switch (*status)
        {
        case library::part_status::up_to_date:
            p.outline(dot, t.text_dim, round, 1.0f);
            break;
        case library::part_status::source_changed:
            p.fill(dot, t.accent, round);
            break;
        case library::part_status::locally_modified:
            p.outline(dot.outset(1.5f), t.text, round + 1.5f, 1.0f);
            p.fill(dot.inset(1.5f), t.text, round - 1.5f);
            break;
        case library::part_status::diverged:
            p.outline(dot.outset(1.5f), t.accent, round + 1.5f, 1.0f);
            p.fill(dot.inset(1.0f), t.accent, round - 1.0f);
            break;
        case library::part_status::source_missing:
            p.draw_icon(icon::close, dot.outset(1.0f), t.text_disabled);
            break;
        case library::part_status::unlinked:
            break;
        }
    }

    void add_part_details(widget &into, const library::part &part, std::string_view source,
                          const std::filesystem::path &models, std::function<void(const std::string &url)> open_link,
                          const library::library *owner)
    {
        if (!part.description.empty())
        {
            auto &description = into.emplace<label>(part.description);
            description.set_wrap(true, 6);
        }

        auto &grid = into.emplace<property_grid>();
        grid.set_name("part fields");
        grid.set_name_share(0.36f);

        grid.add_section("Part");
        if (!source.empty())
            add_value(grid, "Library", std::string(source));
        for (auto &[name, value] : part_fields(part))
            add_value(grid, std::move(name), std::move(value));

        if (!part.parameters.empty())
        {
            grid.add_section("Parameters");
            for (const library::parameter &parameter : part.parameters)
                add_value(grid, parameter.name, parameter.text());
        }

        // What a simulation would stumble on: a misspelled parameter, one
        // the part's device needs and does not have.
        if (const std::vector<library::issue> issues = library::schema::builtin().check(part); !issues.empty())
        {
            grid.add_section("Checks");
            for (const library::issue &found : issues)
            {
                const bool note = found.level == library::severity::note;
                add_value(grid, found.level == library::severity::error ? "Error" : note ? "Note" : "Warning",
                          found.message, note ? text_role::dim : text_role::accent);
            }
        }

        if (!part.keywords.empty())
        {
            std::string keywords;
            for (const std::string &keyword : part.keywords)
            {
                if (!keywords.empty())
                    keywords += ", ";
                keywords += keyword;
            }
            grid.add_section("Keywords");
            add_value(grid, "Keywords", std::move(keywords));
        }

        if (!part.suppliers.empty())
        {
            grid.add_section("Suppliers");
            for (const library::supplier &supplier : part.suppliers)
                add_value(grid, supplier.name.empty() ? "Supplier" : supplier.name,
                          supplier.sku.empty() ? supplier.url : supplier.sku);
        }

        grid.add_section("Documents");
        if (part.datasheet.empty())
        {
            add_value(grid, "Datasheet", "None given", text_role::dim);
        }
        else
        {
            // A field, so the address can be selected and copied; and, for
            // a web address, a button that opens it in the browser. Only
            // http and https: a library file is not to start programs or
            // open local files.
            auto line = std::make_unique<row>(4.0f);
            line->set_cross_align(align::center);
            auto &field = line->emplace<text_field>(part.datasheet);
            field.set_read_only(true);
            field.set_name("datasheet");
            field.set_tooltip(part.datasheet);
            field.select(0, 0); // the start of the address, not its end
            field.layout().flex = 1.0f;
            if (open_link && platform::is_web_url(part.datasheet))
            {
                auto &open = line->emplace<button>("Open", icon::open);
                open.set_name("open datasheet");
                open.set_tooltip("Open the datasheet in the web browser");
                open.clicked.connect([open_link, url = part.datasheet] { open_link(url); });
            }
            grid.add_row("Datasheet", std::move(line));
        }

        const std::filesystem::path model = model_path(part, models);
        if (model.empty())
        {
            add_value(grid, "3D model", "None given", text_role::dim);
        }
        else if (owner != nullptr && owner->find_file(part.model) != nullptr)
        {
            add_value(grid, "3D model", part.model);
            add_value(grid, "Model file", "In the library file", text_role::dim);
        }
        else
        {
            std::error_code error;
            const bool there = std::filesystem::is_regular_file(model, error);
            add_value(grid, "3D model", part.model);
            add_value(grid, "Model file", there ? "Found in the models folder" : "Missing: not in the models folder",
                      there ? text_role::dim : text_role::accent);
        }
    }

} // namespace volt::ui
