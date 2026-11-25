#include <app/ui/windows/schematic_properties.hpp>

#include <app/ui/windows/part_match.hpp>
#include <schematic/document.hpp>
#include <schematic/symbol.hpp>
#include <ui/layout/box.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/checkbox.hpp>
#include <ui/widgets/dropdown.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/numeric_field.hpp>
#include <ui/widgets/property_grid.hpp>
#include <ui/widgets/text_field.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

namespace volt::ui
{
    using schematic::item_id;
    using schematic::item_kind;
    using schematic::item_ref;

    namespace
    {
        // A text field that says when it loses the focus (what was typed
        // into it is one step until then, as in the part editor), when it
        // is clicked into, and when Escape is pressed in it: then Escape is
        // used up here, so that it does not go on to cancel the tool.
        class edit_field final : public text_field
        {
        public:
            using text_field::text_field;

            signal<> focus_lost;
            signal<> clicked_into;
            signal<> escaped;

        protected:
            void on_focus_changed(bool focused, focus_reason reason) override
            {
                text_field::on_focus_changed(focused, reason);
                if (!focused)
                    focus_lost.emit();
                else if (reason == focus_reason::pointer)
                    clicked_into.emit();
            }

            bool on_key(const key_event &e) override
            {
                const bool escape = e.key == key::escape && e.action != action::release &&
                                    (e.mods & held_modifiers) == modifiers::none;
                const bool used = text_field::on_key(e);
                if (!escape)
                    return used;
                escaped.emit();
                return true;
            }
        };

        // Shows `text` unless the field shows it already: setting it moves
        // the caret to the end, which the field being typed in must not
        // have happen for nothing.
        void show_text(text_field *field, const std::string &text)
        {
            if (field != nullptr && field->text() != text)
                field->set_text(text);
        }

        void show_label(label *shown, std::string text)
        {
            if (shown != nullptr)
                shown->set_text(std::move(text));
        }

        // A number field shows the item's value unless the user is in it.
        void show_number(numeric_field *field, double value)
        {
            if (field != nullptr && !field->has_focus() && field->value() != value)
                field->set_value(value);
        }

        std::unique_ptr<label> value_label(std::string text = {}, text_role role = text_role::normal)
        {
            auto shown = std::make_unique<label>(std::move(text), role);
            shown->set_wrap(true, 2);
            return shown;
        }

        std::string point_text(schematic::point p)
        {
            return std::format("{}, {}", grid_units(p.x()), grid_units(p.y()));
        }

        // "Net label", "Power port", "Note".
        std::string kind_name(schematic::label_kind kind)
        {
            switch (kind)
            {
            case schematic::label_kind::net:
                return "Net label";
            case schematic::label_kind::power:
                return "Power port";
            case schematic::label_kind::note:
                return "Note";
            }
            return {};
        }

        std::vector<std::string> rotation_choices()
        {
            return {rotation_text(0), rotation_text(1), rotation_text(2), rotation_text(3)};
        }

        std::string net_name(const schematic::netlist *nets, std::optional<std::size_t> index)
        {
            if (nets == nullptr || !index || *index >= nets->nets.size())
                return "Not connected";
            return nets->nets[*index].name;
        }
    } // namespace

    // ---- helpers ------------------------------------------------------------------------------

    std::string property_merge_key(std::string_view field, item_id id)
    {
        return std::format("type:{}:{}", field, id);
    }

    std::string grid_units(int sheet_units)
    {
        // Hundredths of a grid square at most: two decimals, trailing zeros
        // dropped.
        std::string text = std::format("{:.2f}", static_cast<double>(sheet_units) / schematic::units_per_grid);
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
        if (text == "-0")
            text = "0";
        return text;
    }

    std::string selection_summary(const schematic::sheet &s, std::span<const item_ref> items)
    {
        std::array<std::size_t, 3> counts{};
        for (const item_ref &item : items)
        {
            if (schematic::contains(s, item))
                ++counts[static_cast<std::size_t>(item.kind)];
        }
        static constexpr std::array<std::pair<const char *, const char *>, 3> names{
            {{"part", "parts"}, {"wire", "wires"}, {"label", "labels"}}};
        std::string text;
        for (std::size_t i = 0; i < counts.size(); ++i)
        {
            if (counts[i] == 0)
                continue;
            if (!text.empty())
                text += ", ";
            text += std::format("{} {}", counts[i], counts[i] == 1 ? names[i].first : names[i].second);
        }
        return text.empty() ? std::string("Nothing") : text;
    }

