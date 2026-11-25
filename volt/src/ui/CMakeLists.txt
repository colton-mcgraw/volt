# The interface framework: a retained widget tree, its layout, input routing,
# focus, popups, theme and the draw list it paints into. No GPU and no window:
# input arrives as the events of <app/ui/input/events.hpp> (header-only), text
# goes through volt_font, and the draw list names textures by id. The Vulkan
# side that draws the list lives in the application (app/ui/renderer.cpp).
add_library(volt_ui STATIC
  draw_list.cpp
  text.cpp
  theme.cpp
  painter.cpp
  widget.cpp
  screen.cpp
  layout/box.cpp
  layout/grid.cpp
  widgets/label.cpp
  widgets/button.cpp
  widgets/checkbox.cpp
  widgets/radio_group.cpp
  widgets/slider.cpp
  widgets/progress_bar.cpp
  widgets/panel.cpp
  widgets/bars.cpp
  widgets/scroll_area.cpp
  widgets/splitter.cpp
  widgets/tab_view.cpp
  widgets/text_field.cpp
  widgets/numeric_field.cpp
  widgets/menu.cpp
  widgets/dropdown.cpp
  widgets/row_view.cpp
  widgets/list_view.cpp
  widgets/column_view.cpp
  widgets/property_grid.cpp
  widgets/viewport.cpp
  widgets/tool_strip.cpp
  widgets/log_view.cpp
  widgets/tile_grid.cpp
  widgets/toggle_list.cpp
  controllers.cpp
)

target_link_libraries(volt_ui PUBLIC volt_core volt_font)
volt_set_warnings(volt_ui)
