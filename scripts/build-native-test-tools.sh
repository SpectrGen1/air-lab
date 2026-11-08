#!/bin/sh
set -eu
lab_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_build="$lab_root/.local/build/native-tests"
mkdir -p "$test_build"
# Protocol XML is generated locally, with upstream copyright headers retained.
python3 - "$lab_root" <<'PY'
from pathlib import Path
import sys,urllib.request
root=Path(sys.argv[1]);upstream=root/'.local/upstream';upstream.mkdir(parents=True,exist_ok=True)
files={
'wlr-foreign-toplevel-management-unstable-v1.xml':'https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/1106a4a1c57562ddd438c547da4c0d6dc150d624/unstable/wlr-foreign-toplevel-management-unstable-v1.xml',
'virtual-keyboard-unstable-v1.xml':'https://gitlab.freedesktop.org/wlroots/wlroots/-/raw/0.20.2/protocol/virtual-keyboard-unstable-v1.xml',
'wlr-virtual-pointer-unstable-v1.xml':'https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/1106a4a1c57562ddd438c547da4c0d6dc150d624/unstable/wlr-virtual-pointer-unstable-v1.xml'}
for name,url in files.items():
 path=upstream/name
 if not path.exists():
  with urllib.request.urlopen(url,timeout=30) as response:path.write_bytes(response.read())
PY
for name in virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1 wlr-foreign-toplevel-management-unstable-v1; do
    wayland-scanner client-header "$lab_root/.local/upstream/$name.xml" "$test_build/$name-protocol.h"
    wayland-scanner private-code "$lab_root/.local/upstream/$name.xml" "$test_build/$name-protocol.c"
done
protocol_dir=$(pkg-config --variable=pkgdatadir wayland-protocols)
wayland-scanner client-header "$protocol_dir/staging/ext-session-lock/ext-session-lock-v1.xml" "$test_build/ext-session-lock-v1-protocol.h"
wayland-scanner private-code "$protocol_dir/staging/ext-session-lock/ext-session-lock-v1.xml" "$test_build/ext-session-lock-v1-protocol.c"
cc -std=c11 -Wall -Wextra -I "$test_build" "$lab_root/native/tests/control.c" \
 "$test_build/virtual-keyboard-unstable-v1-protocol.c" "$test_build/wlr-virtual-pointer-unstable-v1-protocol.c" \
 $(pkg-config --cflags --libs wayland-client xkbcommon) -o "$test_build/air-test-control"
cc -std=c11 -Wall -Wextra -I "$test_build" "$lab_root/native/tests/lock.c" \
 "$test_build/ext-session-lock-v1-protocol.c" $(pkg-config --cflags --libs wayland-client) -o "$test_build/air-test-lock"

cc -std=c11 -Wall -Wextra -I "$test_build" "$lab_root/native/tests/disconnect.c" \
 "$test_build/wlr-foreign-toplevel-management-unstable-v1-protocol.c" \
 $(pkg-config --cflags --libs wayland-client) -o "$test_build/air-test-disconnect"
