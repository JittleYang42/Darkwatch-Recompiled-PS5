# Darkwatch Recompiled - the game project.
#   Windows: src/win (window, launcher, settings) + src/gpu (Vulkan renderer)
#   Linux:   src/main_posix.c (SDL2 runtime: development and testing)
# The game's own code is translated by xboxrecomp into src/recomp/gen from
# your copy of default.xbe (tools/generate.py); it is not part of this repo.
cmake_minimum_required(VERSION 3.20)
project(darkwatch C)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
set(DW_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH "generated sources")
if(NOT EXISTS "${DW_GEN_DIR}/recomp_funcs.h" AND NOT EXISTS "${DW_GEN_DIR}/recomp_0000.c")
    message(FATAL_ERROR "No generated sources in ${DW_GEN_DIR}.\n"
        "Generate them from your own copy of the game first:\n"
        "  python darkwatch_game/tools/generate.py <folder with default.xbe>\n"
        "(see docs/BUILDING.md)")
endif()

# The PC build targets x86-64-v3 (AVX2/FMA/BMI: any desktop CPU since 2013)
# -- the 8-wide vertex programs and the rest of the runtime run on AVX2, as
# the PS5 build's Zen 2 target does. win/cpu_check.c, built generic, refuses
# to start on an older CPU with a message instead of an illegal instruction.
if(WIN32 AND NOT MSVC)
    add_compile_options(-march=x86-64-v3 -mtune=znver4)
endif()
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${DW_GEN_DIR}/*.c")

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    # The PC host: its own window, launcher and settings (src/win).
    set(HOST_SOURCES src/win/cpu_check.c src/win/win_main.c src/win/dw_settings.c src/win/dw_tables.c src/win/launcher.c src/win/launcher_ui.c
        src/win/ui_draw.c src/win/darkwatch.rc)
    set_source_files_properties(src/win/cpu_check.c PROPERTIES COMPILE_OPTIONS "-march=x86-64;-mtune=generic")
    # The window is the host's (a Vulkan swapchain), not the toolkit's GDI one.
    target_compile_definitions(xbox_video PRIVATE XBOXRECOMP_HOST_WINDOW)
else()
    set(HOST_SOURCES src/main_posix.c)
endif()

add_executable(${PROJECT_NAME}
    ${HOST_SOURCES}
    src/recomp_manual.c
    src/dw_patches.c src/dw_nettrace.c
    ${RECOMP_GEN_SOURCES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${DW_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE ON)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/win
        ${XBOXRECOMP_DIR}/src/input ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi dxguid xinput1_4 winmm dbghelp
        user32 gdi32 shell32 ole32 comdlg32 uuid)
endif()

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
else()
    # Generated code: 2.8 M lines. -O1 keeps compile memory and time sane;
    # warnings off for gen/ only would be nicer, but the files are uniform.
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-O1;-w;-fno-strict-aliasing;-fwrapv")
    if(WIN32)
        # Static winpthreads and a 16 MB default stack reservation (guest
        # threads recurse through lifted code on the native stack).
        target_link_libraries(${PROJECT_NAME} PRIVATE -Wl,-Bstatic pthread -Wl,-Bdynamic)
        target_link_options(${PROJECT_NAME} PRIVATE -Wl,--stack,16777216 -static-libgcc
            -Wl,-Map=${CMAKE_BINARY_DIR}/darkwatch.map)
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE m pthread dl)
        # Names in the fault handler's host backtrace.
        target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
    endif()
    # src/dw_patches.c src/dw_nettrace.c: wrapped lifted functions.
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=sub_003646A1 -Wl,--wrap=sub_001AB6A0 -Wl,--wrap=sub_0040E5B2 -Wl,--wrap=sub_0040EA44 -Wl,--wrap=sub_0040EAA6 -Wl,--wrap=sub_0040EB19 -Wl,--wrap=sub_0040EA9A -Wl,--wrap=sub_003A9841 -Wl,--wrap=sub_003485D0 -Wl,--wrap=sub_00348680 -Wl,--wrap=sub_0035711B -Wl,--wrap=sub_0035718A -Wl,--wrap=sub_00357195 -Wl,--wrap=sub_003571A0 -Wl,--wrap=sub_003571B6 -Wl,--wrap=sub_003571C1 -Wl,--wrap=sub_003571D0 -Wl,--wrap=sub_003571DB -Wl,--wrap=sub_00357200 -Wl,--wrap=sub_00357229 -Wl,--wrap=sub_00357247 -Wl,--wrap=sub_00357154 -Wl,--wrap=sub_0035717F -Wl,--wrap=sub_00357163 -Wl,--wrap=sub_00357110 -Wl,--wrap=sub_00357105 -Wl,--wrap=sub_001E8C26 -Wl,--wrap=sub_000C5F6D -Wl,--wrap=sub_000C59A4)
endif()

# A binary built on a newer distro that still runs on glibc 2.35 (Ubuntu
# 22.04, the WSL/Cowork VM). Configure with a static-only SDL2 in
# CMAKE_PREFIX_PATH so nothing newer comes in through a shared library.
# Vulkan render back end for the pushbuffer executor (src/gpu). Loaded at run
# time through vkGetInstanceProcAddr, so there is no link-time dependency on a
# Vulkan library; DW_RENDERER=vulkan turns it on.
target_sources(${PROJECT_NAME} PRIVATE src/gpu/nv2a_vulkan.c)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers
    ${XBOXRECOMP_DIR}/src/kernel)

option(DW_PORTABLE "glibc 2.35 compatible binary (Linux)" OFF)
if(DW_PORTABLE AND UNIX AND NOT APPLE)
    target_sources(${PROJECT_NAME} PRIVATE src/glibc_compat.c)
    target_link_options(${PROJECT_NAME} PRIVATE
        -Wl,--wrap=fmod -Wl,--wrap=fmodf -static-libgcc)
endif()
