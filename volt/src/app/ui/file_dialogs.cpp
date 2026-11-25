#include <app/ui/file_dialogs.hpp>

#include <library/library.hpp>
#include <library/package.hpp>
#include <library/project.hpp>

#include <string>
#include <vector>

namespace volt::ui
{
    namespace
    {
        platform::file_filter filter(std::string name, std::string_view extension)
        {
            return {std::move(name), {std::string(extension)}};
        }

        template <std::size_t N>
        platform::file_filter filter(std::string name, const std::string_view (&extensions)[N])
        {
            platform::file_filter result{std::move(name), {}};
            for (const std::string_view extension : extensions)
                result.extensions.emplace_back(extension);
            return result;
        }

        platform::file_filter project_filter()
        {
            return filter("Volt project", library::project_extension);
        }
        platform::file_filter library_filter()
        {
            return filter("Parts library", library::library_extension);
        }
        platform::file_filter package_filter()
        {
            return filter("Part package", library::package_extension);
        }
        platform::file_filter model_filter()
        {
            return filter("3D model", model_extensions);
        }
        platform::file_filter image_filter()
        {
            return filter("Image", image_extensions);
        }
        platform::file_filter all_files()
        {
            return {"All files", {"*"}};
        }
    } // namespace

    platform::file_dialog file_dialog_for(file_purpose purpose)
    {
        using platform::file_dialog_mode;

        platform::file_dialog dialog;
        switch (purpose)
        {
        case file_purpose::open_any:
        {
            // Everything at once first, so that a folder of mixed files
            // shows all of them; then each kind on its own.
            platform::file_filter everything{"Everything Volt opens", {}};
            for (const platform::file_filter &kind :
                 {project_filter(), library_filter(), package_filter(), model_filter(), image_filter()})
                everything.extensions.insert(everything.extensions.end(), kind.extensions.begin(),
                                             kind.extensions.end());
            dialog.mode = file_dialog_mode::open_files;
            dialog.title = "Open";
            dialog.filters = {everything,     project_filter(), library_filter(), package_filter(),
                              model_filter(), image_filter(),   all_files()};
            break;
        }
        case file_purpose::project:
            dialog.title = "Open Project";
            dialog.filters = {project_filter()};
            break;
        case file_purpose::save_project:
            dialog.mode = file_dialog_mode::save_file;
            dialog.title = "Save Project As";
            dialog.filters = {project_filter()};
            dialog.default_extension = platform::normalize_extension(library::project_extension);
            break;
        case file_purpose::project_folder:
            dialog.mode = file_dialog_mode::choose_folder;
            dialog.title = "Choose the Folder for the New Project";
            break;
        case file_purpose::library:
            dialog.title = "Open Parts Library";
            dialog.filters = {library_filter()};
            break;
        case file_purpose::part_package:
            dialog.title = "Import Part Package";
            dialog.filters = {package_filter()};
            break;
        case file_purpose::model:
            dialog.title = "Choose a 3D Model";
            dialog.filters = {model_filter(), all_files()};
            break;
        case file_purpose::image:
            dialog.title = "Open Image";
            dialog.filters = {image_filter(), all_files()};
            break;
        }
        return dialog;
    }

} // namespace volt::ui
