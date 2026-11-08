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

On 2026-11-08 the user reported severe drift again in native mode despite several
minutes on a table. A stationary measurement of the session-owned UDP feed showed
+294.887° unwrapped yaw over 35 seconds, before native projection. Runtime
inspection showed a nonzero startup calibration and FusionOffset's timer stuck
at zero with no learned residual correction. XREAL connection unconditionally
calls `device_imu_calibrate(..., 1000, true, true, false)`: it averages the first
second without verifying stationarity. Motion captured there can produce an
error above the later estimator's 3°/s stationary threshold, preventing recovery.

Disabling and re-enabling the existing session reader while the user kept the
Air resting reset that poisoned calibration. Another 35-second measurement
showed yaw +0.1208° (range 0.1557°), pitch +0.0954°, roll -0.0273°. Evidence is
`.local/drift/native-stationary.json` and `recalibrated-stationary.json`.
The new startup patch preserves factory calibration and the stationary estimator
instead of running the blind average. Its regression compiles the actual
connection function and fails against original upstream. The old reader then
reconnected, ran its bad calibration again, and the user reported slower drift
and upside-down panels; another recenter restored upright viewing.

The fixed reader was subsequently started in the existing desktop using the
tracked handoff monitor, after terminating the old reader. Its actual factory
calibration remained unchanged, and the stationary estimator learned a small
residual bias. A fresh 35-second stationary measurement showed yaw -0.00878°
(range 0.01788°), pitch +0.00351° (range 0.02380°), and roll -0.00021° (range
0.01347°). Evidence is `.local/drift/factory-preserved-stationary.json`. No second
IMU reader was active. The monitor's disposable-process test verifies that the
replacement stops when the compositor exits while unrelated clients survive.
The new renderer also exits on a tracker sequence/reference reset rather than
continuing with a stale neutral frame. Long-term drift and movement/reconnect
hardware acceptance still require separate checks.

With the fixed reader running, the user put the Air on, entered the visible
preview, recentered upright, looked around, then held still for 20–30 seconds and
confirmed the windows were upright and steady. This is a short physical
acceptance result, not a ten-minute drift benchmark.

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
killing the editor. Eight Python bridge/capture/launcher/handoff tests, the tracker timestamp
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

## Size, session duration and horizon feedback

On 2026-11-08 the user reported panels about 50% too large, short preview/active
sessions, and sideways window tilt during horizontal head turns. The new default
panel scale is 2/3 of the earlier physical width/height, applied in the compositor
to root panels and transients so rendering/picking agree. Canonical desktop
rectangles remain unchanged. A user-guided forward/left comparison showed mostly
yaw with several degrees of reported roll. The new default camera deliberately
removes relative roll while preserving the forward vector; full roll is an
opt-in preference, not a claimed sensor-axis calibration.

Healthy previews previously hit an unconditional two-minute ARMING deadline.
The revised deadline covers startup only. Actual presented-frame liveness is
separate from a complete scene acknowledgment, so recapturing a window revokes
click eligibility without pretending that the renderer has stopped. Source review
also found the presenter waiting for `wl_buffer.release` after capture: the
image-copy protocol explicitly does not use that event. Ready/failed now complete
buffer ownership, allowing resize-constraint failures to reallocate/retry. A
hardware-free test compiles those actual completion handlers; it fails against
the prior implementation. Pose/presentation loss watchdogs remain enabled and
now log distinct causes. These compositor changes require a new login; the old
running session is not hot-replaced.

The NVIDIA GLES integration run kept a healthy preview open for 130 seconds,
then active mode for another 130 seconds with its other output asleep. Original
editor input, animation, head/pointer independence, lock, escape, lost tracking,
reference reset, frozen renderer and output disconnect all passed afterward.
Evidence is `.local/native-check/1794156937630873426/`. This demonstrates the
removed duration limit in a nested session, not physical long-session acceptance
of the new defaults. The current production compositor remains the earlier
binary until the user saves work and logs in again.
