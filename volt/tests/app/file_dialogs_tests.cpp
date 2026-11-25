#include "math/check.hpp"

#include <app/ui/file_dialogs.hpp>
#include <image/io.hpp>
#include <library/library.hpp>
#include <library/package.hpp>
#include <library/project.hpp>
#include <model/io.hpp>

#include <string>
#include <string_view>

// The application's file dialogs (<app/ui/file_dialogs.hpp>, app/ui/
// file_dialogs.cpp compiled in): what each one asks for, and that what
// File > Open offers is what app_services::open_files() opens, the way a
// drop goes, and the other way round.

using namespace volt;
using platform::file_dialog_mode;
using ui::file_dialog_for;
using ui::file_purpose;

namespace
{
    // What open_files() does with a file of this name: true when it is a
    // project, a library, a part package, a model or an image.
    bool opens(std::string_view name)
    {
        const std::filesystem::path path = platform::path_from_utf8(name);
        const std::string extension = platform::normalize_extension(platform::path_to_utf8(path.extension()));
        const auto is = [&](std::string_view of) { return extension == platform::normalize_extension(of); };
        return is(library::project_extension) || is(library::library_extension) ||
               is(library::package_extension) || model::format_of(path) != model::format::unknown ||
               image::format_from_extension(path) != image::file_format::unknown;
    }

    void open_any()
    {
        const platform::file_dialog dialog = file_dialog_for(file_purpose::open_any);
        CHECK(dialog.mode == file_dialog_mode::open_files);
        CHECK(dialog.filters.size() == 7);
        if (dialog.filters.size() != 7)
            return;

        // The first type is everything; each of its extensions opens, in
        // either case.
        const platform::file_filter &everything = dialog.filters.front();
        CHECK(platform::filter_extensions(everything).size() == 3 + std::size(ui::model_extensions) +
                                                                    std::size(ui::image_extensions));
        for (const std::string &extension : platform::filter_extensions(everything))
        {
            CHECK(opens("part." + extension));
            std::string shouted = extension;
            for (char &c : shouted)
                c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            CHECK(opens("PART." + shouted));
        }

        // And what opens is listed.
        for (const char *name : {"Blinky.voltproj", "parts.voltlib", "r.voltpart", "board.step", "b.stp", "m.obj",
                                 "m.stl", "logo.png", "photo.jpg", "photo.jpeg", "icon.bmp", "logo.svg"})
        {
            CHECK(opens(name));
            CHECK(platform::filter_matches(everything, platform::path_from_utf8(name)));
        }
        CHECK(!opens("notes.txt"));
        CHECK(!platform::filter_matches(everything, platform::path_from_utf8("notes.txt")));

        // Each kind on its own, then every file.
        CHECK(platform::filter_patterns(dialog.filters[1]) == "*.voltproj");
        CHECK(platform::filter_patterns(dialog.filters[2]) == "*.voltlib");
        CHECK(platform::filter_patterns(dialog.filters[3]) == "*.voltpart");
        CHECK(platform::filter_patterns(dialog.filters.back()) == "*");
    }

    void purposes()
    {
        // Every purpose has a dialog; files have types to choose from,
        // folders none.
        for (std::size_t i = 0; i < ui::file_purpose_count; ++i)
        {
            const auto purpose = static_cast<file_purpose>(i);
            const platform::file_dialog dialog = file_dialog_for(purpose);
            CHECK(!dialog.title.empty());
            CHECK(dialog.directory.empty());
            if (dialog.mode == file_dialog_mode::choose_folder)
                CHECK(dialog.filters.empty());
            else
                CHECK(!dialog.filters.empty() && !platform::filter_patterns(dialog.filters.front()).empty());
        }

        const platform::file_dialog save = file_dialog_for(file_purpose::save_project);
        CHECK(save.mode == file_dialog_mode::save_file);
        CHECK(save.default_extension == "voltproj");
        CHECK(platform::filter_label(save.filters.front()) == "Volt project (*.voltproj)");

        CHECK(file_dialog_for(file_purpose::project).mode == file_dialog_mode::open_file);
        CHECK(file_dialog_for(file_purpose::project_folder).mode == file_dialog_mode::choose_folder);

        // A model to choose: the three formats, STEP's three names.
        const platform::file_dialog model = file_dialog_for(file_purpose::model);
        CHECK(platform::filter_patterns(model.filters.front()) == "*.step;*.stp;*.p21;*.obj;*.stl");
        for (const std::string_view extension : ui::model_extensions)
            CHECK(model::format_of(platform::path_from_utf8("m." + std::string(extension))) != model::format::unknown);
        for (const std::string_view extension : ui::image_extensions)
            CHECK(image::format_from_extension(platform::path_from_utf8("i." + std::string(extension))) !=
                  image::file_format::unknown);
    }
} // namespace

int main()
{
    open_any();
    purposes();
    return volt::tests::result("app file dialogs");
}
