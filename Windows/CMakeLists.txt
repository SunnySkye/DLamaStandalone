cmake_minimum_required(VERSION 3.20)

project(DelayLamaWindows VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Keep the release ZIP self-contained: do not require a separately
    # installed Visual C++ runtime on the target Windows machine.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "The native standalone target in Windows/ requires a Windows host.")
endif()

set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_library(delay_lama_dsp STATIC
    "${PROJECT_ROOT}/DSP/delay.c"
    "${PROJECT_ROOT}/DSP/synth.c"
    "${PROJECT_ROOT}/DSP/voice.c"
)
target_include_directories(delay_lama_dsp PUBLIC "${PROJECT_ROOT}/DSP")

if(MSVC)
    target_compile_options(delay_lama_dsp PRIVATE /W4)
else()
    target_compile_options(delay_lama_dsp PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(delay_lama_windows_bridge STATIC standalone_bridge_win32.c)
target_include_directories(delay_lama_windows_bridge PRIVATE
    "${PROJECT_ROOT}/DSP"
    "${PROJECT_ROOT}/Sources"
)
target_link_libraries(delay_lama_windows_bridge PUBLIC delay_lama_dsp winmm)

if(MSVC)
    target_compile_options(delay_lama_windows_bridge PRIVATE /W4)
else()
    target_compile_options(delay_lama_windows_bridge PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(DelayLamaStandalone WIN32 delay_lama_win32.cpp)
target_link_libraries(DelayLamaStandalone PRIVATE
    delay_lama_windows_bridge
    gdiplus
    gdi32
    shell32
    user32
    winmm
)
target_compile_definitions(DelayLamaStandalone PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(DelayLamaStandalone PRIVATE /W4 /EHsc)
else()
    target_compile_options(DelayLamaStandalone PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(WINDOWS_RESOURCE_FILES
    background.png
    faces.png
    about.png
    DelayLama-Original-Manual.pdf
)
add_custom_command(TARGET DelayLamaStandalone POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:DelayLamaStandalone>/Resources"
    VERBATIM
)
foreach(resource IN LISTS WINDOWS_RESOURCE_FILES)
    add_custom_command(TARGET DelayLamaStandalone POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_ROOT}/Resources/${resource}"
            "$<TARGET_FILE_DIR:DelayLamaStandalone>/Resources/${resource}"
        VERBATIM
    )
endforeach()
add_custom_command(TARGET DelayLamaStandalone POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_ROOT}/MONKSYNTH-LICENSE.txt"
        "$<TARGET_FILE_DIR:DelayLamaStandalone>/MONKSYNTH-LICENSE.txt"
    VERBATIM
)

add_executable(delay_lama_dsp_smoke "${PROJECT_ROOT}/Tests/dsp_smoke.c")
target_link_libraries(delay_lama_dsp_smoke PRIVATE delay_lama_dsp)
target_include_directories(delay_lama_dsp_smoke PRIVATE "${PROJECT_ROOT}/DSP")
if(NOT MSVC)
    target_link_libraries(delay_lama_dsp_smoke PRIVATE m)
endif()

# These headless tests are deliberately independent of the GUI and audio
# device. They exercise the same native C sources that the executable links.
add_executable(delay_lama_windows_smoke "${PROJECT_ROOT}/Tests/windows_smoke.c")
target_link_libraries(delay_lama_windows_smoke PRIVATE delay_lama_dsp)
target_include_directories(delay_lama_windows_smoke PRIVATE "${PROJECT_ROOT}/DSP")
if(NOT MSVC)
    target_link_libraries(delay_lama_windows_smoke PRIVATE m)
endif()

add_executable(delay_lama_windows_bridge_smoke
    "${PROJECT_ROOT}/Tests/windows_bridge_smoke.c"
)
target_include_directories(delay_lama_windows_bridge_smoke PRIVATE
    "${PROJECT_ROOT}/Sources"
)
target_link_libraries(delay_lama_windows_bridge_smoke PRIVATE delay_lama_windows_bridge)

enable_testing()
add_test(NAME delay_lama_dsp_smoke COMMAND delay_lama_dsp_smoke)
add_test(NAME delay_lama_windows_smoke COMMAND delay_lama_windows_smoke)
add_test(NAME delay_lama_windows_bridge_smoke COMMAND delay_lama_windows_bridge_smoke)
