#include "harness.hpp"

#include <ui/layout/box.hpp>
#include <ui/widgets/button.hpp>
#include <ui/widgets/dropdown.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/menu.hpp>
#include <ui/widgets/panel.hpp>
#include <ui/widgets/text_field.hpp>

#include <string>
#include <vector>

// The overlay: dropdowns, menus and their submenus, context menus, modal
// dialogs, tooltips; opening, choosing, and every way of dismissing them.

using namespace volt::tests::ui;
using volt::math::vec2f;

namespace
{
    void dropdown_open_choose_dismiss()
    {
        harness h({400, 300});
        // Side by side: the list opens below the dropdown, not over the button.
        auto &root = h.screen->emplace_root<row>(20.0f, insets::uniform(10.0f));
        root.set_cross_align(align::start);
        auto &d = root.emplace<dropdown>(std::vector<std::string>{"Isometric", "Top", "Front"}, 0);
        auto &after = root.emplace<button>("after");
        h.screen->layout();

        int changes = 0;
        int chosen = -1;
        d.changed.connect([&](int i) {
            ++changes;
            chosen = i;
        });

        // Click: the list opens below, at least as wide.
        h.click_on(d);
        CHECK(d.is_open());
        CHECK(h.screen->popup_count() == 1);
        h.frame();
        menu *list = d.list();
        CHECK(list != nullptr);
        CHECK(list->bounds().y >= d.bounds().bottom());
        CHECK(list->bounds().width >= d.bounds().width);
        CHECK(list->hot() == 0);                   // the current choice highlighted
        CHECK(h.screen->focused() == list);        // and the keyboard with it

        // Click an item: chosen, closed, focus back.
        const rect item = list->item_rect(2);
        h.click(static_cast<int>(item.center().x()), static_cast<int>(item.center().y()));
        CHECK(!d.is_open());
        CHECK(h.screen->popup_count() == 0);
        CHECK(d.selected() == 2);
        CHECK(chosen == 2 && changes == 1);
        CHECK(h.screen->focused() == &d);

        // Escape closes without a change.
        h.click_on(d);
        CHECK(d.is_open());
        h.key(key::escape);
        CHECK(!d.is_open());
        CHECK(changes == 1);

        // A click outside closes it, and does nothing else: the button it
        // landed on is not clicked.
        int clicked = 0;
        after.clicked.connect([&] { ++clicked; });
        h.click_on(d);
        h.frame();
        CHECK(d.is_open());
        h.click(390, 290); // empty space
        CHECK(!d.is_open());
        h.click_on(d);
        h.frame();
        h.click_on(after);
        CHECK(!d.is_open());
        CHECK(clicked == 0);
        h.click_on(after);
        CHECK(clicked == 1);

        // Clicking the dropdown again closes it rather than reopening.
        h.click_on(d);
        CHECK(d.is_open());
        h.click_on(d);
        CHECK(!d.is_open());

        // The keyboard: Up/Down choose while closed, Enter opens, arrows
        // and Enter choose in the list.
        d.focus();
        h.key(key::up);
        CHECK(d.selected() == 1);
        h.key(key::enter);
        CHECK(d.is_open());
        h.key(key::up);
        h.key(key::enter);
        CHECK(!d.is_open());
        CHECK(d.selected() == 0);
    }

