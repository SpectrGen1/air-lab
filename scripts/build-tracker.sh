#!/bin/sh
# Build the tested upstream tracker locally; no installation or services.
set -eu
lab_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tracker_src="$lab_root/.local/upstream/XRLinuxDriver"
tracker_rev=da303d2bc594bb5bf8a9123c6203f81f5869d955
python3 -c 'import yaml' || { echo 'Build requires PyYAML in the selected Python.' >&2; exit 1; }
mkdir -p "$lab_root/.local/upstream"
if [ ! -d "$tracker_src/.git" ]; then
    git clone --no-checkout https://github.com/wheaney/XRLinuxDriver.git "$tracker_src"
    git -C "$tracker_src" checkout --detach "$tracker_rev"
fi
if [ "$(git -C "$tracker_src" rev-parse HEAD)" != "$tracker_rev" ]; then
    echo "Tracker checkout differs from tested revision $tracker_rev; inspect it manually." >&2
    exit 1
fi
git -C "$tracker_src" submodule update --init --recursive
fusion_src="$tracker_src/modules/xrDeviceKit"
fusion_patch="$lab_root/patches/xrdevicekit-timestamp-rebase.patch"
if git -C "$fusion_src" apply --reverse --check "$fusion_patch" 2>/dev/null; then
    echo 'Timestamp rebase patch already applied.'
else
    git -C "$fusion_src" apply --check "$fusion_patch"
    git -C "$fusion_src" apply "$fusion_patch"
fi
# Upstream CMake references this static library but does not build it for Ninja.
cargo build --locked --release --manifest-path "$tracker_src/modules/xrealOneDeviceKit/modules/xreal_one_driver/Cargo.toml"
cmake -S "$tracker_src" -B "$lab_root/.local/build/xr-driver" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$lab_root/.local/build/xr-driver" -j 8