    std::string rotation_text(std::uint8_t quarter_turns)
    {
        return std::format("{}\xC2\xB0", (quarter_turns % 4) * 90);
    }

    std::vector<pin_net> pin_nets(const schematic::part &p, item_id id, const schematic::netlist &nets)
    {
        std::vector<pin_net> out;
        for (const schematic::pin &pin : schematic::find_symbol(p.symbol).pins)
        {
            pin_net entry{pin.number, pin.name, {}};
            // A pin touching nothing is a net of its own, which says
            // nothing: it is not connected.
            if (const auto net = nets.net_of_pin(id, pin.number); net && *net < nets.nets.size())
            {
                const schematic::net &n = nets.nets[*net];
                if (n.named || n.pins.size() > 1 || !n.wires.empty() || !n.labels.empty())
                    entry.net = n.name;
            }
            out.push_back(std::move(entry));
        }
        return out;
    }

    std::string pin_net_text(const pin_net &pin)
    {
        const std::string net = pin.net.empty() ? std::string("not connected") : pin.net;
        if (pin.name.empty() || pin.name == pin.number)
            return std::format("{}: {}", pin.number, net);
        return std::format("{} ({}): {}", pin.number, pin.name, net);
    }

    std::span<const std::string_view> standard_power_ports() noexcept
    {
        static constexpr std::array<std::string_view, 6> names{"GND", "+3V3", "+5V", "+12V", "-12V", "VCC"};
        return names;
    }

    std::vector<std::string> power_port_names(const schematic::sheet &s)
    {
        std::vector<std::string> names(standard_power_ports().begin(), standard_power_ports().end());
        std::vector<std::string> others;
        for (const auto &e : s.labels)
        {
            if (e.record.kind != schematic::label_kind::power || e.record.text.empty())
                continue;
            if (std::ranges::find(names, e.record.text) == names.end())
                others.push_back(e.record.text);
        }
        std::ranges::sort(others);
        const auto repeats = std::ranges::unique(others);
        others.erase(repeats.begin(), repeats.end());
        names.insert(names.end(), others.begin(), others.end());
        return names;
    }

    std::vector<item_ref> unassigned_parts(const schematic::sheet &s)
    {
        std::vector<item_ref> out;
        for (const auto &e : s.parts)
        {
            if (is_placeholder(e.record))
                out.push_back({item_kind::part, e.id});
        }
        return out;
    }

    // ---- the page -------------------------------------------------------------------------------

    struct selection_properties::fields
    {
        text_field *reference = nullptr;
        text_field *value = nullptr;
        text_field *text = nullptr;
        label *symbol = nullptr;
        label *device = nullptr;
        label *library = nullptr;
        button *reveal = nullptr;
        button *assign = nullptr;
        std::optional<bool> placeholder; // what the library row's colour says
        numeric_field *x = nullptr;
        numeric_field *y = nullptr;
        dropdown *rotation = nullptr;
        checkbox *mirrored = nullptr;
        label *kind = nullptr;
        label *from = nullptr;
        label *to = nullptr;
        label *length = nullptr;
        label *net = nullptr;
        label *summary = nullptr;
        std::vector<label *> pins;
        property_grid *grid = nullptr;
    };

    selection_properties::selection_properties(schematic_tools &tools, hooks h)
        : m_tools(tools), m_hooks(std::move(h)), m_fields(std::make_unique<fields>())
    {
    }

    selection_properties::~selection_properties() = default;

    std::unique_ptr<widget> selection_properties::build()
    {
        auto holder = std::make_unique<column>(0.0f);
        holder->set_name("selection properties");
        m_holder = holder.get();
        *m_fields = {};
        m_made = false;
        m_items.clear();
        m_document = nullptr;
        return holder;
    }

    void selection_properties::detach()
    {
        m_holder = nullptr;
        *m_fields = {};
        m_made = false;
        m_items.clear();
        m_document = nullptr;
    }

    void selection_properties::update()
    {
        if (m_holder == nullptr)
            return;
        const schematic::document *doc = m_tools.document();
        const std::span<const item_ref> selection = m_tools.selection();
        bool again = !m_made || doc != m_document || !std::ranges::equal(selection, m_items);
        if (!again && doc != nullptr && m_items.size() == 1 && m_items[0].kind == item_kind::part)
        {
            const schematic::part *p = doc->find_part(m_items[0].id);
            again = p == nullptr || p->symbol != m_symbol;
        }
        if (again)
            rebuild();
        else
            refresh();
    }

