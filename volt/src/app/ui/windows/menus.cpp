#include <app/ui/windows/menus.hpp>

#include <app/ui/windows/about.hpp>
#include <app/ui/windows/preferences.hpp>
#include <ui/widgets/image_view.hpp>
#include <ui/widgets/label.hpp>
#include <ui/widgets/panel.hpp>

#include <algorithm>

namespace volt::ui
{
    namespace
    {
        // The logo's size in a toolbar, logical units.
        constexpr float toolbar_logo = 22.0f;
    } // namespace

    std::string window_command(window_kind kind)
    {
        return "window." + std::string(window_key(kind));
    }

    menu_item command_item(std::string text, std::string command, ui::icon icon)
    {
        menu_item item;
        item.text = std::move(text);
        item.command = std::move(command);
        item.icon = icon;
        return item;
    }

    menu_item action_item(std::string text, std::function<void()> action, std::function<bool()> checked)
    {
        menu_item item;
        item.text = std::move(text);
        item.action = std::move(action);
        item.checked = std::move(checked);
        return item;
    }

    menu_item submenu_item(std::string text, std::vector<menu_item> items)
    {
        menu_item item;
        item.text = std::move(text);
        item.submenu = std::move(items);
        return item;
    }

    std::vector<menu_item> window_menu(app_services &services)
    {
        std::vector<menu_item> items;
        for (window_kind kind : all_window_kinds)
        {
            menu_item item = command_item(std::string(window_name(kind)), window_command(kind));
            item.checked = [&services, kind] { return services.is_open(kind); };
            items.push_back(std::move(item));
        }
        return items;
    }

    void add_standard_commands(window_host &host)
    {
        ui::screen &s = host.screen();
        app_services &services = host.services();

        s.add_command("file.open", [&host] { host.services().open_with_dialog(host.kind()); });
        s.add_command("file.preferences", [&host] { open_preferences(host); });
        s.add_command("file.close", [&host] { host.close(); });
        s.add_command("file.exit", [&services] { services.exit(); });
        for (window_kind kind : all_window_kinds)
            s.add_command(window_command(kind), [&services, kind] { services.open_window(kind); });
        s.add_command("help.about", [&host] { open_about(host); });
        s.add_command("view.menu", [&host] {
            if (menu_bar *bar = host.menus())
                bar->open(0, true);
        });

        shortcut_table &keys = s.shortcuts();
        (void)keys.bind("Ctrl+O", "file.open");
        (void)keys.bind("Ctrl+,", "file.preferences");
        (void)keys.bind("Ctrl+W", "file.close");
        (void)keys.bind("Ctrl+Q", "file.exit");
        (void)keys.bind("Ctrl+1", window_command(window_kind::project_manager));
        (void)keys.bind("Ctrl+2", window_command(window_kind::schematic_editor));
        (void)keys.bind("Ctrl+3", window_command(window_kind::board_editor));
        (void)keys.bind("Ctrl+4", window_command(window_kind::library_browser));
        (void)keys.bind("F1", "help.about");
        (void)keys.bind("F10", "view.menu");
    }

    std::unique_ptr<widget> build_menu_bar(window_host &host, window_layout &layout)
    {
        auto strip = std::make_unique<panel>("", surface::panel, 0.0f, insets{4.0f, 0.0f, 4.0f, 0.0f});
        strip->set_edges(false, true, false, false);
        auto bar = std::make_unique<menu_bar>();
        bar->set_name("menu bar");
        host.set_menu_bar(bar.get());

        // Open... first, unless the content puts it among its own items.
        std::vector<menu_item> file = std::move(layout.file_items);
        if (std::ranges::none_of(file, [](const menu_item &item) { return item.command == "file.open"; }))
            file.insert(file.begin(), command_item("Open\xE2\x80\xA6", "file.open", icon::open));
        if (!file.empty())
            file.push_back(menu_item::separator());
        file.push_back(command_item("Preferences\xE2\x80\xA6", "file.preferences"));
        file.push_back(menu_item::separator());
        // In the project manager, closing the window is exiting.
        if (host.kind() != window_kind::project_manager)
            file.push_back(command_item("Close Window", "file.close"));
        file.push_back(command_item("Exit", "file.exit"));
        bar->add_menu("File", std::move(file));

        for (window_layout::menu &m : layout.menus)
            bar->add_menu(std::move(m.title), std::move(m.items));
        layout.menus.clear();

        bar->add_menu("Window", window_menu(host.services()));
        bar->add_menu("Help", {command_item("About Volt", "help.about")});

        strip->add(std::move(bar));
        return strip;
    }

    void add_brand(toolbar &bar, window_host &host)
    {
        auto &logo = bar.emplace<image_view>(texture_id{}, math::vec2f{toolbar_logo, toolbar_logo});
        logo.set_name("logo");
        host.track_logo(logo, toolbar_logo);
        auto &brand = bar.emplace<label>("VOLT");
        brand.set_size(text_size::title);
        brand.layout().min_size = {48.0f, 0.0f};
        bar.add_separator();
    }

    button &add_command_tool(toolbar &bar, window_host &host, ui::icon icon, std::string tooltip, std::string command,
                             std::string text)
    {
        button &tool = bar.add_tool(icon, std::move(tooltip), std::move(text));
        tool.clicked.connect([&host, command = std::move(command)] { host.screen().run_command(command); });
        return tool;
    }

} // namespace volt::ui