    void menus()
    {
        harness h({600, 400});
        auto &root = h.screen->emplace_root<column>();
        root.set_cross_align(align::start);
        auto &bar = root.emplace<menu_bar>();

        int saved = 0;
        int exited = 0;
        bool dark = true;
        h.screen->add_command("file.save", [&] { ++saved; });
        h.screen->add_command("file.exit", [&] { ++exited; });
        CHECK(h.screen->shortcuts().bind("Ctrl+S", "file.save").has_value());

        menu_item save;
        save.text = "Save";
        save.command = "file.save";
        menu_item exit;
        exit.text = "Exit";
        exit.command = "file.exit";
        menu_item disabled;
        disabled.text = "Print";
        disabled.action = [&] { ++saved; };
        disabled.enabled = [] { return false; };

        menu_item light;
        light.text = "Light";
        light.action = [&] { dark = false; };
        light.checked = [&] { return !dark; };
        menu_item theme_item;
        theme_item.text = "Theme";
        theme_item.submenu = {light};

        bar.add_menu("File", {save, disabled, menu_item::separator(), exit});
        bar.add_menu("View", {theme_item});
        h.screen->layout();

        // Click the title: its menu opens under it.
        const rect file = bar.title_rect(0);
        h.click(static_cast<int>(file.center().x()), static_cast<int>(file.center().y()));
        CHECK(bar.open_index() == 0);
        menu *m = bar.open_menu_widget();
        CHECK(m != nullptr);
        h.frame();
        CHECK(m->bounds().y >= file.bottom());

        // The shortcut shows; measured wide enough for it.
        CHECK(h.screen->shortcut_text("file.save") == "Ctrl+S");
        CHECK(m->bounds().width > h.text.measure("Save", h.screen->theme().body_style()) +
                                      h.text.measure("Ctrl+S", h.screen->theme().body_style()));

        // A disabled item does nothing and keeps the menu open.
        const rect print = m->item_rect(1);
        h.click(static_cast<int>(print.center().x()), static_cast<int>(print.center().y()));
        CHECK(saved == 0);
        CHECK(bar.open_index() == 0);

        // Keyboard: Down skips the disabled item and the separator.
        h.key(key::down);
        CHECK(m->hot() == 0);
        h.key(key::down);
        CHECK(m->hot() == 3);
        h.key(key::enter);
        CHECK(exited == 1);
        CHECK(h.screen->popup_count() == 0);
        CHECK(bar.open_index() == -1);

        // Open File, move over View: it switches.
        h.click(static_cast<int>(file.center().x()), static_cast<int>(file.center().y()));
        const rect view = bar.title_rect(1);
        h.move(static_cast<int>(view.center().x()), static_cast<int>(view.center().y()));
        CHECK(bar.open_index() == 1);
        m = bar.open_menu_widget();
        h.frame();

        // Hover the submenu's item: the submenu opens to the right.
        const rect theme_row = m->item_rect(0);
        h.move(static_cast<int>(theme_row.center().x()), static_cast<int>(theme_row.center().y()));
        CHECK(m->submenu() != nullptr);
        CHECK(h.screen->popup_count() == 2);
        h.frame();
        menu *sub = m->submenu();
        CHECK(sub->bounds().x >= m->bounds().right() - 4.0f);

        // Click in the submenu: runs, and the whole chain closes.
        const rect light_row = sub->item_rect(0);
        h.click(static_cast<int>(light_row.center().x()), static_cast<int>(light_row.center().y()));
        CHECK(!dark);
        CHECK(h.screen->popup_count() == 0);

        // Keyboard into a submenu and back out.
        bar.open(1, true);
        m = bar.open_menu_widget();
        h.frame();
        h.key(key::right);
        CHECK(m->submenu() != nullptr);
        CHECK(h.screen->focused() == m->submenu());
        h.key(key::left);
        CHECK(m->submenu() == nullptr);
        CHECK(h.screen->focused() == m);
        // Right on an item without a submenu, at the top level, goes to
        // the next menu; Left from the first wraps round to the last.
        h.key(key::escape);
        bar.open(0, true);
        h.frame();
        h.key(key::right);
        CHECK(bar.open_index() == 1);
        bar.open(0, true);
        h.key(key::left);
        CHECK(bar.open_index() == 1);
        h.key(key::escape);
        CHECK(h.screen->popup_count() == 0);

        // Ctrl+S runs the command with no menu open.
        h.key(key::s, modifiers::control);
        CHECK(saved == 1);
    }

    void context_menu()
    {
        harness h({400, 300});
        auto &root = h.screen->emplace_root<column>();
        (void)root;
        h.screen->layout();

        int ran = 0;
        menu_item item;
        item.text = "Fit";
        item.action = [&] { ++ran; };
        menu &m = open_context_menu(*h.screen, {item}, {380.0f, 290.0f});
        h.frame();
        // Kept on screen: flipped up and pulled left.
        CHECK(m.bounds().right() <= 400.0f);
        CHECK(m.bounds().bottom() <= 300.0f);
        const rect r = m.item_rect(0);
        h.click(static_cast<int>(r.center().x()), static_cast<int>(r.center().y()));
        CHECK(ran == 1);
        CHECK(h.screen->popup_count() == 0);
    }

