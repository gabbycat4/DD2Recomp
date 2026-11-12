# Double Dragon II (USA Rev 1) on the NESRecomp cycle backend.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ./build/dd2recomp dd2.nes
#
# Headless (scroll logging, CI):  add -DNESRECOMP_HEADLESS=ON, separate build dir.
cmake_minimum_required(VERSION 3.20)
project(DD2Recomp C)

set(NESRECOMP_ROOT "$ENV{HOME}/Downloads/nesrecomp" CACHE PATH "nesrecomp engine checkout")
if(NOT EXISTS "${NESRECOMP_ROOT}/runner/cyc/project.cmake")
    message(FATAL_ERROR "NESRECOMP_ROOT=${NESRECOMP_ROOT} is not a nesrecomp checkout "
                        "(set -DNESRECOMP_ROOT=/path/to/nesrecomp)")
endif()
option(NESRECOMP_HEADLESS "Build without SDL window/audio support" OFF)

include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")

set(dd2_options "")
if(NESRECOMP_HEADLESS)
    list(APPEND dd2_options HEADLESS)
endif()

# Relative paths are relative to this directory. SEED_FILE / CAPTURE_FILE
# override the paths in game.toml.
nesrecomp_add_cycle_game(dd2recomp
    ROM          "dd2.nes"
    GAME_CONFIG  "game.toml"
    SEED_FILE    "seeds.txt"
    CAPTURE_FILE "capture.txt"
    PLAYERS 2
    REGION NTSC
    HOST_EXTRAS  "src/dd2_extras.c" "src/dd2_smooth.c"
    ${dd2_options})

if(UNIX)
    target_link_libraries(dd2recomp PRIVATE m)   # lround in dd2_smooth.c
endif()
