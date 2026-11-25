// The schematic editor's pieces that need no window and no GPU: the view's
// controller (<app/ui/windows/schematic_view.hpp>) handing pointer and key
// events to the tools and what they leave to the camera, its tolerance
// following the zoom, and the context menu's rule for right clicks, first
// with events made by hand and then through a real viewport and the real
// input processor (so a click arrives before its release, as in a window);
// and the selection's properties page (<app/ui/windows/
// schematic_properties.hpp>) on a screen: what it shows for a part, a
// wire, a label and several items, typing into Value as one undoable step
// per run of typing, a field kept (not made again) while the document
// changes under it, positions typed in (the wires following), a
// placeholder's Assign Part…, and its helpers; then the canvas and the page
// on one screen: a label placed hands its Text field the keyboard, plain
// keys typed there reach no tool and no shortcut, Enter or Escape hands it
// back with the tool armed and the next label counting on from the name;
// and the Tool section of each placing tool.

#include "ui/harness.hpp"

#include <app/ui/windows/schematic_properties.hpp>
#include <app/ui/windows/schematic_tools.hpp>
#include <app/ui/windows/schematic_view.hpp>
#include <schematic/document.hpp>
#include <schematic/netlist.hpp>
#include <schematic/symbol.hpp>
#include <ui/layout/box.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/dropdown.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/numeric_field.hpp>
#include <ui/widgets/text_field.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace volt;
using namespace volt::tests::ui;
using schematic::item_id;
using schematic::item_kind;
using schematic::item_ref;
using tools_t = volt::ui::schematic_tools;

namespace
{
    schematic::point pt(int x, int y) { return {x, y}; }
    item_ref part_ref(item_id id) { return {item_kind::part, id}; }
    item_ref wire_ref(item_id id) { return {item_kind::wire, id}; }
    item_ref label_ref(item_id id) { return {item_kind::label, id}; }

    // R1 (1) at the origin and C1 (2) to its right, a wire (3) from R1's
    // pin 2 to C1's pin 1, and a net label (4) on it.
    schematic::sheet small_sheet()
    {
        schematic::sheet s;
        schematic::part r;
        r.symbol = "R";
        r.reference = "R1";
        r.device = "resistor";
        (void)s.parts.insert(1, r);
        schematic::part c;
        c.symbol = "C";
        c.reference = "C1";
        c.position = {600, 0};
        (void)s.parts.insert(2, c);
        (void)s.wires.insert(3, schematic::wire{{100, 0}, {500, 0}, {}});
        schematic::label l;
        l.position = {300, 0};
        l.text = "MID";
        (void)s.labels.insert(4, l);
        return s;
    }

    constexpr math::vec2f view_size{800.0f, 600.0f};

    // A view 800 by 600 pixels on the camera: the pixel of a sheet point.
    math::vec2f pixel_of(const pan_zoom_controller &camera, float x, float y)
    {
        return camera.to_pixels(math::vec2f{x, y} / 100.0f, view_size);
    }

    viewport_pointer pointer(math::vec2f position, mouse_button button, mouse_buttons held,
                             math::vec2f delta = {}, int clicks = 1)
    {
        viewport_pointer e;
        e.position = position;
        e.delta = delta;
        e.size = view_size;
        e.button = button;
        e.buttons = held;
        e.clicks = clicks;
        return e;
    }

    key_event press_key(key k, modifiers mods = modifiers::none)
    {
        key_event e{};
        e.key = k;
        e.action = action::press;
        e.mods = mods;
        return e;
    }

    // The document, the tools on it, the camera and the controller between.
    struct rig
    {
        schematic::document doc{small_sheet()};
        tools_t tools;
        pan_zoom_controller camera;
        schematic_view_controller controller{tools, camera};
        int told = 0;

        rig()
        {
            tools.set_document(&doc);
            camera.zoom = 50.0f;
            controller.tools_changed = [this] { ++told; };
        }
        rig(const rig &) = delete;
        rig &operator=(const rig &) = delete;

        math::vec2f px(float x, float y) const { return pixel_of(camera, x, y); }

        void click(math::vec2f at, mouse_button button = mouse_button::left)
        {
            (void)controller.pointer_down(pointer(at, button, mask_of(button)));
            (void)controller.pointer_up(pointer(at, button, mouse_buttons::none));
        }
    };

    std::vector<item_ref> selection_of(const tools_t &t)
    {
        return {t.selection().begin(), t.selection().end()};
    }

    // ---- the controller, event by event ---------------------------------------------------

