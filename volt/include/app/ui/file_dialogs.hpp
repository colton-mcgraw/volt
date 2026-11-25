#pragma once

#include <platform/file_dialog.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

// -------------------------------------------------------------------------
// the application's file dialogs
// -------------------------------------------------------------------------
//
// What each of the application's file dialogs asks for: one purpose per
// kind of file, each with its dialog (mode, title, types, default
// extension). app_services shows them (<app/ui/services.hpp>, "file
// dialogs": choose_file(), show_file_dialog(), open_with_dialog()) and
// remembers, per purpose, the folder last chosen in, for the session.
//
// The types listed are the ones app_services::open_files() opens (which a
// test checks): what a dialog offers, a drop takes, and the other way
// round.

namespace volt::ui
{

    enum class file_purpose : std::uint8_t
    {
        open_any,       // File > Open: everything Volt opens, several at once
        project,        // a project (.voltproj) to open
        save_project,   // where a project is saved (Save Project As)
        project_folder, // the folder a new project goes in
        library,        // a parts library (.voltlib) to open
        part_package,   // a part package (.voltpart) to import
        model,          // a 3D model (STEP, OBJ, STL)
        image,          // an image (PNG, JPEG, BMP, SVG)
    };

    inline constexpr std::size_t file_purpose_count = 8;

    constexpr std::size_t index_of(file_purpose purpose) noexcept
    {
        return static_cast<std::size_t>(purpose);
    }

    // The dialog for `purpose`, without a folder to start in (app_services
    // adds the remembered one) or a suggested name.
    platform::file_dialog file_dialog_for(file_purpose purpose);

    // The extensions of each kind of file Volt opens, without dots, as the
    // dialogs list them.
    inline constexpr std::string_view model_extensions[] = {"step", "stp", "p21", "obj", "stl"};
    inline constexpr std::string_view image_extensions[] = {"png", "jpg", "jpeg", "jpe", "jfif",
                                                            "bmp", "dib", "svg", "svgz"};

} // namespace volt::ui