    void modal_dialog()
    {
        harness h({400, 300});
        auto &root = h.screen->emplace_root<column>(0.0f, insets::uniform(10.0f));
        root.set_cross_align(align::start);
        auto &behind = root.emplace<button>("behind");
        auto &field = root.emplace<text_field>("text");
        h.screen->layout();

        int behind_clicks = 0;
        behind.clicked.connect([&] { ++behind_clicks; });
        field.focus();

        auto dialog = std::make_unique<panel>("", surface::raised, 8.0f, insets::uniform(12.0f));
        dialog->layout().width = 200.0f;
        auto &ok = dialog->emplace<button>("OK");
        auto &cancel = dialog->emplace<button>("Cancel");
        popup_options options;
        options.placement = placement::centered;
        options.modal = true;
        widget &shown = h.screen->open_popup(std::move(dialog), std::move(options));
        h.frame();

        // Centred; the focus moved into it.
        CHECK(near(shown.bounds().center().x(), 200.0f, 1.0f));
        CHECK(h.screen->focused() == &ok);

        // Nothing behind it can be reached: not by the pointer, not by Tab,
        // not by a click outside (which does not close it either).
        h.click_on(behind);
        CHECK(behind_clicks == 0);
        CHECK(h.screen->popup_count() == 1);
        h.key(key::tab);
        CHECK(h.screen->focused() == &cancel);
        h.key(key::tab);
        CHECK(h.screen->focused() == &ok);

        // The backdrop is painted: a full-screen rectangle before it.
        bool backdrop = false;
        for (const draw_item &item : h.list.items())
        {
            const bool whole = item.bounds == (rect{0.0f, 0.0f, 400.0f, 300.0f});
            const bool dimmed = item.color == h.screen->theme().backdrop;
            backdrop = backdrop || (item.kind == shape_kind::rectangle && whole && dimmed);
        }
        CHECK(backdrop);

        // Escape closes it, and the focus goes back where it was.
        h.key(key::escape);
        CHECK(h.screen->popup_count() == 0);
        CHECK(h.screen->focused() == &field);
        h.click_on(behind);
        CHECK(behind_clicks == 1);
    }

    void tooltips()
    {
        harness h({400, 300});
        auto &root = h.screen->emplace_root<column>(0.0f, insets::uniform(10.0f));
        root.set_cross_align(align::start);
        auto &tip = root.emplace<button>("tip");
        tip.set_tooltip("Does a thing");
        h.screen->layout();

        const auto c = h.center_of(tip);
        h.move(c.x(), c.y());
        CHECK(h.screen->popup_count() == 0);
        // The delay is a timer: the screen knows when to wake for it.
        CHECK(h.screen->next_wakeup().has_value());
        h.wait(std::chrono::milliseconds(700));
        CHECK(h.screen->popup_count() == 1);
        // It lets the pointer through and goes on a press.
        CHECK(h.screen->hit_test(h.screen->popup(0)->bounds().center()) != h.screen->popup(0));
        h.press();
        CHECK(h.screen->popup_count() == 0);
        h.release();
    }

    // A disabled control explains itself all the same, which is where an
    // explanation is wanted most; it still takes no input.
    void tooltips_on_disabled()
    {
        harness h({400, 300});
        auto &root = h.screen->emplace_root<column>(8.0f, insets::uniform(10.0f));
        root.set_cross_align(align::start);
        auto &off = root.emplace<button>("off");
        off.set_tooltip("Nothing to undo");
        off.set_enabled(false);
        int clicked = 0;
        off.clicked.connect([&] { ++clicked; });

        // Inside a disabled group, the explanation of the innermost one.
        auto &group = root.emplace<column>(0.0f);
        group.set_tooltip("The group");
        auto &inner = group.emplace<button>("inner");
        inner.set_tooltip("The inner one");
        group.set_enabled(false);
        h.screen->layout();

        const auto c = h.center_of(off);
        h.move(c.x(), c.y());
        h.wait(std::chrono::milliseconds(700));
        CHECK(h.screen->popup_count() == 1);
        h.click_on(off);
        CHECK(clicked == 0);
        CHECK(h.screen->popup_count() == 0);

        const auto g = h.center_of(inner);
        h.move(g.x(), g.y());
        h.wait(std::chrono::milliseconds(700));
        CHECK(h.screen->popup_count() == 1);
    }
} // namespace

int main()
{
    dropdown_open_choose_dismiss();
    menus();
    context_menu();
    modal_dialog();
    tooltips();
    tooltips_on_disabled();
    return volt::tests::result("ui popups");
}