    void sheet_units_and_tolerance()
    {
        rig r;
        // The view's centre is the camera's centre; 50 pixels a grid square.
        const math::vec2f s = r.controller.to_sheet({450.0f, 275.0f}, view_size);
        CHECK(near(s.x(), 100.0f) && near(s.y(), 50.0f));

        // Five logical units at 50 pixels a grid square (100 sheet units).
        CHECK(near(r.controller.tolerance(), 10.0f));
        r.controller.pixel_scale = 2.0f; // the same distance on a denser display
        CHECK(near(r.controller.tolerance(), 20.0f));
        r.controller.pixel_scale = 1.0f;
        r.camera.zoom = 10.0f; // zoomed out: the same pixels are more of the sheet
        CHECK(near(r.controller.tolerance(), 50.0f));

        // Which decides what a click picks: 40 units below the wire is too
        // far at 50 pixels a square, near enough at 10 (the wire before
        // R1's body, which is then near enough too).
        r.camera.zoom = 50.0f;
        r.click(r.px(150.0f, -40.0f));
        CHECK(r.tools.selection().empty());
        r.camera.zoom = 10.0f;
        r.click(r.px(150.0f, -40.0f));
        CHECK(selection_of(r.tools) == std::vector<item_ref>{wire_ref(3)});
    }

    void left_to_the_tools_middle_and_right_to_the_camera()
    {
        rig r;
        const math::vec2f center = r.camera.center;

        // A left click selects, and the camera stays.
        const math::vec2f on_r1 = r.px(0.0f, 0.0f);
        CHECK(!r.controller.pointer_down(pointer(on_r1, mouse_button::left, mask_of(mouse_button::left))));
        (void)r.controller.pointer_up(pointer(on_r1, mouse_button::left, mouse_buttons::none));
        CHECK(selection_of(r.tools) == std::vector<item_ref>{part_ref(1)});
        CHECK(r.told > 0);
        CHECK(r.camera.center == center);

        // A middle drag pans; the selection stays, nothing is moved.
        const schematic::sheet before = r.doc.sheet();
        (void)r.controller.pointer_down(pointer(on_r1, mouse_button::middle, mask_of(mouse_button::middle)));
        CHECK(r.controller.panning());
        CHECK(r.controller.cursor() == cursor_shape::move);
        CHECK(r.controller.pointer_move(
            pointer(on_r1 + math::vec2f{50.0f, 0.0f}, mouse_button::middle, mask_of(mouse_button::middle), {50.0f, 0.0f})));
        (void)r.controller.pointer_up(pointer(on_r1, mouse_button::middle, mouse_buttons::none));
        CHECK(!r.controller.panning());
        CHECK(near(r.camera.center.x(), center.x() - 1.0f)); // 50 pixels, one grid square
        CHECK(r.doc.sheet() == before);
        CHECK(selection_of(r.tools) == std::vector<item_ref>{part_ref(1)});

        // A right press with nothing open is the camera's, and its click
        // may open the context menu.
        (void)r.controller.pointer_down(pointer(on_r1, mouse_button::right, mask_of(mouse_button::right)));
        CHECK(r.controller.panning());
        CHECK(r.controller.context_menu_allowed());
        (void)r.controller.pointer_up(pointer(on_r1, mouse_button::right, mouse_buttons::none));
        CHECK(!r.controller.panning());

        // A left drag on R1 moves it: the tools', the camera stays.
        const math::vec2f camera_now = r.camera.center;
        (void)r.controller.pointer_down(pointer(r.px(0.0f, 0.0f), mouse_button::left, mask_of(mouse_button::left)));
        (void)r.controller.pointer_move(pointer(r.px(0.0f, 100.0f), mouse_button::left, mask_of(mouse_button::left),
                                                {0.0f, -50.0f}));
        CHECK(r.controller.cursor() == cursor_shape::move);
        (void)r.controller.pointer_up(pointer(r.px(0.0f, 100.0f), mouse_button::left, mouse_buttons::none));
        CHECK(r.doc.find_part(1)->position == pt(0, 100));
        CHECK(r.camera.center == camera_now);
        CHECK(r.doc.undo_label() == "Move R1");

        // The wheel zooms about the pointer.
        const float zoom = r.camera.zoom;
        CHECK(r.controller.scroll({0.0f, 1.0f}, pointer(r.px(0.0f, 0.0f), mouse_button::left, mouse_buttons::none)));
        CHECK(r.camera.zoom > zoom);
    }

