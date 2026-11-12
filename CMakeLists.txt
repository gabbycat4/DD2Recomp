# Double Dragon II (USA Rev 1) on the NESRecomp cycle backend. See README.md.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ./build/dd2recomp dd2.nes
#
# Your own ROM: dd2.nes beside this file, or -DDD2_ROM=/path/to/rom.nes.
# Headless (scroll logging, CI):  add -DNESRECOMP_HEADLESS=ON, separate build dir.
cmake_minimum_required(VERSION 3.20)
project(DD2Recomp C)

# The engine: the nesrecomp submodule in this repository, unless you point
# NESRECOMP_ROOT at another checkout.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nesrecomp/runner/cyc/project.cmake")
    set(_dd2_engine "${CMAKE_CURRENT_SOURCE_DIR}/nesrecomp")
else()
    set(_dd2_engine "$ENV{HOME}/Downloads/nesrecomp")
endif()
set(NESRECOMP_ROOT "${_dd2_engine}" CACHE PATH "nesrecomp engine checkout")
if(NOT EXISTS "${NESRECOMP_ROOT}/runner/cyc/project.cmake")
    message(FATAL_ERROR "NESRECOMP_ROOT=${NESRECOMP_ROOT} is not a nesrecomp checkout "
                        "(run: git submodule update --init --recursive)")
endif()
set(DD2_ROM "${CMAKE_CURRENT_SOURCE_DIR}/dd2.nes" CACHE FILEPATH "Double Dragon II (USA) (Rev 1) ROM")
if(NOT EXISTS "${DD2_ROM}")
    message(FATAL_ERROR "No ROM at ${DD2_ROM}: put your Double Dragon II (USA) (Rev 1) dump there as dd2.nes, "
                        "or pass -DDD2_ROM=/path/to/rom.nes")
endif()
option(NESRECOMP_HEADLESS "Build without SDL window/audio support" OFF)

include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")

set(dd2_options "")
if(NESRECOMP_HEADLESS)
    list(APPEND dd2_options HEADLESS)
endif()

# Relative paths are relative to this directory. seeds.txt lists ROM code
# addresses the recompiler should compile (no game data). capture.txt (code
# the game copies into RAM, which holds bytes of the game) is local only: make
# it with tools/capture.sh; without it that code runs on the interpreter.
set(dd2_capture "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/capture.txt")
    set(dd2_capture CAPTURE_FILE "capture.txt")
endif()
nesrecomp_add_cycle_game(dd2recomp
    ROM          "${DD2_ROM}"
    GAME_CONFIG  "game.toml"
    SEED_FILE    "seeds.txt"
    ${dd2_capture}
    PLAYERS 2
    REGION NTSC
    HOST_EXTRAS  "src/dd2_extras.c" "src/dd2_smooth.c"
    ${dd2_options})

if(UNIX)
    target_link_libraries(dd2recomp PRIVATE m)   # lround in dd2_smooth.c
endif()
