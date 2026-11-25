# Tests of the font module. They read only the repository's fonts, found
# through VOLT_ASSETS_DIR.
foreach(suite IN ITEMS sfnt outline raster text atlas cff corruption)
  volt_add_test(volt_font_${suite}_tests font/${suite}_tests.cpp)
  target_link_libraries(volt_font_${suite}_tests PRIVATE volt_font)
  target_compile_definitions(volt_font_${suite}_tests PRIVATE VOLT_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
endforeach()

# Tools, not tests: a rendered specimen to look at (with timings), and a
# robustness run over whatever fonts the system has. Neither is registered
# with CTest; run them by hand.
foreach(tool IN ITEMS specimen robustness)
  add_executable(volt_font_${tool} font/${tool}.cpp)
  target_include_directories(volt_font_${tool} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
  target_link_libraries(volt_font_${tool} PRIVATE volt_font)
  target_compile_definitions(volt_font_${tool} PRIVATE VOLT_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
  volt_set_warnings(volt_font_${tool})
endforeach()