    void right_click_ending_a_wire_is_no_context_menu()
    {
        rig r;
        r.tools.set_tool(tools_t::tool::wire);
        CHECK(r.controller.cursor() == cursor_shape::crosshair);

        // From R1's pin 1 down two squares.
        r.click(r.px(-100.0f, 0.0f));
        CHECK(r.tools.drawing_wire());
        (void)r.controller.pointer_move(pointer(r.px(-100.0f, -200.0f), mouse_button::left, mouse_buttons::none));
        const std::size_t wires = r.doc.sheet().wires.size();

        // The right press ends it: the tools', not the camera's, and its
        // click opens no menu.
        (void)r.controller.pointer_down(pointer(r.px(-100.0f, -200.0f), mouse_button::right,
                                                mask_of(mouse_button::right)));
        CHECK(!r.tools.drawing_wire());
        CHECK(!r.controller.panning());
        CHECK(!r.controller.context_menu_allowed());
        (void)r.controller.pointer_up(pointer(r.px(-100.0f, -200.0f), mouse_button::right, mouse_buttons::none));
        // Nothing fixed by a click: the wire ends where it was, with no
        // segment (a right click keeps only what was fixed).
        CHECK(r.doc.sheet().wires.size() == wires);

        // Drawn with a corner click first, the right press keeps it.
        r.click(r.px(-100.0f, 0.0f));
        r.click(r.px(-100.0f, -200.0f));
        (void)r.controller.pointer_down(pointer(r.px(-300.0f, -200.0f), mouse_button::right,
                                                mask_of(mouse_button::right)));
        (void)r.controller.pointer_up(pointer(r.px(-300.0f, -200.0f), mouse_button::right, mouse_buttons::none));
        CHECK(r.doc.sheet().wires.size() == wires + 1);
        CHECK(r.doc.undo_label() == "Draw wire");
        CHECK(!r.controller.context_menu_allowed());

        // The next right press, with nothing open, is a pan and a menu again.
        (void)r.controller.pointer_down(pointer(r.px(0.0f, 0.0f), mouse_button::right, mask_of(mouse_button::right)));
        CHECK(r.controller.context_menu_allowed() && r.controller.panning());
        (void)r.controller.pointer_up(pointer(r.px(0.0f, 0.0f), mouse_button::right, mouse_buttons::none));
    }

    void keys_to_the_tools_then_the_camera()
    {
        rig r;
        r.tools.select(part_ref(1));

        // R turns the selection: used, so no shortcut runs as well.
        CHECK(r.controller.key(press_key(key::r), view_size));
        CHECK(r.doc.find_part(1)->rotation == 1);
        // Shift+R back.
        CHECK(r.controller.key(press_key(key::r, modifiers::shift), view_size));
        CHECK(r.doc.find_part(1)->rotation == 0);
        // The arrows nudge by the grid.
        CHECK(r.controller.key(press_key(key::right), view_size));
        CHECK(r.doc.find_part(1)->position == pt(100, 0));

        // W is the window's (a tool shortcut): not used here.
        CHECK(!r.controller.key(press_key(key::w), view_size));
        // Ctrl+Z is the window's too.
        CHECK(!r.controller.key(press_key(key::z, modifiers::control), view_size));

        // Home is the camera's: it fits.
        r.camera.fit_min = {-5.0f, -5.0f};
        r.camera.fit_max = {5.0f, 5.0f};
        r.camera.center = {30.0f, 30.0f};
        CHECK(r.controller.key(press_key(key::home), view_size));
        CHECK(near(r.camera.center.x(), 0.0f) && near(r.camera.center.y(), 0.0f));

        // Escape: the selection goes, then nothing is left to cancel.
        CHECK(r.controller.key(press_key(key::escape), view_size));
        CHECK(r.tools.selection().empty());
        CHECK(!r.controller.key(press_key(key::escape), view_size));
    }

    // ---- the controller in a real viewport ----------------------------------------------------

    void through_a_viewport()
    {
        harness h{{800, 600}};
        rig r;
        auto &root = h.screen->emplace_root<column>(0.0f);
        auto &view = root.emplace<schematic_viewport>(texture_id{}, r.controller);
        view.layout().flex = 1.0f;
        view.set_controller(&r.controller);
        int menus = 0;
        int refused = 0;
        view.context_requested.connect([&](math::vec2f) {
            if (r.controller.context_menu_allowed())
                ++menus;
            else
                ++refused;
        });
        h.frame();
        const math::vec2i expected_size{800, 600};
        CHECK(view.pixel_size() == expected_size);

        const auto at = [&](float x, float y) {
            const math::vec2f p = r.px(x, y);
            return math::vec2i{static_cast<int>(std::lround(p.x())), static_cast<int>(std::lround(p.y()))};
        };

        // A plain right click: the menu.
        h.click(at(0, 0).x(), at(0, 0).y(), mouse_button::right);
        CHECK(menus == 1 && refused == 0);

        // A right drag pans, and is no click.
        const math::vec2f center = r.camera.center;
        h.drag(400, 300, 500, 300, mouse_button::right);
        CHECK(menus == 1);
        CHECK(r.camera.center.x() < center.x());

        // A left click on R1 selects it; the view takes the focus.
        h.click(at(0, 0).x(), at(0, 0).y());
        CHECK(selection_of(r.tools) == std::vector<item_ref>{part_ref(1)});
        CHECK(view.has_focus());
        CHECK(view.cursor({}) == cursor_shape::move); // over what a drag would move

        // Keys while it has the focus: X mirrors R1.
        h.key(key::x);
        CHECK(r.doc.find_part(1)->mirrored);

        // A wire from R1's pin 1 (where the mirror put it), one corner
        // clicked, then a right click ends it and opens no menu.
        r.tools.set_tool(tools_t::tool::wire);
        const std::size_t wires = r.doc.sheet().wires.size();
        const schematic::point pin1 = schematic::placed_pins(*r.doc.find_part(1)).front().position;
        h.click(at(static_cast<float>(pin1.x()), static_cast<float>(pin1.y())).x(),
                at(static_cast<float>(pin1.x()), static_cast<float>(pin1.y())).y());
        CHECK(r.tools.drawing_wire());
        CHECK(view.cursor({}) == cursor_shape::crosshair);
        h.click(at(static_cast<float>(pin1.x()), -300.0f).x(), at(static_cast<float>(pin1.x()), -300.0f).y());
        h.click(at(-500.0f, -300.0f).x(), at(-500.0f, -300.0f).y(), mouse_button::right);
        CHECK(!r.tools.drawing_wire());
        CHECK(r.doc.sheet().wires.size() == wires + 1);
        CHECK(menus == 1 && refused == 1);

        // And the next one is a menu again.
        h.click(at(-500.0f, -300.0f).x(), at(-500.0f, -300.0f).y(), mouse_button::right);
        CHECK(menus == 2);
    }

