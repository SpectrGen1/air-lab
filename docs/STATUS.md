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
killing the editor. Nine Python bridge/capture/launcher/handoff tests, the tracker timestamp
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

## Origin-facing panels and spatial frame controls

The requested controls are implemented in labwc and the native presenter with
private protocol version 2. Elevated panels now tilt toward the fixed viewer
origin as well as facing inward horizontally. Shared plane math handles separate
horizontal/vertical physical scales, capture offsets and full 3D picking.

Each captured surface group has an outer spatial title bar and resize frame.
Dragging the bar moves the root group along the cylinder; wheel movement during
that grab changes radial distance, retaining the held point on the mouse ray.
Edges change one physical span, corners change both about the centre. Original
desktop rectangles and client resolution are retained. Transients move with the
root and can have their own size. Poses persist within the current compositor.
The outer frame bounds include popup/subsurface extensions and shadows, keeping
controls outside captured application input. Consumed buttons/wheel events are
not sent to clients, and an owned release is swallowed after cancellation,
target destruction or force-desktop recovery.

The full NVIDIA GLES run `.local/native-check/1794159295104007535/` and Vulkan run
`.local/native-check/1794159316509777732/` passed edge/corner resize, translation,
both depth directions, head-only independence while held, unchanged desktop
rectangles, forced-exit release handling, and destruction of a held second view
while the original editor survived. Existing input, animation, fractional scale,
modal, lock, stale-feed, reference-reset, frozen-helper and output-disconnect
regressions also passed. The geometry test includes elevated/depth-shifted planes,
unequal scales, frame bounds with negative popup origins and off-centre depth
grabs. Feature-enabled and feature-disabled labwc builds pass without warnings.
Physical use of these new controls still requires the new compositor login;
nested results do not establish optical usability or physical acceptance.

## Mouse projection and gain

The user found pointer motion warped/uneven/too fast with the installed spatial
controls build. Its fixed .0012 radians per motion unit was approximately 3.16
viewport pixels/unit at 1920 pixels and 40° horizontal FOV. Vertical movement
used linear cylinder height, making angular response vary with height. The new
mapping derives gain from viewport width (approximately one pixel/unit at the
centre), integrates angular elevation and compensates horizontal travel for
elevation. It retains normal device acceleration and never consumes head pose.
Splitting the same diagonal input into smaller packets produces the same path,
including at the vertical limit. Speed can be adjusted live with Super+Alt+Minus/
Equal and reset with Super+Alt+0.

The empty-space hotspot now sits on the actual cylinder; previously the ray was
multiplied by the cylinder radius, placing it on a sphere. The cursor glyph now
keeps a constant viewport pixel size, rather than expanding/shrinking with
target depth. Rendering still anchors it at the compositor's actual world hit.
Viewport changes refresh gain/aspect and require a newly presented scene before
clicks. This does not make pointing depend on head motion or rewrite client
coordinates to mask an inconsistent visual pointer. Physical feel with these
new defaults remains to be checked after the new compositor login.

Both `.local/native-check/1794160431501248068/` (NVIDIA GLES) and
`.local/native-check/1794160453573167858/` (Vulkan) passed the expected-editor-pixel
gain check and live speed adjustment, along with the existing head/pointer,
capture, frame manipulation, cancellation and recovery tests. Geometry tests
cover packet splitting through vertical limits, true cylinder positions and
cursor pixel scale across depths. The matching release is staged for the next
login; the currently running compositor has not been replaced.


## Ready notifications and right-click menus

The session supervisor now emits one warmup notice and one ready notice per
connection, independent of whether a preview is requested. Readiness requires
connection, completed calibration, the full minute and the owned UDP source port.
The readiness test covers unchanged state crossing the deadline, absent ports,
no repeated ready notice and reconnects; the tracker handoff shares this logic.

Right-click testing exposed popup constraints using a sleeping output's usable
area and stale client coordinates after pointer summon. Spatial XDG popups now
use parent-window constraints, and seat motion is synchronized before a press
and after summon. Real GTK menu action selection, Escape dismissal and outside
click dismissal passed with the flat output asleep on both GLES and Vulkan,
with the existing input/manipulation/recovery suite. Test input now uses real
monotonic timestamps, including elapsed waits, so GTK menu timing is realistic.
Evidence: `.local/native-check/1794161349588093884/` (GLES) and
`.local/native-check/1794161373431958510/` (Vulkan). Physical app acceptance is
still pending the updated login; XWayland override-redirect menus are unvalidated.


## Wear/sleep warmup and persistent notifications

The user reported that the glasses start tracking when worn, sleep off-face on
the table, then reconnect and restart calibration when worn again. Driver logs
confirm reconnect/calibration cycles; no proximity event is exposed to our
supervisor, so disconnection is labeled “disconnected or asleep,” not assumed to
be a confirmed wear event. The supported instruction is now to keep them on and
hold the head still for the full minute. A real reconnect still resets readiness.
Factory calibration preservation and tracking-loss recovery remain intact.

The workstation's mako config also had `ignore-timeout=1`, forcing five-second
expiry even for persistent notices. Installation adds an app-specific exception
and backs up that config. Tracker notices now request persistence, replace their
previous notification by ID, replace stale readiness with disconnection guidance,
and retry failed delivery. Unit tests cover expiry requests, replacement IDs,
delivery retries, warmup/calibration/source-port gates and reconnects. Ten Python
checks and the tracker regressions pass; native binaries are unchanged and retain
their previous passing integration hashes. The selected release is
`.local/releases/c99544ac08835660`; the live handoff applied it without replacing
the compositor or desktop clients. The user kept the glasses on through warmup
and confirmed that “Glasses ready” appeared and stayed visible. The monitor log
also records successful warmup/ready notification delivery. This verifies the
notification workflow, not a new long-term drift or sleep/recovery acceptance.


The user subsequently preferred normal expiry for the ready notice. The Air-only
mako rule now lets “Glasses ready” expire after the usual five seconds, including
in the running handoff. Warmup/disconnection guidance still persists. No tracker
restart or new calibration is required for this notification-only change.
