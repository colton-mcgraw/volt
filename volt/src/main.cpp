#include <app/volt.hpp>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char **argv)
{
    try
    {
        volt::volt_app app;

        // The assets are staged next to the executable.
        if (argc > 0)
            app.set_asset_root(std::filesystem::path(argv[0]).parent_path() / "assets");

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);

            // See volt_app::set_extended_color() for why this is not the default.
            if (arg == "--hdr")
                app.set_extended_color(true);

            // --ui-scale 1.25: the interface 25% bigger than the system
            // makes everything on each window's display. Preferences
            // changes it later. See <app/ui/scale.hpp>.
            if (arg == "--ui-scale" && i + 1 < argc)
                app.set_ui_scale(std::strtof(argv[++i], nullptr));

            // --open board.step: opens the file as dropping it on the
            // project manager would. May be given more than once.
            if (arg == "--open" && i + 1 < argc)
                app.add_open_path(argv[++i]);

            // --library-dir D:\parts: scans that directory's .voltlib files
            // too, ahead of the user's and the shipped libraries. May be
            // given more than once; the first given comes first. (The
            // argument is in the system's encoding, as std::filesystem::path
            // reads a narrow string.)
            if (arg == "--library-dir" && i + 1 < argc)
                app.add_library_directory(std::filesystem::path(argv[++i]));

            // --smoke-frames 120: exits, successfully, once that many frames
            // have been presented. For automated smoke runs.
            if (arg == "--smoke-frames" && i + 1 < argc)
                app.set_smoke_frames(std::strtoull(argv[++i], nullptr, 10));

            // --windows schematic,board,library: opens those windows at
            // start too, beside the project manager (which always opens).
            if (arg == "--windows" && i + 1 < argc)
            {
                std::string_view list(argv[++i]);
                while (!list.empty())
                {
                    const std::size_t comma = list.find(',');
                    const std::string_view key = list.substr(0, comma);
                    if (const auto kind = volt::ui::window_kind_from_key(key))
                        app.add_start_window(*kind);
                    else if (!key.empty())
                        std::cerr << "--windows: no window called \"" << key
                                  << "\" (project, schematic, board, library)" << std::endl;
                    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
                }
            }
        }

        if (auto initialized = app.initialize(); !initialized)
        {
            std::cerr << "Initialization failed: " << initialized.error() << std::endl;
            return 1;
        }

        if (auto ran = app.run(); !ran)
        {
            std::cerr << "Rendering failed: " << ran.error() << std::endl;
            return 1;
        }

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