    // ---- the properties page --------------------------------------------------------------------

    struct page_rig
    {
        harness h{{420, 900}};
        schematic::document doc{small_sheet()};
        tools_t tools;
        std::optional<schematic::netlist> nets;
        std::uint64_t nets_revision = ~std::uint64_t{0};
        int edits = 0;
        int assigns = 0;
        selection_properties page{tools, selection_properties::hooks{
                                             .netlist = [this]() -> const schematic::netlist * {
                                                 if (!nets || nets_revision != doc.revision())
                                                 {
                                                     nets = schematic::build_netlist(doc.sheet());
                                                     nets_revision = doc.revision();
                                                 }
                                                 return &*nets;
                                             },
                                             .reveal = {},
                                             .can_reveal = {},
                                             .edited = [this] { ++edits; },
                                             .return_focus = {},
                                             .renamed = {},
                                             .assign = [this] { ++assigns; },
                                             .part_name = {},
                                         }};
        column *root = nullptr;

        page_rig()
        {
            tools.set_document(&doc);
            root = &h.screen->emplace_root<column>(0.0f);
            root->add(page.build());
            h.frame();
        }
        page_rig(const page_rig &) = delete;
        page_rig &operator=(const page_rig &) = delete;

        // As the editor does between frames.
        void frame()
        {
            page.update();
            h.frame();
        }

        template <typename W>
        W *find(std::string_view name)
        {
            return root->find_as<W>(name);
        }

        std::string text_of(std::string_view name)
        {
            const label *l = find<label>(name);
            return l != nullptr ? l->text() : std::string("<none>");
        }

        std::size_t steps() const { return doc.history().position(); }
    };