    bool selection_properties::focus_text(item_ref item)
    {
        update();
        if (m_items.size() != 1 || m_items[0] != item)
            return false;
        text_field *field = item.kind == item_kind::part ? m_fields->value : m_fields->text;
        if (field == nullptr || field->owner() == nullptr)
            return false;
        field->focus();
        field->select_all();
        m_handed = true;
        return true;
    }

    void selection_properties::done()
    {
        if (m_hooks.edited)
            m_hooks.edited();
    }

    void selection_properties::typing_done(bool escape)
    {
        if (!escape && !m_handed)
            return; // a field clicked into keeps Enter
        m_handed = false;
        if (m_hooks.return_focus)
            m_hooks.return_focus();
    }

    void selection_properties::rebuild()
    {
        schematic::document *doc = m_tools.document();
        m_building = true;
        // A run of typing ends with its field.
        if (doc != nullptr)
            doc->seal();

        *m_fields = {};
        m_holder->clear_children();
        m_document = doc;
        m_items.assign(m_tools.selection().begin(), m_tools.selection().end());
        m_symbol.clear();
        m_made = true;
        m_handed = false;

        if (doc != nullptr && !m_items.empty())
        {
            m_fields->grid = &m_holder->emplace<property_grid>();
            m_fields->grid->set_name("selection grid");
            if (m_items.size() == 1)
            {
                switch (m_items[0].kind)
                {
                case item_kind::part:
                    build_part(m_items[0].id);
                    break;
                case item_kind::wire:
                    build_wire(m_items[0].id);
                    break;
                case item_kind::label:
                    build_label(m_items[0].id);
                    break;
                }
            }
            else
            {
                build_several();
            }
            build_actions();
        }
        m_building = false;
        refresh();
    }

