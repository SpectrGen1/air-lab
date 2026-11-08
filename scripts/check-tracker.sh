#!/bin/sh
set -eu
lab_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tracker_src="$lab_root/.local/upstream/XRLinuxDriver/modules/xrDeviceKit"
tracker_build="$lab_root/.local/build/xr-driver/modules/xrealAirDeviceKit/xrDeviceKit"
cc -std=c17 -O2 -I "$tracker_src/include" -I "$tracker_src/modules/Fusion" \
    "$lab_root/scripts/check-tracker.c" "$tracker_build/libxrDeviceKit.a" \
    "$tracker_build/modules/Fusion/Fusion/libFusion.a" \
    $(pkg-config --cflags --libs json-c) -lm -o "$lab_root/.local/check-tracker"
"$lab_root/.local/check-tracker"