    void part_properties()
    {
        page_rig r;
        r.frame();
        CHECK(r.find<text_field>("property value") == nullptr); // nothing selected, nothing shown

        r.tools.select(part_ref(1));
        r.frame();
        text_field *reference = r.find<text_field>("property reference");
        text_field *value = r.find<text_field>("property value");
        CHECK(reference != nullptr && value != nullptr);
        if (reference == nullptr || value == nullptr)
            return;
        CHECK(reference->text() == "R1" && value->text().empty());
        CHECK(r.text_of("property symbol") == "R");
        CHECK(r.text_of("property device") == "resistor");
        // No library part: a placeholder, with Assign Part… (not Show in
        // Library).
        CHECK(r.text_of("property library part") == "Placeholder \xE2\x80\x94 no part assigned");
        button *assign = r.find<button>("property assign part");
        button *reveal = r.find<button>("property show in library");
        CHECK(assign != nullptr && reveal != nullptr);
        if (assign != nullptr && reveal != nullptr)
        {
            CHECK(assign->visible() && assign->text() == "Assign Part\xE2\x80\xA6");
            CHECK(!reveal->visible());
            r.h.click_on(*assign);
            CHECK(r.assigns == 1);
        }
        // Pin 1 touches nothing; pin 2 is on the wire the label names.
        CHECK(r.text_of("property pin 1") == "Not connected");
        CHECK(r.text_of("property pin 2") == "MID");
        CHECK(r.page.shown().size() == 1);

        // Typing a value: every key an edit, all of it one step.
        CHECK(r.page.focus_text(part_ref(1)));
        CHECK(value->has_focus());
        r.h.type("47k");
        CHECK(r.doc.find_part(1)->value == "47k");
        CHECK(r.steps() == 1);
        CHECK(r.doc.undo_label() == "Edit value");
        CHECK(r.edits == 3);

        // The document changed under the page: the field is the same one,
        // its caret where the typing left it.
        r.frame();
        CHECK(r.find<text_field>("property value") == value);
        CHECK(value->caret() == 3 && value->has_focus());

        // Another field: the run of typing ended; the reference is its own
        // step.
        reference->focus();
        reference->select(2, 2);
        r.h.type("0");
        CHECK(r.doc.find_part(1)->reference == "R10");
        CHECK(r.steps() == 2);

        // Back to the value: a new run, a new step.
        value->focus();
        r.h.type("0");
        CHECK(r.doc.find_part(1)->value == "47k0");
        CHECK(r.steps() == 3);

        // Undone, the fields show the item again.
        CHECK(r.doc.undo() && r.doc.undo());
        r.frame();
        CHECK(value->text() == "47k" && reference->text() == "R1");
        CHECK(r.doc.undo());
        r.frame();
        CHECK(value->text().empty());

        // A position typed in moves the part by the difference, the wire
        // on its pin 2 following it as in a drag: one step.
        numeric_field *x = r.find<numeric_field>("property x");
        CHECK(x != nullptr);
        if (x != nullptr)
        {
            const std::size_t before = r.steps();
            x->focus();
            x->select_all();
            r.h.type("-3");
            r.h.key(key::enter);
            CHECK(r.doc.find_part(1)->position == pt(-300, 0));
            CHECK(r.doc.undo_label() == "Move R1");
            CHECK(r.steps() == before + 1);
            const schematic::point pin2 = schematic::placed_pins(*r.doc.find_part(1)).back().position;
            bool joined = false;
            for (const auto &e : r.doc.sheet().wires)
                joined = joined || e.record.a == pin2 || e.record.b == pin2;
            CHECK(joined);
            r.frame();
            CHECK(r.text_of("property pin 2") == "MID");
            CHECK(r.doc.undo());
            CHECK(r.doc.find_part(1)->position == pt(0, 0));
            CHECK(r.doc.find_wire(3) != nullptr && r.doc.find_wire(3)->a == pt(100, 0));
            CHECK(r.doc.redo());
            r.frame();
        }

        // Moved under the page (a drag), the fields follow, but not one
        // being typed in.
        reference->focus();
        const std::array<item_ref, 1> r1{part_ref(1)};
        CHECK(r.doc.move(r1, {0, 200}));
        r.frame();
        numeric_field *y = r.find<numeric_field>("property y");
        CHECK(y != nullptr && near(static_cast<float>(y->value()), 2.0f));
    }

    void other_selections()
    {
        page_rig r;

        // A wire: its ends, length and net.
        r.tools.select(wire_ref(3));
        r.frame();
        CHECK(r.text_of("property from") == "1, 0");
        CHECK(r.text_of("property to") == "5, 0");
        CHECK(r.text_of("property length") == "4");
        CHECK(r.text_of("property net") == "MID");
        CHECK(r.find<text_field>("property value") == nullptr);

        // A label: its text, typed into as one step.
        r.tools.select(label_ref(4));
        r.frame();
        text_field *text = r.find<text_field>("property text");
        CHECK(text != nullptr && text->text() == "MID");
        CHECK(r.text_of("property kind") == "Net label");
        CHECK(r.page.focus_text(label_ref(4)));
        r.h.type("DLE");
        CHECK(r.doc.find_label(4)->text == "DLE");
        CHECK(r.steps() == 1 && r.doc.undo_label() == "Edit label");
        r.frame();
        CHECK(r.text_of("property net") == "DLE");

        // Several: how many of each.
        const std::array<item_ref, 3> several{part_ref(1), part_ref(2), wire_ref(3)};
        r.tools.select(several);
        r.frame();
        CHECK(r.text_of("property summary") == "2 parts, 1 wire");
        CHECK(!r.page.focus_text(part_ref(1)));

        // The actions act on the selection, and say so.
        auto *rotate = r.find<button>("selection rotate");
        CHECK(rotate != nullptr);
        if (rotate != nullptr)
        {
            r.h.click_on(*rotate);
            CHECK(r.doc.undo_label() == "Rotate");
            CHECK(r.edits > 0);
        }
        auto *remove = r.find<button>("selection delete");
        CHECK(remove != nullptr);
        if (remove != nullptr)
        {
            r.h.click_on(*remove);
            CHECK(r.doc.find_part(1) == nullptr && r.doc.find_wire(3) == nullptr);
        }
        // The page follows the selection, emptied by the delete.
        r.frame();
        CHECK(r.page.shown().empty());
        CHECK(r.find<button>("selection rotate") == nullptr);
    }

    // ---- the keyboard between the page and the canvas ------------------------------------------

