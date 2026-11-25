#pragma once

#include <app/ui/windows/library_model.hpp>
#include <ui/layout/box.hpp>
#include <ui/widgets/list_view.hpp>
#include <ui/widgets/text_field.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// library widgets
// -------------------------------------------------------------------------
//
// Pieces the Library Browser and the parts panel both build from:
//
//   search_field        a text field that hands Up, Down, Page Up and Page
//                       Down to the list it searches (a text field keeps
//                       every plain key it gets, so they would not bubble)
//   key_column          a column that sees the keys its children leave
//                       (bubbled up to it): Escape, from a search field
//   status marker       the small dot that says how a project part compares
//                       with its source, in theme colours
//   scope tree nodes    the libraries and categories as tree_view nodes
//   part details        a part's fields, parameters, keywords, datasheet
//                       and model, as a column of widgets

namespace volt::ui
{

    class app_services;
    class painter;
    struct theme;

    class search_field final : public text_field
    {
    public:
        using text_field::text_field;

        // Up, Down, Page Up, Page Down (without modifiers); returns whether
        // it moved anything.
        std::function<bool(key k)> navigate;

    protected:
        bool on_key(const key_event &e) override
        {
            if (e.action != action::release && navigate && (e.mods & held_modifiers) == modifiers::none &&
                (e.key == key::up || e.key == key::down || e.key == key::page_up || e.key == key::page_down) &&
                navigate(e.key))
                return true;
            return text_field::on_key(e);
        }
    };

    class key_column final : public column
    {
    public:
        explicit key_column(float spacing = 0.0f, insets padding = {}) : column(spacing, padding) {}

        // Returns whether it handled the key.
        std::function<bool(const key_event &)> keys;

    protected:
        bool on_key(const key_event &e) override { return keys && keys(e); }
    };

    // Draws the status marker centred in `area`: nothing for a part made in
    // the project, a hollow ring when up to date, a filled dot when the
    // source changed, a dot in a ring when edited here, both when diverged,
    // a dim cross when the source is missing.
    void paint_status_marker(painter &p, const rect &area, std::optional<library::part_status> status);

    // The scope tree as tree nodes, "Label (count)"; the node id is
    // scope_id(library, category).
    std::vector<tree_node> scope_nodes(const std::vector<scope_node> &tree);
    std::string scope_id(std::string_view library, std::string_view category);
    std::pair<std::string, std::string> parse_scope_id(std::string_view id);

    // Adds the details of `part` (of library `source`, empty for a project
    // part) to `into`: the description, a grid of its fields, its
    // parameters, keywords, datasheet and 3D model (and whether the file is
    // there, under `models`). Long values break at ':', '_' and '/'
    // (with_break_points()) before they break anywhere. A datasheet that
    // is a web address gets an Open button, which calls `open_link` with
    // it (app_services::open_link()); without one, or for anything else,
    // the address can only be selected and copied. What the schema
    // (<library/schema.hpp>) finds wrong with the part's parameters is
    // listed under "Checks". `owner` is the library the part is in, which
    // may carry the model file itself; without it only the models folder
    // is looked in.
    void add_part_details(widget &into, const library::part &part, std::string_view source,
                          const std::filesystem::path &models,
                          std::function<void(const std::string &url)> open_link = {},
                          const library::library *owner = nullptr);

    // "1,234": counts as the browser shows them.
    std::string grouped(std::size_t count);

} // namespace volt::ui
