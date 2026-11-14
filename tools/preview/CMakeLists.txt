cmake_minimum_required(VERSION 3.20)
project(sentinel_ui_preview LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(IMGUI_SOURCE_DIR "" CACHE PATH "Existing Dear ImGui v1.91.9 source folder")
set(STB_INCLUDE_DIR "" CACHE PATH "Existing folder containing stb_image_write.h")
if(NOT EXISTS "${IMGUI_SOURCE_DIR}/imgui.cpp" OR NOT EXISTS "${STB_INCLUDE_DIR}/stb_image_write.h")
  message(FATAL_ERROR "Set IMGUI_SOURCE_DIR and STB_INCLUDE_DIR to existing dependency sources; this tool never downloads them.")
endif()
find_package(Lua 5.4 REQUIRED)
add_executable(ui_preview ../ui_preview.cpp
  "${IMGUI_SOURCE_DIR}/imgui.cpp" "${IMGUI_SOURCE_DIR}/imgui_draw.cpp"
  "${IMGUI_SOURCE_DIR}/imgui_tables.cpp" "${IMGUI_SOURCE_DIR}/imgui_widgets.cpp")
target_include_directories(ui_preview PRIVATE "${IMGUI_SOURCE_DIR}" "${STB_INCLUDE_DIR}" "${LUA_INCLUDE_DIR}")
target_link_libraries(ui_preview PRIVATE ${LUA_LIBRARIES})