    // The canvas and the Properties page side by side on one screen, wired
    // as the editor wires them: a label placed hands its Text field the
    // keyboard; Enter or Escape there hands it back. The window's one-key
    // shortcuts are bound to counters, Escape to the tools' cancel.
    struct focus_rig
    {
        harness h{{1200, 700}};
        schematic::document doc{small_sheet()};
        tools_t tools;
        pan_zoom_controller camera;
        schematic_view_controller controller{tools, camera};
        int returned = 0;
        int shortcuts = 0;
        schematic_viewport *view = nullptr;
        selection_properties page{tools, selection_properties::hooks{
                                             .netlist = {},
                                             .reveal = {},
                                             .can_reveal = {},
                                             .edited = {},
                                             .return_focus =
                                                 [this] {
                                                     ++returned;
                                                     view->focus();
                                                 },
                                             .renamed =
                                                 [this](schematic::label_kind kind, const std::string &text) {
                                                     if (tools.active_tool() == tools_t::tool::label)
                                                         tools.label_renamed(kind, text);
                                                 },
                                             .assign = {},
                                             .part_name = {},
                                         }};
        tool_options options{tools, tool_options::hooks{
                                        .edited = {},
                                        .return_focus =
                                            [this] {
                                                ++returned;
                                                view->focus();
                                            },
                                        .choose_part = {},
                                    }};
        column *side = nullptr;

        focus_rig()
        {
            tools.set_document(&doc);
            camera.zoom = 50.0f;
            tools.placed = [this](item_ref item) {
                const schematic::label *l = doc.find_label(item.id);
                if (l != nullptr && l->kind != schematic::label_kind::power)
                    (void)page.focus_text(item);
            };
            auto &root = h.screen->emplace_root<row>(0.0f);
            auto &v = root.emplace<schematic_viewport>(texture_id{}, controller);
            v.layout().flex = 1.0f;
            v.set_controller(&controller);
            view = &v;
            auto &s = root.emplace<column>(0.0f);
            s.layout().width = 400.0f;
            s.add(options.build());
            s.add(page.build());
            side = &s;

            ui::screen &screen = *h.screen;
            for (const char *k : {"R", "X", "Y", "W", "L", "P", "A", "T", "Space", "Delete", "Backspace", "Left",
                                  "Right", "Up", "Down", "Enter"})
                (void)screen.shortcuts().bind(k, "test.shortcut");
            screen.add_command("test.shortcut", [this] { ++shortcuts; });
            (void)screen.shortcuts().bind("Escape", "test.cancel");
            screen.add_command("test.cancel", [this] { (void)tools.cancel(); });
            frame();
        }
        focus_rig(const focus_rig &) = delete;
        focus_rig &operator=(const focus_rig &) = delete;

        void frame()
        {
            options.update();
            page.update();
            h.frame();
        }

        // The view's pixel of a sheet point, in the window.
        math::vec2i at(float x, float y) const
        {
            const math::vec2f p = view->bounds().position() +
                                  camera.to_pixels(math::vec2f{x, y} / 100.0f, view->pixel_size().cast<float>());
            return {static_cast<int>(std::lround(p.x())), static_cast<int>(std::lround(p.y()))};
        }

        // A key as a keyboard sends it: the press, then its character.
        void press(key k, std::string_view character = {})
        {
            h.key(k);
            if (!character.empty())
                h.type(character);
        }

        template <typename W>
        W *find(std::string_view name)
        {
            return side->find_as<W>(name);
        }
    };

