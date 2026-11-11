# Native setup

Air Lab targets the original XREAL Air with labwc. Use [NATIVE.md](NATIVE.md)
for build, validation, session installation, and recovery commands.

## Tracker build

The tracker uses pinned [XRLinuxDriver](https://github.com/wheaney/XRLinuxDriver)
sources. Build dependencies include Git, a C/C++ toolchain, CMake, Ninja,
Rust/Cargo, pkg-config, PyYAML, and development files for libusb, libudev,
libevdev, OpenSSL, json-c, libcurl, and Wayland.

```sh
sh scripts/build-tracker.sh
sh scripts/check-tracker.sh
```

The build compiles the upstream XREAL One Rust library and applies the patches
required by the original Air integration. The timestamp regression checks
startup and stream gaps against the actual built sensor integration.

The native session owns the tracker. Stop that session before starting another
IMU reader or head-to-mouse service. Tracker settings and logs are under
`.local/tracker/`. The tracker sends OpenTrack UDP to loopback. Its launcher
disables metrics, the OpenTrack listener, and Gamescope integration.

## Hardware checks

```sh
lsusb -d 3318:0424
kanshictl status
wlr-randr
```

Check that the original Air appears as an extended output and that USB data is
available through the same connection. Connector names and supported modes vary
with hardware and cabling. The native session log identifies the compositor
release in use.
