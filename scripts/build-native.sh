#!/bin/sh
# Build local native binaries; no installation or session replacement.
set -eu
lab_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
labwc_src="$lab_root/.local/upstream/labwc"
labwc_rev=e0a3970b0c70e321814e8004ff7c6dbc7aeaf884
mkdir -p "$lab_root/.local/upstream"
if [ ! -d "$labwc_src/.git" ]; then
    git clone --branch 0.20.3 --depth 1 https://github.com/labwc/labwc.git "$labwc_src"
fi
if [ "$(git -C "$labwc_src" rev-parse HEAD)" != "$labwc_rev" ]; then
    echo "Unexpected labwc revision; expected $labwc_rev" >&2
    exit 1
fi
cp "$lab_root/native/protocols/air-widget-v1.xml" "$labwc_src/protocols/"
native_patch="$lab_root/patches/labwc-native-spatial.patch"
if git -C "$labwc_src" apply --reverse --check "$native_patch" 2>/dev/null; then
    echo 'Native integration patch already applied.'
else
    git -C "$labwc_src" apply --check "$native_patch"
    git -C "$labwc_src" apply "$native_patch"
fi
cp "$lab_root"/native/labwc/*.c "$labwc_src/src/"
cp "$lab_root"/native/labwc/*.h "$lab_root"/native/common/air-*.h "$labwc_src/include/"
if [ -f "$lab_root/.local/build/labwc/build.ninja" ]; then
    meson setup --reconfigure "$lab_root/.local/build/labwc" "$labwc_src" --buildtype=debugoptimized -Dair-spatial=enabled -Dman-pages=disabled -Dsystemd-session=disabled
else
    meson setup "$lab_root/.local/build/labwc" "$labwc_src" --buildtype=debugoptimized -Dair-spatial=enabled -Dman-pages=disabled -Dsystemd-session=disabled
fi
ninja -C "$lab_root/.local/build/labwc" -j 8
if [ -f "$lab_root/.local/build/air-native/build.ninja" ]; then
    meson setup --reconfigure "$lab_root/.local/build/air-native" "$lab_root/native" --buildtype=debugoptimized -Dlayer_protocol="$labwc_src/protocols/wlr-layer-shell-unstable-v1.xml"
else
    meson setup "$lab_root/.local/build/air-native" "$lab_root/native" --buildtype=debugoptimized -Dlayer_protocol="$labwc_src/protocols/wlr-layer-shell-unstable-v1.xml"
fi
ninja -C "$lab_root/.local/build/air-native" -j 8
meson test -C "$lab_root/.local/build/air-native" --print-errorlogs
