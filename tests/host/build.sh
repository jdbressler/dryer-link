#!/bin/sh
# Builds tests/host/build/sim: the real bus component and the real dryer_link component, compiled for this computer
# against stand-ins for ESPHome (stubs/), with pretend dryers. Needs g++ with C++20 and python3.
#   ./build.sh && ./build/sim --test
set -e
cd "$(dirname "$0")"
rm -rf build
mkdir -p build/inc/esphome/components
ln -s "$(pwd)/../../components/gea" build/inc/esphome/components/gea
ln -s "$(pwd)/../../components/dryer_link" build/inc/esphome/components/dryer_link
python3 gen_page.py > build/page_gz.cpp
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I stubs -I build/inc -o build/sim \
  sim.cpp ../../components/gea/gea.cpp ../../components/dryer_link/dryer_link.cpp build/page_gz.cpp
echo "built build/sim"
