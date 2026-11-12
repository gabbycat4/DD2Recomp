#!/bin/sh
# Make capture.txt: the code Double Dragon II copies into RAM, recorded by
# running route.txt (the opening of Mission 1) headless. With it in place the
# next configure compiles that code too (100% native). Needs a built
# headless binary:
#   cmake -S . -B build-headless -DCMAKE_BUILD_TYPE=Release -DNESRECOMP_HEADLESS=ON
#   cmake --build build-headless
#   tools/capture.sh
set -e
cd "$(dirname "$0")/.."
ROM="${1:-dd2.nes}"
./build-headless/dd2recomp "$ROM" --frames 3000 --input route.txt --capture-log capture.txt
echo "capture.txt written; reconfigure your window build (cmake -S . -B build) to use it."