    void selection_properties::build_part(item_id id)
    {
        const schematic::part *p = m_tools.document()->find_part(id);
        if (p == nullptr)
            return;
        m_symbol = p->symbol;
        property_grid &grid = *m_fields->grid;
        grid.add_section("Part");

        // Typed into: one step per run of typing, sealed when the field is
        // left (or Enter).
        const auto typed = [this](std::unique_ptr<edit_field> &field, std::string name,
                                  std::function<void(schematic::document &, const std::string &)> edit) {
            field->set_name(std::move(name));
            field->changed.connect([this, edit = std::move(edit)](const std::string &text) {
                if (m_building)
                    return;
                if (schematic::document *d = m_tools.document())
                {
                    edit(*d, text);
                    done();
                }
            });
            const auto seal = [this] {
                if (schematic::document *d = m_tools.document())
                    d->seal();
            };
            field->focus_lost.connect(seal);
            field->clicked_into.connect([this] { m_handed = false; });
            field->submitted.connect([this, seal](const std::string &) {
                seal();
                typing_done(false);
            });
            field->escaped.connect([this, seal] {
                seal();
                typing_done(true);
            });
        };

        auto reference = std::make_unique<edit_field>(p->reference);
        typed(reference, "property reference", [id](schematic::document &d, const std::string &text) {
            (void)d.set_reference(id, text, property_merge_key("reference", id));
        });
        m_fields->reference = &grid.add_row("Reference", std::move(reference));

        auto value = std::make_unique<edit_field>(p->value, "e.g. 10k");
        typed(value, "property value", [id](schematic::document &d, const std::string &text) {
            (void)d.set_value(id, text, property_merge_key("value", id));
        });
        m_fields->value = &grid.add_row("Value", std::move(value));

        m_fields->symbol = &grid.add_row("Symbol", value_label());
        m_fields->symbol->set_name("property symbol");
        m_fields->device = &grid.add_row("Device", value_label());
        m_fields->device->set_name("property device");
        m_fields->library = &grid.add_row("Library part", value_label({}, text_role::dim));
        m_fields->library->set_name("property library part");
        // Show in Library and Change Part... for an assigned part, Assign
        // Part... for a placeholder: which, refresh() says.
        // One above the other: side by side they are wider than the row.
        auto buttons = std::make_unique<column>(4.0f);
        buttons->set_name("property part buttons");
        buttons->layout().cross = align::start;
        buttons->set_cross_align(align::start);
        auto &reveal = buttons->emplace<button>("Show in Library", icon::component);
        reveal.set_name("property show in library");
        reveal.set_tooltip("Show this part of the project library in the Library Browser");
        reveal.clicked.connect([this, id] {
            const schematic::document *d = m_tools.document();
            const schematic::part *part = d != nullptr ? d->find_part(id) : nullptr;
            if (part != nullptr && !part->library_part.empty() && m_hooks.reveal)
                m_hooks.reveal(part->library_part);
        });
        m_fields->reveal = &reveal;
        if (m_hooks.assign)
        {
            auto &assign = buttons->emplace<button>("Assign Part\xE2\x80\xA6");
            assign.set_name("property assign part");
            assign.clicked.connect([this] {
                if (m_hooks.assign)
                    m_hooks.assign();
            });
            m_fields->assign = &assign;
        }
        grid.add_row("", std::move(buttons));

        grid.add_section("Placement");
        const double step = std::max(m_tools.grid(), 1) / static_cast<double>(schematic::units_per_grid);
        const auto coordinate = [&](std::string name, bool y) {
            auto field = std::make_unique<numeric_field>(0.0, -1.0e6, 1.0e6, step);
            field->set_name(std::move(name));
            field->set_decimals(2);
            field->value_changed.connect([this, id, y](double v) {
                if (m_building)
                    return;
                schematic::document *d = m_tools.document();
                const schematic::part *part = d != nullptr ? d->find_part(id) : nullptr;
                if (part == nullptr)
                    return;
                const int target = static_cast<int>(std::lround(v * schematic::units_per_grid));
                const schematic::point delta = y ? schematic::point{0, target - part->position.y()}
                                                 : schematic::point{target - part->position.x(), 0};
                // The wires attached to it follow, as in a drag.
                const std::array<item_ref, 1> items{item_ref{item_kind::part, id}};
                if (d->move_with_wires(items, delta, {}, m_tools.connectivity()))
                    done();
            });
            return field;
        };
        m_fields->x = &grid.add_row("X", coordinate("property x", false));
        m_fields->y = &grid.add_row("Y", coordinate("property y", true));

        auto rotation = std::make_unique<dropdown>(rotation_choices(), 0);
        rotation->set_name("property rotation");
        rotation->changed.connect([this, id](int index) {
            schematic::document *d = m_tools.document();
            const schematic::part *part = d != nullptr ? d->find_part(id) : nullptr;
            if (part == nullptr || index < 0)
                return;
            schematic::part next = *part;
            next.rotation = static_cast<std::uint8_t>(index % 4);
            if (d->replace_part(id, std::move(next)))
                done();
        });
        m_fields->rotation = &grid.add_row("Rotation", std::move(rotation));

        auto mirrored = std::make_unique<checkbox>("", p->mirrored);
        mirrored->set_name("property mirrored");
        mirrored->layout().cross = align::start;
        mirrored->changed.connect([this, id](bool on) {
            schematic::document *d = m_tools.document();
            const schematic::part *part = d != nullptr ? d->find_part(id) : nullptr;
            if (part == nullptr)
                return;
            schematic::part next = *part;
            next.mirrored = on;
            if (d->replace_part(id, std::move(next)))
                done();
        });
        m_fields->mirrored = &grid.add_row("Mirrored", std::move(mirrored));

        // The nets of its pins; filled in by refresh().
        const std::vector<schematic::pin> &pins = schematic::find_symbol(p->symbol).pins;
        if (!pins.empty())
        {
            grid.add_section("Pins");
            for (const schematic::pin &pin : pins)
            {
                const std::string name = pin.name.empty() || pin.name == pin.number
                                             ? std::format("Pin {}", pin.number)
                                             : std::format("Pin {} ({})", pin.number, pin.name);
                label &shown = grid.add_row(name, value_label());
                shown.set_name("property pin " + pin.number);
                m_fields->pins.push_back(&shown);
            }
        }
    }

    void selection_properties::build_wire(item_id)
    {
        property_grid &grid = *m_fields->grid;
        grid.add_section("Wire");
        m_fields->from = &grid.add_row("From", value_label());
        m_fields->from->set_name("property from");
        m_fields->to = &grid.add_row("To", value_label());
        m_fields->to->set_name("property to");
        m_fields->length = &grid.add_row("Length", value_label());
        m_fields->length->set_name("property length");
        m_fields->net = &grid.add_row("Net", value_label());
        m_fields->net->set_name("property net");
    }

