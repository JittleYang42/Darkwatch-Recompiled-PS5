# Toolchain file for building darkwatch.exe with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw: clang, lld and mingw-w64, UCRT) --
# on Windows, Linux or macOS alike. Any clang + mingw-w64 setup that puts the
# same x86_64-w64-mingw32-* tool names on PATH works too.
#
#   cmake -S darkwatch_game -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=darkwatch_game/cmake/llvm-mingw.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(DW_MINGW_PREFIX "x86_64-w64-mingw32" CACHE STRING "Tool name prefix of the mingw-w64 toolchain")
set(CMAKE_C_COMPILER ${DW_MINGW_PREFIX}-clang)
set(CMAKE_RC_COMPILER ${DW_MINGW_PREFIX}-windres)
find_program(DW_AR NAMES ${DW_MINGW_PREFIX}-ar llvm-ar)
find_program(DW_RANLIB NAMES ${DW_MINGW_PREFIX}-ranlib llvm-ranlib)
if(DW_AR)
    set(CMAKE_AR ${DW_AR} CACHE FILEPATH "" FORCE)
endif()
if(DW_RANLIB)
    set(CMAKE_RANLIB ${DW_RANLIB} CACHE FILEPATH "" FORCE)
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
