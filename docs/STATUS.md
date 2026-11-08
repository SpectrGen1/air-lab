# Baseline and design review

## Imported source

Imported 2026-11-07 from the user-supplied `air-spatial-lab.zip`, unpacked into
the repository root without a redundant nested project. The root `DESIGN.md`
was byte-identical to the archived copy and was preserved.

Archive SHA-256:
`26858da037c2d209db65e64ac80cda1ed580e36d0b701f2d9e5572beb91c0018`.
Original archive retained locally at `.local/archive/air-spatial-lab.zip`.
The external os-agent deployment README was reviewed as the earlier hardware
baseline. Its historical results were rechecked rather than assumed current.

## Confirmed on this workstation

- Rust probe: 1,000 finite accelerometer/gyro samples, increasing device timestamps,
  1,000.0 Hz device rate; ordinary same-image-on-both-eyes display mode.
- Air is a separate extended output at 1080p/60, with both monitors still enabled.
- Active Wayland session advertises foreign-toplevel list, toplevel capture source,
  image-copy capture, and output-power management interfaces. `grim -T` captured
  a real sacrificial foot window. No compositor upgrade was needed.
- Pinned local XRLinuxDriver release build succeeds; 100 valid 52-byte UDP pose
  packets received over 0.099 seconds after calibration. This is a short packet
  throughput observation, not a latency or long-term stability benchmark.
- Three original bridge tests pass (packet formats, rejection, HTTP/capture/SSE).
- Headless Chromium check rendered the real terminal capture, received synthetic
  UDP poses, and showed stale-feed status. Simulated and UDP head turns preserved
  the panel hit coordinates; recentering preserved the pointer. No JavaScript
  errors occurred. Temporary harness and screenshot are under `.local/`.
- Simulator launcher served its config endpoint; terminating the launcher exited
  cleanly and stopped the child HTTP server.
- User confirmed the live head-tracked preview works in the glasses.
  The user then reported a continuous full-circle drift, so that confirmation
  establishes connectivity only, not stable tracking.

## Drift investigation

With the user confirming the Air was motionless on a table, the initial driver
reported approximately -189.47° yaw change in 30 seconds, with large pitch and
roll changes. This was in the incoming pose, before browser projection.

Inspection found that `xrDeviceKit` integrated the first timestamp difference
and stream gaps without a bound. The local patch rebases the timestamp before
AHRS integration at startup, after gaps over 100 ms, and after non-increasing
timestamps. A regression test against the actual sensor integration fails with
the original code and passes with the patch. It also verifies that real slow
turns are preserved; the browser does not subtract a guessed yaw rate.

Gyro bias correction also needs more time stationary than the driver's initial
15-second ready indication. The launcher now leaves a 60-second calibration
warmup before opening the browser. Recenter alone is not bias calibration.
During the patched 45-second startup experiment, final ten-second stationary
changes were yaw +0.0398°, pitch +0.0357°, roll +0.1322°. Earlier startup packets
included a reference reset, reinforcing the need to wait before opening the
preview. This short test does not demonstrate drift-free long-term tracking.

After restarting with the timestamp patch and full stationary warmup, the user
put the glasses back on, looked around, recentered, and confirmed that the view
stays steady and the continuous rotation is gone. Longer drift/ergonomics tests
remain part of design gate 1.

## Review findings and next gates

The supplied prototype is suitable for the first interaction experiment. The
camera basis is separate from the cylinder pointer and panel picking. Its
bridge captures only explicitly selected windows and accepts no input-injection
commands. PNG capture is deliberately slow and is not the planned renderer.

The main setup mismatch was that the archived README assumed an installed
XRLinuxDriver while the workstation only had a raw-sensor Rust probe. The local
build and launch scripts now bridge that gap without a service installation.

## Native integration added 2026-11-07

The implementation now includes an optional labwc 0.20.3 patch, native EGL/SHM
presenter, private inherited IPC/Wayland connections, compositor picking and seat
routing, per-workspace panels, transient groups, cursor image transport, mode
transactions, output power snapshots and recovery, and lock teardown. See
`docs/NATIVE.md` for exact supported behavior and remaining feature gates.

The latest disposable two-output regression run passed real GTK editor
clicks/typing, animation with its flat output asleep, head-only input independence,
offscreen click protection, unbounded selection/release, scale 1.25, transient
dialogs, lock blanking/re-entry, frozen-helper escape with one matching release,
stale feed recovery, hung helper recovery, and simulated output disconnect without
killing the editor. Six Python bridge/launcher tests, the tracker timestamp
regression, and the native geometry test pass. Enabled/disabled labwc feature
builds are checked. Evidence and binary hashes are in `.local/native-check/`.

The login launcher is installed with frozen local binaries and a stock
fallback. After reboot on 2026-11-08, the installed compositor was verified running.
The first physical preview showed only its status line: NVIDIA GLES advertised
24-bit BGR888 SHM captures, while the presenter only accepted four-byte formats.
The fix supports three-byte pixels and padded row strides, with color/alpha
regressions and a full passing GLES integration run in addition to Vulkan.
The renderer-only update was applied to the running session's next helper spawn;
the compositor and desktop applications were preserved. The user confirmed that
windows now appear in the glasses after recentering. This establishes physical
native visibility, not completion of input/power/recovery acceptance. Actual Air
sleep/wake/unplug, ordinary workloads, long-running video, ten-minute drift and
optical readability still need physical acceptance after that login. Do not
convert the nested test's results into claims of physical hardware validation.

Server-side decorations and several advanced input/XWayland cases are remaining
gates, with explicit desktop fallbacks for the currently recognized unsupported
gestures. DMA-BUF optimization, stereo and DRM lease are separate later work.
No remote or push destination has been configured; project licensing still needs
selection before distribution. Upstream dependencies retain their licenses.