    void selection_properties::build_label(item_id id)
    {
        const schematic::label *l = m_tools.document()->find_label(id);
        if (l == nullptr)
            return;
        property_grid &grid = *m_fields->grid;
        grid.add_section(kind_name(l->kind));

        auto text = std::make_unique<edit_field>(l->text);
        text->set_name("property text");
        text->changed.connect([this, id, kind = l->kind](const std::string &typed) {
            if (m_building)
                return;
            if (schematic::document *d = m_tools.document())
            {
                (void)d->set_label_text(id, typed, property_merge_key("label", id));
                // A label just placed, being named: the next one counts on
                // from its name.
                if (m_handed && m_hooks.renamed)
                    m_hooks.renamed(kind, typed);
                done();
            }
        });
        const auto seal = [this] {
            if (schematic::document *d = m_tools.document())
                d->seal();
        };
        text->focus_lost.connect(seal);
        text->clicked_into.connect([this] { m_handed = false; });
        text->submitted.connect([this, seal](const std::string &) {
            seal();
            typing_done(false);
        });
        text->escaped.connect([this, seal] {
            seal();
            typing_done(true);
        });
        m_fields->text = &grid.add_row("Text", std::move(text));

        m_fields->kind = &grid.add_row("Kind", value_label());
        m_fields->kind->set_name("property kind");

        const double step = std::max(m_tools.grid(), 1) / static_cast<double>(schematic::units_per_grid);
        const auto coordinate = [&](std::string name, bool y) {
            auto field = std::make_unique<numeric_field>(0.0, -1.0e6, 1.0e6, step);
            field->set_name(std::move(name));
            field->set_decimals(2);
            field->value_changed.connect([this, id, y](double v) {
                if (m_building)
                    return;
                schematic::document *d = m_tools.document();
                const schematic::label *found = d != nullptr ? d->find_label(id) : nullptr;
                if (found == nullptr)
                    return;
                const int target = static_cast<int>(std::lround(v * schematic::units_per_grid));
                const schematic::point delta = y ? schematic::point{0, target - found->position.y()}
                                                 : schematic::point{target - found->position.x(), 0};
                const std::array<item_ref, 1> items{item_ref{item_kind::label, id}};
                if (d->move_with_wires(items, delta, {}, m_tools.connectivity()))
                    done();
            });
            return field;
        };
        m_fields->x = &grid.add_row("X", coordinate("property x", false));
        m_fields->y = &grid.add_row("Y", coordinate("property y", true));

        auto rotation = std::make_unique<dropdown>(rotation_choices(), 0);
        rotation->set_name("property rotation");
        rotation->changed.connect([this, id](int index) {
            schematic::document *d = m_tools.document();
            const schematic::label *found = d != nullptr ? d->find_label(id) : nullptr;
            if (found == nullptr || index < 0)
                return;
            schematic::label next = *found;
            next.rotation = static_cast<std::uint8_t>(index % 4);
            if (d->replace_label(id, std::move(next)))
                done();
        });
        m_fields->rotation = &grid.add_row("Rotation", std::move(rotation));

        if (l->kind != schematic::label_kind::note)
        {
            m_fields->net = &grid.add_row("Net", value_label());
            m_fields->net->set_name("property net");
        }
    }

    void selection_properties::build_several()
    {
        property_grid &grid = *m_fields->grid;
        grid.add_section("Selection");
        m_fields->summary = &grid.add_row("Selected", value_label());
        m_fields->summary->set_name("property summary");
    }

    void selection_properties::build_actions()
    {
        // What the Edit menu does with the selection, at hand.
        auto &actions = m_holder->emplace<row>(6.0f, insets{8.0f, 4.0f, 8.0f, 8.0f});
        actions.set_name("selection actions");
        const auto action = [&](const char *text, ui::icon icon, const char *name, const char *tooltip,
                                std::function<void()> act) {
            auto &b = actions.emplace<button>(text, icon);
            b.set_name(name);
            b.set_tooltip(tooltip);
            b.clicked.connect([this, act = std::move(act)] {
                act();
                done();
            });
        };
        action("Rotate", icon::none, "selection rotate", "Turn a quarter turn counter-clockwise (R)",
               [this] { (void)m_tools.rotate_selection(false); });
        action("Mirror", icon::none, "selection mirror", "Mirror left for right (X)",
               [this] { (void)m_tools.mirror_selection(true); });
        action("Delete", icon::close, "selection delete", "Delete the selection (Delete)",
               [this] { (void)m_tools.delete_selection(); });
    }

