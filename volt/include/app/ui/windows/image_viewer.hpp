#pragma once

#include <app/ui/windows/content.hpp>

#include <math/vector.hpp>

#include <string>

// -------------------------------------------------------------------------
// the image viewer
// -------------------------------------------------------------------------
//
// An image file dropped on any window opens here, over that window: the
// image fitted into most of the window, on a checkerboard where it is
// transparent, with its name and what it is. Modal; the texture (an id
// from app_services::add_image()) is given back when it closes, however
// it closes, the window closing included. window_host::show_image() is
// the way in. Interface thread.

namespace volt::ui
{

    void open_image_viewer(window_host &host, texture_id image, math::vec2i pixels, std::string title,
                           std::string details);

} // namespace volt::ui
