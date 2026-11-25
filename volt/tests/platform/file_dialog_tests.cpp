#include "math/check.hpp"

#include <platform/file_dialog.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// What the file dialogs are built from, short of showing one
// (<platform/file_dialog.hpp>): extensions as callers write them, the type
// list Windows shows, the default extension, UTF-8 paths, and the command
// lines and output of zenity and kdialog, which the Linux backend runs.
// All of it is portable, so Linux's command lines are checked on Windows
// too.

using platform::file_dialog;
using platform::file_dialog_mode;
using platform::file_filter;
using platform::path_from_utf8;
using platform::path_to_utf8;

namespace
{
    // A path written with forward slashes, as the tests spell them.
    std::string generic(const std::filesystem::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    void extensions()
    {
        using platform::normalize_extension;

        // However a caller writes one, it comes out bare and in lower case.
        CHECK(normalize_extension("step") == "step");
        CHECK(normalize_extension(".STEP") == "step");
        CHECK(normalize_extension("*.Step") == "step");
        CHECK(normalize_extension("voltproj") == "voltproj");

        // Every file.
        CHECK(normalize_extension("*") == "*");
        CHECK(normalize_extension("*.*") == "*");

        // Nothing a pattern list could carry as it is.
        CHECK(normalize_extension("").empty());
        CHECK(normalize_extension(".").empty());
        CHECK(normalize_extension("*.").empty());
        CHECK(normalize_extension("st*p").empty());
        CHECK(normalize_extension("a b").empty());
        CHECK(normalize_extension("tar.gz").empty());
        CHECK(normalize_extension("png;*.exe").empty());
        CHECK(normalize_extension("png|x").empty());
        CHECK(normalize_extension("a/b").empty());
        CHECK(normalize_extension("png\n").empty());

        // Repeats and the unusable go.
        const file_filter models{"3D model", {"step", ".STEP", "stp", "*.stp", "", "s?p"}};
        CHECK(platform::filter_extensions(models) == std::vector<std::string>{"step", "stp"});
    }

    void filters()
    {
        const file_filter models{"3D model", {"step", "stp"}};
        CHECK(platform::filter_patterns(models) == "*.step;*.stp");
        CHECK(platform::filter_patterns(models, " ") == "*.step *.stp");
        CHECK(platform::filter_label(models) == "3D model (*.step;*.stp)");
        CHECK(platform::filter_label(models, " ") == "3D model (*.step *.stp)");

        const file_filter everything{"All files", {"*.*"}};
        CHECK(platform::filter_patterns(everything) == "*");
        CHECK(platform::filter_label(everything) == "All files (*)");

        // A name that says its patterns itself is left alone; without a
        // name the patterns are the label, and without patterns the name.
        CHECK(platform::filter_label({"Images (PNG, JPEG)", {"png", "jpg"}}) == "Images (PNG, JPEG)");
        CHECK(platform::filter_label({"", {"png"}}) == "*.png");
        CHECK(platform::filter_label({"Nothing", {"*?"}}) == "Nothing");
        CHECK(platform::filter_patterns({"Nothing", {}}).empty());

        // Matching goes by extension, in any case.
        CHECK(platform::filter_matches(models, path_from_utf8("C:/parts/BRACKET.STEP")));
        CHECK(platform::filter_matches(models, path_from_utf8("/home/u/r.stp")));
        CHECK(!platform::filter_matches(models, path_from_utf8("/home/u/r.stl")));
        CHECK(!platform::filter_matches(models, path_from_utf8("/home/u/step")));
        CHECK(!platform::filter_matches(models, path_from_utf8("/home/u/r.")));
        CHECK(platform::filter_matches(everything, path_from_utf8("/home/u/README")));
    }

    void default_extension()
    {
        using platform::with_default_extension;
        const auto with = [](std::string_view path, std::string_view extension) {
            return generic(with_default_extension(path_from_utf8(path), extension));
        };

        CHECK(with("/p/Blinky", "voltproj") == "/p/Blinky.voltproj");
        CHECK(with("/p/Blinky", ".voltproj") == "/p/Blinky.voltproj");
        CHECK(with("/p/Blinky.", "voltproj") == "/p/Blinky.voltproj"); // the dot reused

        // A name with an extension keeps it, whichever it is.
        CHECK(with("/p/Blinky.voltproj", "voltproj") == "/p/Blinky.voltproj");
        CHECK(with("/p/Blinky.json", "voltproj") == "/p/Blinky.json");

        // A dot in a folder's name is not the file's extension.
        CHECK(with("/p/v1.2/Blinky", "voltproj") == "/p/v1.2/Blinky.voltproj");

        // Nothing to add, or nothing to add it to.
        CHECK(with("/p/Blinky", "") == "/p/Blinky");
        CHECK(with("/p/", "voltproj") == "/p/");
        CHECK(with("", "voltproj").empty());

        // Not ASCII.
        CHECK(with("/p/Schaltplan \xC3\x84rger", "voltproj") == "/p/Schaltplan \xC3\x84rger.voltproj");
    }

    void utf8_paths()
    {
        // Japanese and German, round trip.
        const std::string text = "C:/Projekte/\xC3\x9C" "bung/\xE6\x97\xA5\xE6\x9C\xAC.voltproj";
        const std::filesystem::path path = path_from_utf8(text);
        CHECK(generic(path) == text);
        CHECK(path_to_utf8(path.filename()) == "\xE6\x97\xA5\xE6\x9C\xAC.voltproj");
        CHECK(path.extension() == ".voltproj");
#ifdef _WIN32
        // What Windows gets: the UTF-16 of the name, not the code page's
        // reading of its bytes.
        CHECK(path.filename().wstring() == L"\u65E5\u672C.voltproj");
        CHECK(path_to_utf8(std::filesystem::path(L"C:\\\u00DC")) == "C:\\\xC3\x9C");
#endif
        CHECK(path_to_utf8({}).empty());
        CHECK(path_from_utf8("").empty());
    }

    file_dialog project_dialog(file_dialog_mode mode)
    {
        file_dialog dialog;
        dialog.mode = mode;
        dialog.title = "Open Project";
        dialog.filters = {{"Volt project", {"voltproj"}}, {"All files", {"*"}}};
        dialog.default_extension = "voltproj";
        dialog.directory = path_from_utf8("/home/u/Projects");
        return dialog;
    }

    void zenity()
    {
        using platform::chooser_command;
        using platform::chooser_program;
        using strings = std::vector<std::string>;

        CHECK(chooser_command(chooser_program::zenity, project_dialog(file_dialog_mode::open_file)) ==
              strings{"zenity", "--file-selection", "--title=Open Project", "--filename=/home/u/Projects/",
                      "--file-filter=Volt project (*.voltproj) | *.voltproj *.VOLTPROJ",
                      "--file-filter=All files (*) | *"});

        // Several files, one per line.
        file_dialog several = project_dialog(file_dialog_mode::open_files);
        several.title.clear();
        several.filters = {{"3D model", {"step", "stl"}}};
        CHECK(chooser_command(chooser_program::zenity, several) ==
              strings{"zenity", "--file-selection", "--multiple", "--separator=\n", "--filename=/home/u/Projects/",
                      "--file-filter=3D model (*.step *.stl) | *.step *.STEP *.stl *.STL"});

        // Saving: the suggested name in the folder, and the question before
        // replacing a file.
        file_dialog save = project_dialog(file_dialog_mode::save_file);
        save.title = "Save Project As";
        save.file_name = "Blinky.voltproj";
        save.filters.resize(1);
        CHECK(chooser_command(chooser_program::zenity, save) ==
              strings{"zenity", "--file-selection", "--title=Save Project As", "--save", "--confirm-overwrite",
                      "--filename=/home/u/Projects/Blinky.voltproj",
                      "--file-filter=Volt project (*.voltproj) | *.voltproj *.VOLTPROJ"});

        // A folder: no filters. Nowhere to start: no --filename.
        file_dialog folder = project_dialog(file_dialog_mode::choose_folder);
        folder.directory.clear();
        CHECK(chooser_command(chooser_program::zenity, folder) ==
              strings{"zenity", "--file-selection", "--title=Open Project", "--directory"});

        // A bar would end the filter's name early.
        file_dialog barred;
        barred.filters = {{"A|B", {"x"}}};
        CHECK(chooser_command(chooser_program::zenity, barred) ==
              strings{"zenity", "--file-selection", "--file-filter=A/B (*.x) | *.x *.X"});
    }

    void kdialog()
    {
        using platform::chooser_command;
        using platform::chooser_program;
        using strings = std::vector<std::string>;

        CHECK(chooser_command(chooser_program::kdialog, project_dialog(file_dialog_mode::open_file)) ==
              strings{"kdialog", "--title", "Open Project", "--getopenfilename", "/home/u/Projects",
                      "*.voltproj *.VOLTPROJ|Volt project (*.voltproj)\n*|All files (*)"});

        file_dialog several = project_dialog(file_dialog_mode::open_files);
        several.title.clear();
        several.filters.resize(1);
        CHECK(chooser_command(chooser_program::kdialog, several) ==
              strings{"kdialog", "--multiple", "--separate-output", "--getopenfilename", "/home/u/Projects",
                      "*.voltproj *.VOLTPROJ|Volt project (*.voltproj)"});

        file_dialog save = project_dialog(file_dialog_mode::save_file);
        save.title.clear();
        save.file_name = "Blinky.voltproj";
        save.filters.resize(1);
        CHECK(chooser_command(chooser_program::kdialog, save) ==
              strings{"kdialog", "--getsavefilename", "/home/u/Projects/Blinky.voltproj",
                      "*.voltproj *.VOLTPROJ|Volt project (*.voltproj)"});

        // Without a folder, the one KDE remembers for Volt.
        file_dialog folder = project_dialog(file_dialog_mode::choose_folder);
        folder.title.clear();
        folder.directory.clear();
        CHECK(chooser_command(chooser_program::kdialog, folder) == strings{"kdialog", "--getexistingdirectory", ":volt"});
    }

    void chooser_output()
    {
        using platform::parse_chooser_output;

        const file_dialog open = project_dialog(file_dialog_mode::open_files);
        const auto paths = parse_chooser_output("/home/u/a.voltproj\n/home/u/b c.step\r\n\n", open);
        CHECK(paths.size() == 2);
        CHECK(paths.size() == 2 && generic(paths[0]) == "/home/u/a.voltproj");
        CHECK(paths.size() == 2 && generic(paths[1]) == "/home/u/b c.step");

        // Nothing printed: nothing chosen.
        CHECK(parse_chooser_output("", open).empty());
        CHECK(parse_chooser_output("\n", open).empty());

        // A name saved without its extension gets the default one; opening
        // takes names as they are.
        const file_dialog save = project_dialog(file_dialog_mode::save_file);
        const auto saved = parse_chooser_output("/home/u/Blinky\n", save);
        CHECK(saved.size() == 1 && generic(saved[0]) == "/home/u/Blinky.voltproj");
        const auto opened = parse_chooser_output("/home/u/Blinky\n", open);
        CHECK(opened.size() == 1 && generic(opened[0]) == "/home/u/Blinky");

        // UTF-8 from the program is UTF-8 in the path.
        const auto named = parse_chooser_output("/home/u/\xC3\x9C" "bung.voltproj\n", open);
        CHECK(named.size() == 1 && path_to_utf8(named[0].filename()) == "\xC3\x9C" "bung.voltproj");
    }

    void unconfirmed_replacement()
    {
        using platform::unconfirmed_replacement;

        // A folder of its own, holding Blinky.voltproj.
        std::error_code error;
        const std::filesystem::path folder =
            std::filesystem::temp_directory_path(error) / "volt-file-dialog-tests-replacement";
        std::filesystem::remove_all(folder, error);
        std::filesystem::create_directories(folder, error);
        const std::filesystem::path existing = folder / "Blinky.voltproj";
        {
            std::ofstream file(existing);
            file << "{}";
        }
        const std::string typed = path_to_utf8(folder / "Blinky") + "\n";
        const std::string full = path_to_utf8(existing) + "\n";
        const std::string other = path_to_utf8(folder / "Other") + "\n";

        // "Blinky" typed, made "Blinky.voltproj", which exists: the dialog
        // never asked about replacing it.
        const file_dialog save = project_dialog(file_dialog_mode::save_file);
        CHECK(unconfirmed_replacement(typed, save) == existing);

        // Named in full, the program asked itself; a new name replaces
        // nothing; cancelled, nothing was chosen.
        CHECK(unconfirmed_replacement(full, save).empty());
        CHECK(unconfirmed_replacement(other, save).empty());
        CHECK(unconfirmed_replacement("", save).empty());

        // Only saving adds an extension; no default extension adds none.
        CHECK(unconfirmed_replacement(typed, project_dialog(file_dialog_mode::open_file)).empty());
        file_dialog bare = save;
        bare.default_extension.clear();
        CHECK(unconfirmed_replacement(typed, bare).empty());

        std::filesystem::remove_all(folder, error);
    }
} // namespace

int main()
{
    extensions();
    filters();
    default_extension();
    utf8_paths();
    zenity();
    kdialog();
    chooser_output();
    unconfirmed_replacement();
    return volt::tests::result("platform file dialogs");
}