    void selection_properties::refresh()
    {
        const schematic::document *doc = m_tools.document();
        if (doc == nullptr || m_items.empty())
            return;
        const schematic::netlist *nets = m_hooks.netlist ? m_hooks.netlist() : nullptr;
        fields &f = *m_fields;

        if (m_items.size() > 1)
        {
            show_label(f.summary, selection_summary(doc->sheet(), m_items));
            return;
        }

        const item_ref item = m_items[0];
        switch (item.kind)
        {
        case item_kind::part:
        {
            const schematic::part *p = doc->find_part(item.id);
            if (p == nullptr)
                return;
            show_text(f.reference, p->reference);
            show_text(f.value, p->value);
            show_label(f.symbol, p->symbol);
            show_label(f.device, p->device.empty() ? std::string("None (not simulated)") : p->device);
            const bool placeholder = is_placeholder(*p);
            std::string library_name = p->library_part;
            if (!library_name.empty() && m_hooks.part_name)
                if (std::string named = m_hooks.part_name(p->library_part); !named.empty())
                    library_name = std::move(named);
            show_label(f.library, placeholder ? std::string("Placeholder \xE2\x80\x94 no part assigned")
                                  : p->library_part.empty() ? std::string("None needed")
                                                            : library_name);
            if (f.library != nullptr && f.placeholder != placeholder)
            {
                f.library->set_role(placeholder ? text_role::accent : text_role::dim);
                f.placeholder = placeholder;
            }
            if (f.reveal != nullptr)
            {
                f.reveal->set_visible(!p->library_part.empty());
                f.reveal->set_enabled(!p->library_part.empty() && m_hooks.reveal &&
                                      (!m_hooks.can_reveal || m_hooks.can_reveal()));
            }
            if (f.assign != nullptr)
            {
                f.assign->set_visible(placeholder || !p->library_part.empty());
                f.assign->set_text(placeholder ? "Assign Part\xE2\x80\xA6" : "Change Part\xE2\x80\xA6");
                f.assign->set_tooltip(placeholder
                                          ? "Choose a library part for this placeholder in the Parts tab (it is "
                                            "kept in the open project's library)"
                                          : "Choose another library part of the same kind and value in the Parts "
                                            "tab");
            }
            show_number(f.x, p->position.x() / static_cast<double>(schematic::units_per_grid));
            show_number(f.y, p->position.y() / static_cast<double>(schematic::units_per_grid));
            if (f.rotation != nullptr && f.rotation->selected() != p->rotation % 4)
                f.rotation->set_selected(p->rotation % 4);
            if (f.mirrored != nullptr && f.mirrored->checked() != p->mirrored)
                f.mirrored->set_checked(p->mirrored);
            if (nets != nullptr)
            {
                const std::vector<pin_net> list = pin_nets(*p, item.id, *nets);
                for (std::size_t i = 0; i < list.size() && i < f.pins.size(); ++i)
                    show_label(f.pins[i], list[i].net.empty() ? std::string("Not connected") : list[i].net);
            }
            break;
        }
        case item_kind::wire:
        {
            const schematic::wire *w = doc->find_wire(item.id);
            if (w == nullptr)
                return;
            show_label(f.from, point_text(w->a));
            show_label(f.to, point_text(w->b));
            const double dx = w->b.x() - w->a.x();
            const double dy = w->b.y() - w->a.y();
            const double length = std::sqrt(dx * dx + dy * dy) / schematic::units_per_grid;
            show_label(f.length, std::format("{:g}", std::round(length * 100.0) / 100.0));
            show_label(f.net, net_name(nets, nets != nullptr ? nets->net_of_wire(item.id) : std::nullopt));
            break;
        }
        case item_kind::label:
        {
            const schematic::label *l = doc->find_label(item.id);
            if (l == nullptr)
                return;
            show_text(f.text, l->text);
            show_label(f.kind, kind_name(l->kind));
            show_number(f.x, l->position.x() / static_cast<double>(schematic::units_per_grid));
            show_number(f.y, l->position.y() / static_cast<double>(schematic::units_per_grid));
            if (f.rotation != nullptr && f.rotation->selected() != l->rotation % 4)
                f.rotation->set_selected(l->rotation % 4);
            show_label(f.net, net_name(nets, nets != nullptr ? nets->net_of_label(item.id) : std::nullopt));
            break;
        }
        }
    }

    // ---- the Tool section ------------------------------------------------------------------------

