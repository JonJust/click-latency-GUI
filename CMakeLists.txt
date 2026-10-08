cmake_minimum_required(VERSION 3.16)
project(click_latency VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CLICK_LATENCY_BUILD_GUI "Build the Linux Dear ImGui application" ON)
include(CTest)
find_package(Threads REQUIRED)

function(click_latency_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endfunction()

add_library(click_latency_core STATIC src/measurement.cpp)
target_include_directories(click_latency_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(click_latency_core PUBLIC Threads::Threads)
click_latency_warnings(click_latency_core)

if(BUILD_TESTING)
    add_executable(measurement_tests tests/measurement_test.cpp)
    target_link_libraries(measurement_tests PRIVATE click_latency_core)
    click_latency_warnings(measurement_tests)
    add_test(NAME measurement_tests COMMAND measurement_tests)
endif()

if(CLICK_LATENCY_BUILD_GUI)
    find_package(OpenGL REQUIRED)

    # Build the pinned SDL source offline. Desktop/audio backends are detected
    # from installed development headers and loaded dynamically at runtime.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_DEPS_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG ON CACHE BOOL "" FORCE)
    foreach(subsystem GPU RENDER JOYSTICK HAPTIC HIDAPI SENSOR CAMERA POWER TRAY)
        set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
    endforeach()
    foreach(backend JACK SNDIO OSS DISKAUDIO KMSDRM VULKAN OPENGLES)
        set(SDL_${backend} OFF CACHE BOOL "" FORCE)
    endforeach()
    # Screen-saver inhibition and synthetic X11 input are unnecessary here.
    set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/SDL3 EXCLUDE_FROM_ALL)

    add_library(click_latency_imgui STATIC
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl3.cpp
        third_party/imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(click_latency_imgui SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends"
    )
    target_link_libraries(click_latency_imgui PUBLIC SDL3::SDL3 OpenGL::GL)

    add_library(click_latency_gui STATIC
        src/gui.cpp src/audio_capture.cpp src/graph_export.cpp
        src/export_locations.cpp src/folder_picker.cpp
    )
    target_link_libraries(click_latency_gui PUBLIC click_latency_core click_latency_imgui)
    click_latency_warnings(click_latency_gui)

    add_executable(click_latency src/main.cpp)
    target_link_libraries(click_latency PRIVATE click_latency_gui)
    click_latency_warnings(click_latency)

    if(BUILD_TESTING)
        add_executable(export_tests tests/export_test.cpp)
        target_link_libraries(export_tests PRIVATE click_latency_gui)
        click_latency_warnings(export_tests)
        add_test(NAME export_tests COMMAND export_tests)
        set_tests_properties(export_tests PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=offscreen;SDL_AUDIODRIVER=dummy")

        add_executable(export_locations_tests tests/export_locations_test.cpp)
        target_link_libraries(export_locations_tests PRIVATE click_latency_gui)
        click_latency_warnings(export_locations_tests)
        add_test(NAME export_locations_tests COMMAND export_locations_tests)

        add_executable(folder_picker_tests tests/folder_picker_test.cpp)
        target_link_libraries(folder_picker_tests PRIVATE click_latency_gui)
        click_latency_warnings(folder_picker_tests)
        add_test(NAME folder_picker_tests COMMAND folder_picker_tests)
    endif()
endif()
