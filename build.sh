#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
trap 'rm -f clip-bridge.new' EXIT
for protocol in ext-data-control wlr-data-control; do
    case "$protocol" in
        ext-data-control) xml=ext-data-control-v1.xml ;;
        wlr-data-control) xml=wlr-data-control-unstable-v1.xml ;;
    esac
    wayland-scanner client-header "protocols/$xml" "protocols/$protocol.h"
    wayland-scanner private-code "protocols/$xml" "protocols/$protocol.c"
    cc -O2 -fPIC $(pkg-config --cflags wayland-client) -c "protocols/$protocol.c" -o "protocols/$protocol.o"
done
# Link only the used C++ support into the executable; no shared C++ runtime.
# Section collection and LTO keep unused library facilities out of the binary.
g++ -std=c++23 -Os -flto -ffunction-sections -fdata-sections -fno-rtti \
    -Wall -Wextra -Wpedantic -static-libstdc++ -static-libgcc \
    $(pkg-config --cflags wayland-client) main.cpp copy.cpp paste.cpp terminate.cpp protocols/ext-data-control.o protocols/wlr-data-control.o \
    -Wl,--gc-sections -Wl,--as-needed -o clip-bridge.new $(pkg-config --libs wayland-client)
mv -f clip-bridge.new clip-bridge