    void typing_a_label_and_back_to_the_canvas()
    {
        focus_rig r;
        r.tools.set_tool(tools_t::tool::label);
        r.frame();

        // A label placed: its Text field has the keyboard, all selected.
        const math::vec2i first = r.at(-500.0f, -300.0f);
        r.h.click(first.x(), first.y());
        r.frame();
        CHECK(r.tools.selection().size() == 1);
        if (r.tools.selection().size() != 1)
            return;
        const item_ref placed = r.tools.selection()[0];
        CHECK(placed.kind == item_kind::label);
        text_field *text = r.find<text_field>("property text");
        CHECK(text != nullptr && text->has_focus() && text->has_selection());
        if (text == nullptr)
            return;
        const std::string first_text = r.doc.find_label(placed.id)->text;
        CHECK(first_text == "NET1");

        // Plain keys typed are the field's: none reaches the tools or the
        // window's shortcuts.
        const schematic::sheet parts_before = r.doc.sheet();
        r.press(key::r, "r");
        r.press(key::x, "x");
        r.press(key::y, "y");
        r.press(key::w, "w");
        r.press(key::l, "l");
        r.press(key::p, "p");
        r.press(key::a, "a");
        r.press(key::t, "t");
        r.press(key::space, " ");
        r.press(key::left);
        r.press(key::right);
        r.press(key::up);
        r.press(key::down);
        r.press(key::del);
        r.press(key::backspace);
        CHECK(r.shortcuts == 0);
        CHECK(text->has_focus());
        CHECK(r.doc.find_label(placed.id)->text == "rxywlpat");
        CHECK(r.doc.find_part(1)->rotation == 0 && !r.doc.find_part(1)->mirrored);
        CHECK(r.doc.find_part(1)->position == parts_before.parts.find(1)->position);
        CHECK(r.tools.active_tool() == tools_t::tool::label);

        // Named "D0": the next label will be "D1". Enter keeps it, and
        // the canvas has the keyboard back, the tool armed, the label
        // still selected.
        text->select_all();
        r.press(key::d, "D");
        r.press(key::digit_0, "0");
        CHECK(r.doc.find_label(placed.id)->text == "D0");
        CHECK(r.tools.label_text(schematic::label_kind::net) == "D1");
        r.press(key::enter);
        CHECK(r.returned == 1);
        CHECK(r.view->has_focus());
        CHECK(r.tools.active_tool() == tools_t::tool::label);
        CHECK(r.tools.selection().size() == 1 && r.tools.selection()[0] == placed);
        CHECK(r.shortcuts == 0);
        // The typing is one step after the placing.
        CHECK(r.doc.undo_label() == "Edit label");

        // The next click places "D1", and the field has the keyboard again.
        r.frame();
        const math::vec2i second = r.at(-500.0f, -600.0f);
        r.h.click(second.x(), second.y());
        r.frame();
        CHECK(r.tools.selection().size() == 1);
        if (r.tools.selection().size() != 1)
            return;
        const item_ref next = r.tools.selection()[0];
        CHECK(next != placed && r.doc.find_label(next.id) != nullptr);
        CHECK(r.doc.find_label(next.id)->text == "D1");
        text = r.find<text_field>("property text");
        CHECK(text != nullptr && text->has_focus());
        if (text == nullptr)
            return;

        // Escape: what was typed stays, the keyboard goes back, and nothing
        // is cancelled (the tool and the selection stay).
        r.press(key::c, "C");
        r.press(key::l, "L");
        r.press(key::k, "K");
        r.press(key::escape);
        CHECK(r.doc.find_label(next.id)->text == "CLK");
        CHECK(r.returned == 2);
        CHECK(r.view->has_focus());
        CHECK(r.tools.active_tool() == tools_t::tool::label);
        CHECK(r.tools.selection().size() == 1);

        // A second Escape, on the canvas: one level, the tool goes.
        r.press(key::escape);
        CHECK(r.tools.active_tool() == tools_t::tool::select);
        CHECK(r.tools.selection().size() == 1);

        // A field clicked into keeps Enter (it is not handed back), but
        // Escape still leaves it without cancelling anything.
        r.frame();
        text = r.find<text_field>("property text");
        CHECK(text != nullptr);
        if (text == nullptr)
            return;
        r.h.click_on(*text);
        CHECK(text->has_focus());
        r.press(key::enter);
        CHECK(text->has_focus() && r.returned == 2);
        r.press(key::escape);
        CHECK(r.view->has_focus() && r.returned == 3);
        CHECK(r.tools.selection().size() == 1);
    }

    void tool_section()
    {
        focus_rig r;

        // Select and Wire: nothing.
        r.frame();
        CHECK(r.find<text_field>("tool text") == nullptr);
        r.tools.set_tool(tools_t::tool::wire);
        r.frame();
        CHECK(r.find<text_field>("tool text") == nullptr);

        // The power port: its name, and the presets.
        r.tools.begin_place_power("GND");
        r.frame();
        text_field *name = r.find<text_field>("tool text");
        dropdown *presets = r.find<dropdown>("tool presets");
        CHECK(name != nullptr && presets != nullptr);
        if (name == nullptr || presets == nullptr)
            return;
        CHECK(name->text() == "GND");
        CHECK(presets->items() == power_port_names(r.doc.sheet()));
        CHECK(presets->selected() == 0);

        // Another typed: armed as it is typed; Enter gives the keyboard back.
        CHECK(r.options.focus_field());
        CHECK(name->has_focus());
        r.h.type("VBAT");
        CHECK(r.tools.label_text(schematic::label_kind::power) == "VBAT");
        CHECK(r.tools.active_tool() == tools_t::tool::power);
        r.press(key::enter);
        CHECK(r.returned == 1 && r.view->has_focus());
        r.frame();
        CHECK(presets->selected() < 0);

        // Placed, it joins the presets.
        const math::vec2i at = r.at(-800.0f, 400.0f);
        r.h.click(at.x(), at.y());
        r.frame();
        CHECK(r.doc.sheet().labels.size() == 2);
        CHECK(presets->items().back() == "VBAT");
        CHECK(presets->selected() == static_cast<int>(presets->items().size()) - 1);

        // Emptied and left: the name armed stays.
        CHECK(r.options.focus_field());
        r.press(key::backspace);
        CHECK(name->text().empty());
        r.press(key::escape);
        CHECK(name->text() == "VBAT" && r.tools.label_text(schematic::label_kind::power) == "VBAT");
        CHECK(r.tools.active_tool() == tools_t::tool::power);

        // The net label's next text.
        r.tools.set_tool(tools_t::tool::label);
        r.frame();
        text_field *next = r.find<text_field>("tool text");
        CHECK(next != nullptr && next->text() == "NET1");
        CHECK(r.find<dropdown>("tool presets") == nullptr);

        // A part: what is armed, its value typed for the next ones.
        schematic::part p;
        p.symbol = "R";
        p.device = "resistor";
        p.value = "10k";
        r.tools.begin_place_part(p);
        r.frame();
        text_field *value = r.find<text_field>("tool value");
        CHECK(value != nullptr && value->text() == "10k");
        const label *library = r.find<label>("tool library part");
        CHECK(library != nullptr && library->text().starts_with("Placeholder"));
        if (value != nullptr)
        {
            CHECK(r.options.focus_field());
            r.h.type("4k7");
            CHECK(r.tools.pending_part() && r.tools.pending_part()->value == "4k7");
            CHECK(r.find<text_field>("tool value") == value); // not made again while typed in
            r.press(key::escape);
            CHECK(r.view->has_focus());
            const math::vec2i where = r.at(-800.0f, -500.0f);
            r.h.click(where.x(), where.y());
            const schematic::part *made = r.tools.selection().size() == 1 && r.tools.selection()[0].kind ==
                                                                                 item_kind::part
                                              ? r.doc.find_part(r.tools.selection()[0].id)
                                              : nullptr;
            CHECK(made != nullptr && made->value == "4k7");
        }
    }

