// app_services: the file dialogs, and saving the project somewhere else
// (what Save Project As asks a dialog for). See "file dialogs" in
// <app/ui/services.hpp>.

#include <app/ui/services.hpp>

#include <app/ui/windows/content.hpp>
#include <library/library.hpp>
#include <library/project.hpp>
#include <platform/file_dialog.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace volt::ui
{

    std::filesystem::path app_services::file_folder(file_purpose purpose) const
    {
        if (const std::filesystem::path &last = m_file_folders[index_of(purpose)]; !last.empty())
            return last;

        // Nothing chosen yet: the open project's folder is where its files
        // are, and a new project goes next to it.
        if (m_project_path.empty())
            return {};
        const std::filesystem::path folder = m_project_path.parent_path();
        switch (purpose)
        {
        case file_purpose::project_folder:
            return folder.parent_path();
        case file_purpose::open_any:
        case file_purpose::project:
        case file_purpose::save_project:
        case file_purpose::model:
            return folder;
        case file_purpose::library:
        case file_purpose::part_package:
        case file_purpose::image:
            break;
        }
        return {};
    }

    void app_services::show_file_dialog(window_kind from, file_purpose purpose, platform::file_dialog dialog,
                                        std::function<void(platform::file_dialog_result)> then)
    {
        const auto refuse = [&then](std::string message) {
            if (then)
                then(std::unexpected(platform::file_dialog_error{.unavailable = false, .message = std::move(message)}));
        };

        // One at a time, application wide: a second one, modal to another
        // window, would leave two of them waiting on each other's loops.
        if (m_file_dialog)
        {
            refuse(*m_file_dialog == from
                       ? std::string("a file dialog is open already")
                       : std::format("a file dialog is open already, in the {}", window_name(*m_file_dialog)));
            return;
        }
        window_host *host = window(from);
        platform::native_window *owner = host != nullptr ? host->native() : nullptr;
        if (owner == nullptr)
        {
            refuse("its window has closed");
            return;
        }

        if (dialog.directory.empty())
            dialog.directory = file_folder(purpose);
        const bool folder = dialog.mode == platform::file_dialog_mode::choose_folder;

        // The platform answers on its own thread (or on this one, at once,
        // if it cannot show anything); the answer is handled here, on the
        // interface thread, like any other posted work.
        m_file_dialog = from;
        platform::show_file_dialog(*owner, std::move(dialog),
                                   [this, purpose, folder, then = std::move(then)](platform::file_dialog_result result) {
                                       post([this, purpose, folder, then, result = std::move(result)]() mutable {
                                           m_file_dialog.reset();
                                           if (result && !result->empty())
                                           {
                                               const std::filesystem::path &first = result->front();
                                               m_file_folders[index_of(purpose)] =
                                                   folder ? first : first.parent_path();
                                           }
                                           if (then)
                                               then(std::move(result));
                                       });
                                   });
    }

    void app_services::choose_file(window_kind from, file_purpose purpose,
                                   std::function<void(const std::filesystem::path &)> then)
    {
        show_file_dialog(from, purpose, file_dialog_for(purpose),
                         [this, from, then = std::move(then)](platform::file_dialog_result result) {
                             if (!result)
                             {
                                 report_file_dialog(from, result.error());
                                 return;
                             }
                             if (!result->empty() && then)
                                 then(result->front());
                         });
    }

    void app_services::open_with_dialog(window_kind from, std::function<void()> unavailable)
    {
        show_file_dialog(from, file_purpose::open_any, file_dialog_for(file_purpose::open_any),
                         [this, from, unavailable = std::move(unavailable)](platform::file_dialog_result result) {
                             if (!result)
                             {
                                 if (result.error().unavailable && unavailable)
                                     unavailable();
                                 else
                                     report_file_dialog(from, result.error());
                                 return;
                             }
                             if (result->empty())
                                 return;

                             // The way a drop goes, so that both open the
                             // same files the same way.
                             std::vector<std::string> paths;
                             for (const std::filesystem::path &path : *result)
                                 paths.push_back(platform::path_to_utf8(path));
                             open_files(paths, from);
                         });
    }

    void app_services::report_file_dialog(window_kind from, const platform::file_dialog_error &error)
    {
        if (error.unavailable)
            show_message(from, "No file dialog: " + error.message + ". Drop the files on the window instead.",
                         std::chrono::seconds(10), log_level::warning);
        else
            show_message(from, "The file dialog did not open: " + error.message, std::chrono::seconds(8),
                         log_level::warning);
    }

    void app_services::close_file_dialog()
    {
        // As if cancelled; the answer is posted, and dropped if nothing runs
        // posted work any more. The window may have gone first, and its
        // dialog with it.
        if (!m_file_dialog)
            return;
        if (window_host *host = window(*m_file_dialog); host != nullptr && host->native() != nullptr)
            platform::close_file_dialog(*host->native());
    }

    // ---- the project --------------------------------------------------------------------

    std::expected<void, std::string> app_services::save_project_as(const std::filesystem::path &file)
    {
        if (!m_project)
            return std::unexpected(std::string("No project is open"));

        std::error_code error;
        std::filesystem::path target = std::filesystem::absolute(file, error);
        if (error)
            target = file;
        target = platform::with_default_extension(target.lexically_normal(), library::project_extension);

        // The same file: an ordinary save.
        const bool same = std::filesystem::exists(target, error) && std::filesystem::exists(m_project_path, error)
                              ? std::filesystem::equivalent(target, m_project_path, error)
                              : target == m_project_path.lexically_normal();
        if (same)
            return save_project();

        // The project library beside it, named after it.
        std::filesystem::path library_path = target;
        library_path.replace_extension(library::library_extension);
        if (std::filesystem::exists(library_path, error) && !std::filesystem::exists(target, error))
            return std::unexpected(platform::path_to_utf8(library_path.filename()) +
                                   " exists already in that folder: choose another name or folder");

        library::project copy = *m_project;
        copy.library_file = platform::path_to_utf8(library_path.filename());
        const bool target_exists = std::filesystem::exists(target, error);

        const std::filesystem::path from_folder = m_project_path.parent_path();
        const std::filesystem::path to_folder = target.parent_path();
        const auto moved = [&](const std::string &stored) {
            return library::relative_path(to_folder, library::resolve_path(from_folder, stored));
        };

        // The sheets are written beside the new file, under the paths the
        // project lists them by (see save_project_as() in services.hpp for
        // the names). The open sheets are the project's, one per entry in
        // order; should they not be (a project of no sheets shows the
        // scratch sheet), the references are only moved.
        struct planned_sheet
        {
            std::size_t index = 0;
            std::string file;           // as the new project stores it
            std::filesystem::path path; // where it is written
        };
        std::vector<planned_sheet> sheets;
        const bool own_sheets = !m_schematic.scratch() && m_schematic.sheet_count() == copy.sheets.size();
        const std::string old_stem = platform::path_to_utf8(m_project_path.stem());
        const std::string new_stem = platform::path_to_utf8(target.stem());
        for (std::size_t i = 0; i < copy.sheets.size(); ++i)
        {
            library::sheet &sheet = copy.sheets[i];
            if (!own_sheets || !m_schematic.sheet_loaded(i) || sheet.file.empty())
            {
                // Not written: a sheet that did not load stays in its file,
                // which the new project refers to where it is.
                if (!sheet.file.empty())
                    sheet.file = moved(sheet.file);
                continue;
            }

            std::filesystem::path relative = library::path_from_utf8(sheet.file);
            if (relative.is_absolute())
                relative = relative.filename();
            if (platform::path_to_utf8(relative.stem()) == old_stem)
                relative.replace_filename(library::path_from_utf8(new_stem) += relative.extension());
            std::filesystem::path path = (to_folder / relative).lexically_normal();

            // Not over the file the sheet came from: that one stays the old
            // project's.
            const std::filesystem::path from = library::resolve_path(from_folder, sheet.file);
            const bool same_file = std::filesystem::exists(path, error) && std::filesystem::exists(from, error)
                                       ? std::filesystem::equivalent(path, from, error)
                                       : path == from.lexically_normal();
            if (same_file)
            {
                relative.replace_filename(library::path_from_utf8(new_stem + " - ") += relative.filename());
                path = (to_folder / relative).lexically_normal();
            }
            if (std::filesystem::exists(path, error) && !target_exists)
                return std::unexpected(platform::path_to_utf8(path.filename()) +
                                       " exists already in that folder: choose another name or folder");
            sheet.file = library::relative_path(to_folder, path);
            sheets.push_back({i, sheet.file, path});
        }
        // The board is not written yet: it is referred to where it is.
        if (!copy.board.empty())
            copy.board = moved(copy.board);

        std::filesystem::create_directories(to_folder, error);
        if (error)
            return std::unexpected("Cannot create " + platform::path_to_utf8(to_folder) + ": " + error.message());
        for (const planned_sheet &s : sheets)
        {
            if (auto written = m_schematic.write_sheet(s.index, s.path); !written)
                return written;
        }
        if (auto saved = library::save_project(target, copy); !saved)
            return std::unexpected(std::move(saved).error());

        // The sheets are in their new files now, as they are; those that
        // were not written are referred to from the new folder.
        m_sheets_busy = true;
        for (const planned_sheet &s : sheets)
            m_schematic.sheet_saved_as(s.index, s.file);
        if (own_sheets)
        {
            for (std::size_t i = 0; i < copy.sheets.size(); ++i)
            {
                if (std::ranges::find(sheets, i, &planned_sheet::index) == sheets.end())
                    m_schematic.set_sheet_file(i, copy.sheets[i].file);
            }
        }
        m_sheets_busy = false;

        *m_project = std::move(copy);
        m_project_path = target;
        m_project_modified = false;
        project_changed.emit();
        return {};
    }

} // namespace volt::ui
