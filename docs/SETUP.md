# Browser baseline setup

Verified on `spectre`, Brian's Gentoo/OpenRC desktop, 2026-11-07.
For the subsequently implemented native desktop session, use [NATIVE.md](NATIVE.md).
Its session-owned tracker excludes a second `run-lab.py --pose` reader.
Commands run as the desktop user, without sudo.

## Files and dependencies

- `bridge.py`, `index.html`: original browser prototype; Python standard library only.
- `test_bridge.py`: synthetic UDP and stub capture tests.
- `scripts/run-lab.py`: launches the bridge, optional tracker and browser;
  stops its children on Ctrl+C, termination, or a child exit.
- `scripts/run-tracker.py`: selects UDP output with isolated project settings.
- `scripts/build-tracker.sh`: fetches and builds pinned upstream code into `.local/`.
- `DESIGN.md`: future native labwc implementation contract.

Runtime: Python 3, browser with pointer lock/fullscreen, `xdg-open`, `setsid`.
Window previews additionally need `grim -T` and the active compositor's
foreign-toplevel/image-copy capture interfaces.

Tracker build: Git, C/C++ toolchain, CMake, Ninja, Rust/Cargo, pkg-config,
PyYAML; development files for libusb, libudev, libevdev, OpenSSL, json-c,
libcurl and Wayland. All were already present. No packages were installed.

The tracker is [XRLinuxDriver](https://github.com/wheaney/XRLinuxDriver), version
2.11.9, revision `da303d2bc594bb5bf8a9123c6203f81f5869d955`, including pinned
submodules. The build script retains their existing licenses in `.local/upstream/`;
they are not vendored or committed into this repository.

## Build and run

```sh
cd /home/work/projects/air-lab
sh scripts/build-tracker.sh
python3 scripts/run-lab.py --pose
```

The build script explicitly builds the upstream XREAL One Rust static library
before CMake/Ninja. Without it, Ninja reports a missing
`libxreal_one_driver.a` even when using an original Air.

It also applies `patches/xrdevicekit-timestamp-rebase.patch` to the pinned
`xrDeviceKit` submodule. This rebases sensor timestamps at stream startup and
after gaps over 100 ms or non-increasing timestamps. Device uptime and gaps
must not be integrated as head motion. The patch is idempotent; unexpected
source changes fail its application rather than silently overwriting code.
`sh scripts/check-tracker.sh` tests the actual built integration with synthetic
timestamps and checks that a slow real turn still integrates.

The launcher starts its loopback UDP receiver before starting the tracker.
Keep the Air on and hold your head still for 60 seconds while the launcher warms
up the tracker. Taking them off may put them to sleep and restart tracking. Its initial UDP packets arrive earlier and do not mean bias
calibration has settled. The browser opens after this warmup. Select 40° on the Air output, fullscreen, look forward, R.
All clicks stay inside the browser prototype. Real application editing through
these panels is a future native milestone.

One reader must own the sensors: stop this lab before using the earlier Rust
probe, another XR driver, or any head-to-mouse service. The upstream driver
retains its normal runtime lock to exclude another `xrDriver` instance.
An independently written reader may not respect that lock.

The private URL changes on every start; the launcher prints it. Never commit
URLs or window captures. The initial session was opened in a terminal titled
`Air Spatial Lab - Ctrl+C to stop`, with a separate `Air Lab Capture Check`
terminal supplying harmless sample content. Closing that sample window makes
its preview unavailable; choose fresh IDs for a later session.

## Settings and network behavior

Tracker config, state and data are under `.local/tracker/{config,state,data}`.
Its generated config selects `external_only` + `opentrack`, UDP to
`127.0.0.1:4243`, disables metrics, the OpenTrack listener and Gamescope
integration. It does not select mouse/joystick output or request stereo.
The bridge binds HTTP and UDP only to loopback.

The upstream driver attempts a license HTTP request even in this UDP mode.
The wrapper directs libcurl HTTP(S) requests to the closed loopback port 9;
these requests fail locally. License connection errors in
`.local/tracker/state/xr_driver/driver.log` are expected; UDP tracking works
without the licensed features. This is a setting for this child process, not
a system firewall or a general network isolation guarantee. Upstream can also
create its ordinary runtime and IPC files outside this checkout.

## Hardware checks

```sh
lsusb -d 3318:0424
kanshictl status
wlr-randr
labwc --version
grim -h
```

Observed: original Air USB `3318:0424`; `dual-xreal`; Air DP-4 at 1920×1080/60 Hz,
position 6880,0, scale 1; Dell DP-2 at 0,0 and LG DP-3 at 3440,0, both enabled.
These are observations, not connector constants. Recheck after reconnection.
The active labwc reports 0.20.2 with wlroots 0.20.2.

`wayland-info` and `lswt` are absent. Protocol availability was checked on the
active Wayland socket using `WAYLAND_DEBUG=client grim -T <invalid-id> -`, then
a real `grim -T` capture of a sacrificial foot window succeeded. Such debug
logs can contain other window titles; keep them in ignored local storage.
For routine listing, an optional local build of upstream `lswt` can be added
later; it was not necessary for this setup.

Earlier raw-sensor probe (stop the lab first):

```sh
timeout 30s /home/brian/projects/os-agent/deploy/xreal-air/target/release/xreal-air-probe
```

Its source, pinned dependency and existing permissions/display setup are
documented in `/home/brian/projects/os-agent/deploy/xreal-air/README.md`.
The probe is an external diagnostic, not a dependency of this repo's preview.
The existing `/etc/udev/rules.d/70-xreal-air.rules` and kanshi `dual-xreal`
profile were sufficient; neither was changed here. Brian already belongs to
`usb` and `plugdev`.

## Recovery

Esc releases browser pointer lock. Ctrl+C in the launcher terminal stops its
bridge and tracker; close the browser tab afterward. A stale pose holds the
last view and labels it `POSE STALE`. If UDP port 4243 is occupied, stop the
earlier lab or use `--udp-port` with another free port; never kill unrelated
readers blindly.

No boot service, autostart entry, package, display rule, or desktop keybinding
was installed. There is no session configuration to roll back. The existing
glasses display profile remains as it was before this task. The preview does
not implement lock-screen privacy or native monitor recovery; keep physical
monitors on and stop the preview before sensitive work.

## Validation

```sh
python3 -m unittest -v
sh -n scripts/build-tracker.sh
python3 -m py_compile bridge.py scripts/run-lab.py scripts/run-tracker.py
sh scripts/check-tracker.sh                 # after building the tracker
```

Browser and physical checks recorded in `STATUS.md` must be repeated when
changing projection, picking, pose handling, capture or the tracker revision.