    void helpers()
    {
        CHECK(grid_units(0) == "0");
        CHECK(grid_units(300) == "3");
        CHECK(grid_units(-250) == "-2.5");
        CHECK(grid_units(25) == "0.25");
        CHECK(grid_units(-1) == "-0.01");
        CHECK(property_merge_key("value", 12) == "type:value:12");
        CHECK(property_merge_key("value", 12) != property_merge_key("value", 13));
        CHECK(property_merge_key("value", 12) != property_merge_key("reference", 12));
        CHECK(rotation_text(0) == "0\xC2\xB0" && rotation_text(3) == "270\xC2\xB0");

        const schematic::sheet s = small_sheet();
        const std::array<item_ref, 4> items{part_ref(1), wire_ref(3), label_ref(4), part_ref(99)};
        CHECK(selection_summary(s, items) == "1 part, 1 wire, 1 label");
        CHECK(selection_summary(s, {}) == "Nothing");

        const schematic::netlist nets = schematic::build_netlist(s);
        const std::vector<pin_net> pins = pin_nets(*s.parts.find(1), 1, nets);
        CHECK(pins.size() == 2);
        if (pins.size() == 2)
        {
            CHECK(pin_net_text(pins[0]) == "1: not connected");
            CHECK(pin_net_text(pins[1]) == "2: MID");
        }
        CHECK(pin_net_text({"1", "+", "VCC"}) == "1 (+): VCC");

        // The power pick: the usual ports, then the sheet's others, sorted,
        // each once.
        schematic::sheet powered = small_sheet();
        for (const char *name : {"VBAT", "GND", "VBAT", "AUX", ""})
        {
            schematic::label l;
            l.kind = schematic::label_kind::power;
            l.text = name;
            (void)powered.labels.insert(static_cast<item_id>(100 + powered.labels.size()), l);
        }
        CHECK(power_port_names(powered) ==
              std::vector<std::string>{"GND", "+3V3", "+5V", "+12V", "-12V", "VCC", "AUX", "VBAT"});
        CHECK(power_port_names(schematic::sheet{}).size() == standard_power_ports().size());

        // R1 and C1 have no library part: both placeholders; one assigned
        // is not, nor is a source.
        CHECK(unassigned_parts(s) == std::vector<item_ref>{part_ref(1), part_ref(2)});
        schematic::sheet assigned_one = small_sheet();
        schematic::part r1 = *assigned_one.parts.find(1);
        r1.library_part = "r_10k";
        (void)assigned_one.parts.replace(1, r1);
        schematic::part v;
        v.symbol = "V_DC";
        v.device = "voltage_source";
        (void)assigned_one.parts.insert(9, v);
        CHECK(unassigned_parts(assigned_one) == std::vector<item_ref>{part_ref(2)});
    }
} // namespace

int main()
{
    sheet_units_and_tolerance();
    left_to_the_tools_middle_and_right_to_the_camera();
    right_click_ending_a_wire_is_no_context_menu();
    keys_to_the_tools_then_the_camera();
    through_a_viewport();
    part_properties();
    other_selections();
    typing_a_label_and_back_to_the_canvas();
    tool_section();
    helpers();
    return volt::tests::result("app schematic editor");
}