    namespace
    {
        // The label kind a tool places, if it places labels.
        std::optional<schematic::label_kind> label_kind_of(schematic_tools::tool t)
        {
            switch (t)
            {
            case schematic_tools::tool::label:
                return schematic::label_kind::net;
            case schematic_tools::tool::power:
                return schematic::label_kind::power;
            case schematic_tools::tool::text:
                return schematic::label_kind::note;
            case schematic_tools::tool::select:
            case schematic_tools::tool::wire:
            case schematic_tools::tool::part:
                break;
            }
            return std::nullopt;
        }

        // Which part is armed, as far as the section's rows go (not its
        // value, which is typed into there).
        std::string part_key(const schematic_tools &tools)
        {
            const std::optional<schematic::part> &p = tools.pending_part();
            return p ? p->symbol + "\n" + p->library_part : std::string();
        }
    } // namespace

    struct tool_options::fields
    {
        edit_field *field = nullptr; // a power port's name, a label's next text, a part's value
        dropdown *presets = nullptr;
        std::vector<std::string> preset_names;
        label *symbol = nullptr;
        label *library = nullptr;
    };

    tool_options::tool_options(schematic_tools &tools, hooks h)
        : m_tools(tools), m_hooks(std::move(h)), m_fields(std::make_unique<fields>())
    {
    }

    tool_options::~tool_options() = default;

    std::unique_ptr<widget> tool_options::build()
    {
        auto holder = std::make_unique<column>(0.0f);
        holder->set_name("tool options");
        m_holder = holder.get();
        *m_fields = {};
        m_tool.reset();
        return holder;
    }

    void tool_options::detach()
    {
        m_holder = nullptr;
        *m_fields = {};
        m_tool.reset();
    }

    void tool_options::update()
    {
        if (m_holder == nullptr)
            return;
        if (m_tool != m_tools.active_tool() || m_part_key != part_key(m_tools) || m_document != m_tools.document())
            rebuild();
        else
            refresh();
    }

    bool tool_options::focus_field()
    {
        update();
        text_field *field = m_fields->field;
        if (field == nullptr || field->owner() == nullptr)
            return false;
        field->focus();
        field->select_all();
        return true;
    }

    void tool_options::rebuild()
    {
        m_building = true;
        *m_fields = {};
        m_holder->clear_children();
        m_tool = m_tools.active_tool();
        m_part_key = part_key(m_tools);
        m_document = m_tools.document();
        m_presets_revision = 0;

        const schematic_tools::tool t = *m_tool;
        const std::optional<schematic::label_kind> kind = label_kind_of(t);
        if (m_document == nullptr || (!kind && t != schematic_tools::tool::part))
        {
            m_building = false;
            return;
        }

        auto &grid = m_holder->emplace<property_grid>();
        grid.set_name("tool grid");
        const auto edited = [this] {
            if (m_hooks.edited)
                m_hooks.edited();
        };
        const auto give_back = [this] {
            if (m_hooks.return_focus)
                m_hooks.return_focus();
        };
        std::string hint;

        if (kind)
        {
            const schematic::label_kind k = *kind;
            grid.add_section(k == schematic::label_kind::power ? "Tool: Power Port"
                             : k == schematic::label_kind::net ? "Tool: Net Label"
                                                               : "Tool: Text");
            auto field = std::make_unique<edit_field>(m_tools.label_text(k),
                                                      k == schematic::label_kind::power ? "e.g. +5V, VBAT" : "");
            field->set_name("tool text");
            field->set_tooltip(k == schematic::label_kind::power
                                   ? "The net the next power port joins: type a name and press Enter, then click "
                                     "on the sheet to place it"
                                   : "What the next one placed says");
            field->changed.connect([this, k, edited](const std::string &text) {
                // An empty name is not armed: the last one stays until
                // something is typed.
                if (m_building || text.empty())
                    return;
                m_tools.set_label_text(k, text);
                edited();
            });
            // Enter or Escape: done, what was typed kept (an empty field
            // shows what is armed again).
            const auto finish = [this, k, give_back] {
                if (m_fields->field != nullptr && m_fields->field->text().empty())
                    m_fields->field->set_text(m_tools.label_text(k));
                give_back();
            };
            field->submitted.connect([finish](const std::string &) { finish(); });
            field->escaped.connect(finish);
            m_fields->field = &grid.add_row(k == schematic::label_kind::power ? "Name" : "Next text", std::move(field));

            if (k == schematic::label_kind::power)
            {
                auto presets = std::make_unique<dropdown>(std::vector<std::string>{}, -1);
                presets->set_name("tool presets");
                presets->set_tooltip("The usual power ports, then those already on the sheet");
                presets->changed.connect([this, edited, give_back](int index) {
                    if (m_building || index < 0 || static_cast<std::size_t>(index) >= m_fields->preset_names.size())
                        return;
                    m_tools.begin_place_power(m_fields->preset_names[static_cast<std::size_t>(index)]);
                    edited();
                    give_back();
                });
                m_fields->presets = &grid.add_row("Preset", std::move(presets));
                hint = "Click on a pin or a wire end to place it; R rotates. P opens the list of ports.";
            }
            else if (k == schematic::label_kind::net)
                hint = "Click on a wire or a pin to place it; a number at the end counts up with each one. R "
                       "rotates.";
            else
                hint = "Click to place the note; R rotates.";
        }
        else
        {
            grid.add_section("Tool: Part");
            const std::optional<schematic::part> &p = m_tools.pending_part();
            if (!p)
            {
                auto choose = std::make_unique<button>("Choose Part\xE2\x80\xA6", icon::component);
                choose->set_name("tool choose part");
                choose->layout().cross = align::start;
                choose->set_tooltip("Find a part in the Parts tab (A)");
                choose->clicked.connect([this] {
                    if (m_hooks.choose_part)
                        m_hooks.choose_part();
                });
                grid.add_row("Nothing armed", std::move(choose));
            }
            else
            {
                m_fields->symbol = &grid.add_row("Symbol", value_label(p->symbol));
                m_fields->symbol->set_name("tool symbol");
                auto field = std::make_unique<edit_field>(p->value, "e.g. 10k");
                field->set_name("tool value");
                field->set_tooltip("The value the next parts placed get");
                field->changed.connect([this, edited](const std::string &text) {
                    if (m_building || !m_tools.pending_part())
                        return;
                    schematic::part next = *m_tools.pending_part();
                    next.value = text;
                    m_tools.begin_place_part(std::move(next));
                    edited();
                });
                field->submitted.connect([give_back](const std::string &) { give_back(); });
                field->escaped.connect(give_back);
                m_fields->field = &grid.add_row("Value", std::move(field));
                m_fields->library = &grid.add_row(
                    "Library part", value_label(p->library_part.empty() && is_placeholder(*p)
                                                    ? std::string("Placeholder \xE2\x80\x94 assign one later")
                                                : p->library_part.empty() ? std::string("None needed")
                                                                          : p->library_part,
                                                text_role::dim));
                m_fields->library->set_name("tool library part");
                hint = "Click to place (on a wire, it cuts the wire); R rotates, X and Y mirror, before or "
                       "while placing.";
            }
        }

        if (!hint.empty())
        {
            auto &room = m_holder->emplace<column>(0.0f, insets{8.0f, 0.0f, 8.0f, 6.0f});
            auto &note = room.emplace<label>(std::move(hint), text_role::dim);
            note.set_name("tool hint");
            note.set_size(text_size::small);
            note.set_wrap(true, 3);
        }
        m_building = false;
        refresh();
    }

    void tool_options::refresh()
    {
        if (!m_tool || m_document == nullptr)
            return;
        fields &f = *m_fields;
        m_building = true;
        if (const std::optional<schematic::label_kind> kind = label_kind_of(*m_tool))
        {
            const std::string text = m_tools.label_text(*kind);
            if (f.field != nullptr && !f.field->has_focus())
                show_text(f.field, text);
            if (f.presets != nullptr)
            {
                if (m_presets_revision != m_document->revision() || f.preset_names.empty())
                {
                    std::vector<std::string> names = power_port_names(m_document->sheet());
                    if (names != f.preset_names)
                    {
                        f.preset_names = names;
                        f.presets->set_items(std::move(names));
                    }
                    m_presets_revision = m_document->revision();
                }
                const auto at = std::ranges::find(f.preset_names, text);
                f.presets->set_selected(at == f.preset_names.end() ? -1
                                                                   : static_cast<int>(at - f.preset_names.begin()));
            }
        }
        else if (const std::optional<schematic::part> &p = m_tools.pending_part(); p && f.field != nullptr)
        {
            if (!f.field->has_focus())
                show_text(f.field, p->value);
        }
        m_building = false;
    }

} // namespace volt::ui
