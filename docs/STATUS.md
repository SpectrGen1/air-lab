# Baseline and design review

The browser study was removed. Its results below are historical.

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
Only corners resize, scaling both physical spans uniformly about the centre
while preserving aspect ratio, including at the size limits. Straight edges
do not resize. Original
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


## Final notification/warmup preference

The user requested no Air notifications and no added one-minute delay. The
supervisor and handoff no longer notify or add a warmup deadline, and the
presenter ignores legacy deadlines in the running session's readiness file.
It still requires connection, initial driver calibration and the child-owned
UDP source port; source validation and tracking-loss recovery are unchanged.
The browser launcher also opens immediately. The driver's approximately
15-second initialization is separate from the removed extra minute.

The installer replaced the Air-only mako rule with suppression for old running
supervisors. The tested helper was redirected live with identical compositor
hashes, so the next toggle uses this policy without a logout or tracker restart.
Eight Python tests pass, including inherited private IPC with a legacy readiness
deadline one hour in the future. No native binaries changed. Selected release:
`.local/releases/cea75bfc3df9707e`. Earlier notification acceptance above is
historical and no longer describes the requested behavior.

## Corner-only proportional resizing (2026-11-08)

Spatial resizing now starts only at the four outer corners. Straight border
clicks are consumed without changing panel size or starting a desktop resize;
the title bar retains movement/depth controls. Corner drags project mouse motion
onto the size diagonal and apply a shared scale factor to both physical spans.
The shared factor also enforces the 0.12–3.5 metre span limits, preserving aspect
ratio through off-diagonal motion and limit clamping. Canonical client rectangles
remain unchanged.

Geometry regressions cover all corners, inert borders, axis-only/off-diagonal
motion and size limits. Python tests, tracker checks, native geometry/pixel tests,
and feature-enabled/disabled builds pass. The Vulkan nested integration run
`.local/native-check/1794166748417575199/` and NVIDIA GLES run
`.local/native-check/1794166784332643171/` pass all four corner gestures, inert
side/bottom drags, unchanged client rectangles and the existing lifecycle/input
checks. Tested release `b1e01e52c9161211` is staged for the next login; the running
compositor was not restarted. Physical acceptance still requires login into the
updated compositor.

## Automatic preview on display connection (2026-11-08)

After the user's logout/login, `/proc/529435/exe` identifies the running
corner-resize compositor from release `b1e01e52c9161211`. Live output enumeration
identifies the extended original Air as DP-4 (`Nreal`, `nreal air`).

The new compositor workflow opens preview when the identified glasses display
appears, including an already-connected display at startup. A 500 ms debounce
and idle-input gate precede the normal preview entry path. Tracker connection,
initial calibration, owned source port and healthy scene gates remain intact.
The first input/monitor-sleep entry still requires Super+Alt+Return. After that,
a new connection can enter automatically. Removal uses the existing compositor
restoration path. Manual exit, lock and failures suppress automatic attempts for
that connection, preserving explicit re-entry after unlock. No extra USB reader,
public input injection, notifications or stationary warmup was added.

`auto_start` defaults true in the installed-session manifest and is passed by
the supervisor to the compositor. Setting it false selects manual-only operation
at the next login. This update changes the compositor and requires another
saved-work logout/login; the current desktop is never restarted by installation.

Policy regressions cover debounce/contact bounce, busy input, cancellation,
locking/unlocking and reconnection. The automatic nested Vulkan run
`.local/native-check/1794167217471515505/` passes startup preview without a
shortcut, tracker readiness, first confirmation, manual override, display
removal/reappearance and original-client survival, plus existing native checks.
The reappearance fixture creates/removes a named headless output only within the
uniquely marked test compositor. Production still requires the original Air's
USB/model identity and an extended layout. Physical put-on/take-off display-event
behavior remains unverified; the workflow follows display presence as requested.

Python tests, tracker regressions, geometry/pixel tests and enabled/disabled
builds pass. Manual-only NVIDIA GLES integration also passes in
`.local/native-check/1794167273872124388/`.
Tested release `110381fa548e7bd0` is staged for the next login with
`auto_start: true`; the running compositor and tracker were not restarted.

## Fully automatic entry; floating Waybar design (2026-11-08)

The user explicitly removed the first-entry confirmation. ARMING now enters
SPATIAL automatically as soon as fresh tracking, renderer readiness and a complete
presented scene are available. No Super+Alt+Return step remains, including after
a new login or manual Super+Alt+X start. Existing mouse-button/constraint guards,
watchdogs, manual-exit suppression and lock/recovery rules are retained. Startup
status no longer asks for confirmation. The earlier confirmation behavior above
is historical and superseded by this request.

The automatic Vulkan regression `.local/native-check/1794167653600634105/`
passes first-entry activation without any confirmation shortcut. It explicitly
holds back tracker connection, calibration, source-port ownership and live pose
data to verify those gates still prevent premature renderer/input/power entry.
Removal/reappearance, manual override and existing interaction/recovery tests
pass. Manual-entry NVIDIA GLES run
`.local/native-check/1794167721960580988/` also passes without a confirmation
shortcut. Python tests, tracker regressions, native geometry/pixel tests and
feature-enabled/disabled builds pass.

Tested release `6c1e99bd5564d56f` is staged for the next login. The
running compositor and tracker were not restarted.

The user also asked how to add interactive Waybar widgets. Live inspection found
Waybar running as a layer surface, with existing launcher/taskbar, clock and
system-control modules. The current spatial capture path enumerates application
toplevels, so Waybar needs explicit layer capture/input support. The agreed
layout follows head yaw horizontally at 0.6096 metres forward and holds height
at -0.3048 metres, allowing look-down reveal. Full yaw/pitch following at that
offset would keep it outside the current vertical field of view. The design and
interaction/validation requirements are recorded in `docs/NATIVE.md`; floating
Waybar rendering is not implemented or claimed by this entry-policy update.

## Floating interactive Waybar dock (2026-11-08)

The user authorized implementation of the agreed dock: horizontal head following,
two feet forward, centre row one foot down, revealed by looking down. The native
integration now has a private authenticated layer-capture manager and explicit
widget panel class (private scene protocol v3). Application panels retain their
workspace-fixed poses and original clients. Yaw-only widget revisions do not
synthesize client focus/motion; geometry is shared by rendering and picking.
Mouse/popup grabs pause following, and widget destruction cancels held releases.

Three compositor-owned temporary Waybar rows reuse the existing user's modules,
actions, JSONC includes and stylesheet. They are separate from the existing
desktop Waybar, without editing its config. Their flat scene nodes are hidden,
avoiding duplicate flat bars over the presentation. Mirrored layer trees include
subsurfaces and popup menus. A same-user `wofi` allowlist during active mode adds
a front search/launcher panel, including launchers opened by existing actions.
Its pidfd pins process identity for dismissal on desktop exit. Temporary bar
processes terminate on exit/lock, follow compositor lifetime and restart after a
crash; replaced captures release their SHM/GL state. Unrelated OS layers and lock
content are not captured by the widget path.

Closing a static widget menu exposed a deferred-capture retry bug: failure or
constraints updates could arrive while `wanted` was temporarily false between
scene messages, leaving no dirty texture to trigger the next capture attempt.
`prepare_capture` now re-evaluates capture on every draw, retaining ready/failed
buffer completion semantics. A regression compiles that actual helper and checks
retry without another dirty frame. This fixes the blank panel/global click gate
stall without relying on periodic Waybar animation.

The full automatic Vulkan integration run
`.local/native-check/1794169288272711794/` passes first-entry activation without
confirmation, calibration/source/live-scene gates, all existing interaction and
recovery checks, real Waybar clicks/scroll/menu actions, yaw-following hit targets,
application keyboard focus, wofi search/Escape, Waybar restart during a held click
without a stray release, complete bar/launcher cleanup and display reconnection.
An ordinary client cannot bind the private widget capture manager. Eleven Python
tests, tracker regressions, geometry/pixel tests and enabled/disabled builds pass.
The current user's Waybar profile parses into 5 application modules, 1 status
module and 9 system modules. Physical optical/input/performance acceptance of the
dock still requires the updated compositor login; these are nested tests.

Final dock-menu ordering is explicit in both the renderer and picker, keeping
large menus in front of neighboring rows without moving their physical planes.
The system row reserves enough logical width for the current profile's metric
labels, avoiding ordinary number changes repeatedly resizing the source.
The final automatic Vulkan run is
`.local/native-check/1794169620613266441/`; the matching full NVIDIA GLES run is
`.local/native-check/1794169699847980793/`. Both pass the larger overlapping-menu
fixture, launcher interaction, held-click crash/restart, complete process cleanup
and the existing native tests. Preview-only runs now use `.local/widget-preview/`
separately and do not create integration acceptance results.

The final real-profile NVIDIA preview is
`.local/widget-preview/1794169928177021548/widgets-profile.png`, visually checked
with the user's existing modules/styles. The spatial taskbar explicitly lists
all outputs, since canonical windows remain on the sleeping desktop outputs;
the preview confirms a taskbar entry for the original nested application. This
setting affects only temporary widget rows, preserving the desktop bar config.
Twelve Python checks now pass, including that taskbar configuration regression.
Tested release `098ba52e0b63a815` is staged with widgets enabled. Installed
compositor/presenter hashes match the latest passing full GLES integration. The
running compositor, desktop Waybar and tracker were not restarted. Activate this
matching protocol-v3 release with a saved-work logout/login.

## Automatic-entry scene starvation fix (2026-11-08)

The user reported that the running `098ba52e0b63a815` release displayed the dock
and startup HUD but never slept the monitors. The live compositor log confirmed
ARMING with captured Waybar layers and no transition to SPATIAL. `scene_update`
advanced widget yaw whenever the previous epoch had been acknowledged, including
when called immediately before `enter` checked that acknowledgment. Real tracking
noise continuously changed the pose, replacing the epoch and invalidating the
very readiness check it was about to perform. The earlier fixture's perfectly
static startup pose masked this bug.

The new regression injects continuous 0.2-degree yaw variation into the ordinary
owned synthetic tracker stream during startup, while retaining calibration,
source-ownership and missing-live-pose checks. It fails against the previous
build (`.local/readiness-before.log`). The dock's startup yaw
is now fixed; only the active frame loop advances head following, with a small
stationary-noise threshold. Input/readiness scene refreshes never change widget
yaw. A complete healthy frame can therefore satisfy automatic entry without
requiring a motionless head or weakening tracking/capture checks.

The full Vulkan run `.local/native-check/1794170709709651836/` passes automatic
entry and actual nested-monitor sleep under continuous pose changes, plus all
existing widget, input, lock, crash/restart and display-reconnection checks.
Twelve Python tests, tracker regressions and enabled/disabled builds pass.

The matching NVIDIA GLES regression run
`.local/native-check/1794170780784451396/` also passes the continuously changing
startup pose and automatic monitor sleep, plus all existing integration checks.
Tested release `a2a721c24463562a` is staged for the next login. Installed
compositor/presenter hashes match that passing run. The running compositor and
tracker were not restarted; a saved-work logout/login is required to load this
compositor fix.

## Continuous connected/unlocked mode (2026-11-08)

The user reported no automatic attempt after login with glasses attached. The
running a2a721c24463562a compositor had auto-start enabled, a usable extended DP-4
Air and healthy tracking, but its connection was marked consumed. Brian's labwc
autostart launches swaylock, which cancelled the one-shot connection attempt.
The user clarified that connected active glasses must use spatial mode whenever
unlocked, including after later locks; the earlier manual-reentry policy is
superseded. No special login-lock option is needed.

The policy now debounces connection/unlock for 500 ms, inhibits entry throughout
lock, and retries cancellation/failure after five seconds while connected. Every
entry still requires fresh tracking and a complete newly presented scene. Escape
restores the desktop immediately; auto_start false provides sustained manual use.

Full automatic widget integration .local/native-check/1794171300593390457 passed
initial login lock with glasses already present, later unlock auto-entry, tracker
recovery retry, monitor sleep, and existing input/widget/recovery checks. Twelve
Python tests, tracker regressions and native geometry/pixel/build checks passed.
The user found these long graphical tests intrusive; do not run more without
explicit permission. The current desktop was not restarted.

## Internal screen events instead of display presence (2026-11-08)

The user confirmed the glasses' own screens switch off on removal while DP-4
remains enabled and DPMS-on. Passive usbmon observation of only the original
Air MCU interface during the requested physical sequence recorded `Open OLED
2D`, `Close OLED`, and 6c04/zero. The complete opening packet was reconstructed
from the truncated text capture and its missing two bytes uniquely verified
against the device's original CRC32. No second device reader was opened.

The tracker now publishes those internal screen states from its existing MCU
reads, including reports otherwise truncated/consumed by synchronous queries.
Compositor automatic entry requires confirmed screen-on plus existing display,
lock, tracking and complete-scene gates. Off/unknown/stale screen state restores
the desktop. Unlock still automatically resumes when the OLED state is on.
Startup without an observed OLED event remains unknown: no verified current-state
query was found, and an already connected/worn login must not be claimed working
from this change alone.

Quiet hardware-free tests compile the real packet parser, publisher and command
receive helper; native and tracker builds pass. No graphical integration was
run, and no live compositor/tracker was restarted. The installer has an explicit
focused-validation path that records this limit and validates all three binary
hashes rather than pretending an older graphical result covers a new build.

Release `dfa0bb21b3214673` is staged for next login using explicitly limited
focused evidence `.local/native-focused/1794172027097498711/`. Thirteen Python
tests, tracker regressions and native geometry/pixel checks pass; all three
installed binary hashes match that record. The running a2a721c24463562a session
is unchanged.

## Super+Alt+X toggles automatic policy (2026-11-08)

The user confirmed the internal OLED-event workflow works well, then reported
that the one-shot X/Escape actions fight automatic retries. Super+Alt+X now
toggles the in-session automatic-mode preference. Disabling immediately invokes
normal compositor recovery/cancels arming and prevents future attempts until X
enables it again, including across lock/unlock and display/OLED changes. Enabling
resets connection debounce and retains all screen/tracking/scene prerequisites.
Repeated presses while the key is held do not repeatedly toggle the preference.
Super+Alt+Escape is removed from the spatial key handler and current HUD/docs.
The manifest's `auto_start` remains the next-login default.

Quiet focused validation `.local/native-focused/1794172478762324793/` passes
fourteen Python tests (including compiling the actual toggle against recovery
mocks in desktop/arming/spatial states), tracker regressions and native
geometry/pixel checks. The compositor build passes. Graphical integration was
not run. Release `1ea4220d3231c3a1` is staged for next login with matching binary
hashes. Running release dfa0bb21b3214673 and tracking were not restarted.

## HUD removal and spatial anti-aliasing (2026-11-08)

The presenter no longer allocates or draws the status banner, including status
text sent by older compositors. AIR_STATUS is log-only. Rendering now uses 2x
supersampling in each axis through Wayland buffer scale, requested 8x EGL MSAA
with supported fallbacks, trilinear mipmaps and maximum EXT anisotropic filtering
for all uploaded content, high-precision UVs and 2x grayscale-AA title textures.
Logical geometry/readiness viewport, pointer gain and capture completion remain
unchanged. No temporal accumulation is used.

The build and quiet focused evidence `.local/native-focused/1794172983797670083/`
pass fourteen Python tests, tracker regressions, geometry/pixel tests, and a new
invisible EGL-device GPU regression. On the RTX 4090 it obtains 8x MSAA and 16x
anisotropy and checks minified checkerboard coverage, NPOT resized colours and
fractional diagonal coverage using the actual shader/filtering code. It opens no
Wayland connection/window or sensor reader. Full-resolution latency/optics and
Wayland supersampling presentation still require physical acceptance; no
graphical integration was run, following the user's instruction.

Release `48625fffafb2bc32` is staged for next login with matching focused binary
hashes. It includes the previously staged automatic-mode toggle. The running
dfa0bb21b3214673 compositor/presenter/tracker were not restarted.

## Visible spatial cursor over browser video (2026-11-08)

The user reported that the pointer disappears over YouTube video but works on
the surrounding page. The compositor previously converted null/empty/destroyed
client cursor surfaces into a zero-sized AIR_CURSOR packet, which explicitly
hid the presenter glyph. Such video/CSS auto-hide requests now select a visible
default arrow. Transparent/unsupported cursors also fall back; a built-in arrow
covers missing/blank themes. Pending/transparent surfaces retain commit listeners
so later visible images can restore their shapes. Client cursor-shape requests
now enter the same authenticated spatial path instead of only updating the
suppressed flat cursor. XRGB/XBGR uploads correctly preserve implicit opacity.
The presenter ignores legacy hide/blank packets without erasing its glyph.

Quiet focused evidence `.local/native-focused/1794173329226712187/` passes
fifteen Python tests, tracker regressions, geometry/pixel tests and the invisible
GPU AA test. A new regression compiles the actual cursor-routing functions with
mocked surfaces and exercises null/pending/transparent/destroyed/unsupported/
unreadable inputs, later commits, theme fallback, alpha/padding, opaque formats
and desktop bypass. Enabled and feature-disabled builds pass. No browser or
graphical integration ran; YouTube acceptance remains for the user's login/test.

Release `28b6dd3fcd2175fd` is staged with matching focused binary hashes and all
previous pending toggle, HUD-removal and AA changes. The running session and
tracker were not restarted.

## Side-by-side stereo and adjustable software IPD (2026-11-08)

The user requested stereo and live IPD adjustment, with calibration after
implementation. Private scene protocol is now v4. The renderer presents parallel
left/right views, ±IPD/2, at 1920x1080 per eye in full 3840x1080 SBS. Head-right
rotates the eye baseline; panel geometry and cyclopean cursor hit stay shared.
Per-eye viewport/scissor, FOV, cursor scale and mouse gain remain consistent;
one scene acknowledgement covers both eyes. New presses require visibility in
both frusta. IPD and guide changes invalidate scene readiness.

The packaged session module native_stereo.py negotiates using the existing
owned reader's SBS control channel, snapshotting/restoring original hardware
mode and Air output mode/scale/transform/position. Only the uniquely identified
original Air is configured. Mode state must be fresh after this tracker
connection, with actual compositor dimensions confirmed and stabilized before
ARMING. Timeout/lock/OLED-off/auto-disable paths restore rather than sending
mispacked stereo. The live user's mode was not switched during implementation.

IPD starts at 64 mm, clamped 45–80 mm. Super+Alt+[ / ] adjusts by 0.5 mm;
Super+Alt+backslash resets to 64. Super+Alt+I opens/closes a temporary guide;
there is no persistent HUD and new clicks are blocked during the guide. The
supervisor merges the value into the latest manifest without replacing staged
release settings. This controls software eye spacing, not measured lens/IPD or
calibrated optical projection.

Quiet focused evidence `.local/native-focused/1794174767917963205/` passes 21
Python tests, tracker regressions, native geometry/pixel tests and invisible GPU
checks. Stereo tests measure actual shader left/right disparity and its greater
magnitude at near depth; maths cover rotated baselines and binocular visibility.
Mocked mode transactions cover confirmed SBS, initial-SBS restoration, invalid
requests/ownership/model, prior-reader state rejection and IPD persistence.
Enabled/disabled compositor and tracker builds pass. No graphical integration or
live HMD stereo/optical/performance acceptance was run.

Release `187f93c3ee98fde1` is staged with matching focused binary hashes,
`stereo: true`, `ipd_mm: 64` and packaged supervisor module. Activate through the
next saved-work logout/login. Running release 28b6dd3fcd2175fd, desktop clients
and session-owned tracking were not restarted by this work.

## Toggle notice, independent desktop profiles and reserved Air (2026-11-08)

Super+Alt+X now forks notify-send without blocking the compositor, emitting the
new Enabled/Disabled state under `Air Spatial Controls` with a coalescing tag.
This app-name bypasses the existing tracker/readiness suppression rule without
reenabling readiness notices. Repeat-key suppression still emits once per toggle.
IPD adjustments move from brackets to Super+Alt+comma/period, including guide
and current controls documentation. Reset/guide controls remain unchanged.

The user requested no ordinary desktop on the glasses, allowing a practical
middle ground. `reserved_display: true` now keeps a black compositor scene over
Air desktop/layer content and mirrors only the private presenter above it. Lock
trees remain above both. Scene ordering is enforced before output composition,
with mirror/output lifetime cleanup. Ordinary placement prefers usable desktop
outputs; on desktop return, windows/pointer in Air move to an ordinary usable
monitor after held buttons/drags complete. Air remains a connected output, not a
DRM lease; when it is the only usable display, desktop access remains as fallback.

The conflicting dual-xreal profile was removed from Brian's kanshi configuration.
Dell/LG dual-static and lg-only blocks retain their settings and omit Air. The
Waybar toggle now calls the tracked profile-priority helper, placing the chosen
profile first before reload/switch so glasses connection/SBS events don't prefer
dual-static over a user's single-monitor choice. Status recognizes legacy
dual-xreal as Dual during migration. Original files are backed up under
`.local/rollback/desktop-monitors-20261108-150450/`. The installer issued no live
reload or output commands; subsequent read-only status reports dual-static.

Quiet focused evidence `.local/native-focused/1794175599782922159/` passes 24
Python tests, tracker regressions, geometry/pixel tests and invisible GPU checks.
Added tests cover toggle notification state, profile priority/removal/idempotence
and desktop routing/only-screen fallback; enabled and disabled builds pass. No
graphical integration or physical reserved-output/notification acceptance ran.
Release `606c01cffeb8cb40` is staged with matching focused binary hashes, stereo
and reserved-display enabled, preserving saved IPD/settings. The running
28b6dd3fcd2175fd compositor and owned tracker were not restarted.

## Air profile matching and blocked live recovery (2026-11-08)

The user reported `(none)` after Super+Alt+X and no glasses display. Read-only
inspection found running release 606c01cffeb8cb40, Air now on DP-1 disabled,
Dell on DP-3 and LG on DP-4 enabled. Kanshi logged no matching profile: removing
Air from both profiles was incorrect because all connected heads must match.

The policy now generates connected/unplugged variants for each desktop choice,
keeping the selected pair first. Connected variants enable only the identified
Air at 6880,0 without specifying a mode; stereo still owns the mode. Separate
variants ensure hotplug reapplies the profile; an optional Air directive would
retain the current matching profile without enabling a new head. Waybar maps
both variants to the same Single/Dual state. Staging preserves the saved profile
order if status is unavailable. Home config/helpers are updated with backups,
latest `.local/rollback/desktop-monitors-20261108-151734/`.

Live reload initially succeeded and reported dual-static with all three outputs
enabled. Subsequent application failed with NVIDIA atomic-commit EINVAL on Air;
an Air-only off/on recovery then blocked in the compositor. Kernel logs show
NVIDIA Xid 16, missing vblank updates and GPU-progress errors (including before
this recovery). Further display mutations stopped; the pending recovery command
was terminated so no delayed enable is queued. No compositor/tracker restart or
logout was performed. Physical cable reconnect was requested; live recovery
remains unconfirmed. The revised policy is saved for next startup, but this is
not a claim that the current graphics-driver fault is fixed.

Three quiet profile tests pass (including matching every head with/without Air,
priority, mode preservation, invalid input and idempotence); Python and home shell
syntax checks and diff whitespace checks pass. No graphical test windows opened.
Native binaries were unchanged.

Follow-up after the user's recovery/reconnect: a fresh boot/session is running
(PID 2671, same 606c01cffeb8cb40 release). Display queries respond immediately;
Air DP-1 is enabled at 6880,0, currently 3840x1080@60, and kanshi reports
`dual-static`. Its log confirms successful connected/unplugged profile transitions.
Both desktop outputs remain enabled. The owned tracker currently reports OLED
off, so automatic spatial entry is waiting on wear state. No new display changes
or graphical tests were issued during this read-only follow-up. This confirms
current recovery, not that cable reconnect alone cleared the prior driver fault.

## Repeated primary-monitor flicker during SBS entry (2026-11-08)

The user reported frequent primary-monitor flicker. Logs show repeated hardware
SBS enable/disable pairs about five seconds apart, corresponding to Air DP
removal/reappearance and kanshi reapplying desktop profiles. There was no completed
spatial entry. The three-second OLED grace expired during stereo negotiation;
restoring 2D produced a fresh on signal and retriggered automatic entry.

For immediate relief the existing session supervisor PID 2650 was briefly
SIGSTOPped (tracker and compositor stayed live). The user explicitly disabled
automatic mode with X; controls/log confirmed the change, then supervisor was
SIGCONTed to complete normal mode restoration. It is running normally again.
Latest read-only checks retain lg-only, LG and Air enabled, Dell disabled. No
new modeset appears after the final restoration at session time 03:50.408.
No session, compositor or tracker was restarted by this work.

The compositor now allows the full bounded 20-second stereo transaction to
settle across transient OLED blanking/DP hotplug. Timeout latches stereo retries
off until an explicit policy toggle, so restoration's OLED-on event cannot cause
an unbounded mode-switch loop. Output-management application also skips heads
whose enabled state, mode, position, scale, transform and adaptive sync already
match and have no pending compositor state. This avoids gratuitously committing
unchanged desktop monitors when only the Air changes. Tests compile the actual
tracked-patch helper and check real changes/pending state/layout removal are not
skipped. Active video remains enabled under the reserved black Air composition;
reservation does not remove the connector or its video signal.

Release e801652c14c3ad37 is staged for next login. Quiet focused evidence
`.local/native-focused/1794176902132551815/` passes 27 Python tests, tracker
regressions, geometry/pixel checks and tiny invisible EGL checks. Enabled and
feature-disabled builds and whitespace checks pass. No graphical tests or live
SBS test were run. Current session remains on 606c01cffeb8cb40 with auto disabled;
physical negotiation/anti-flicker acceptance requires the next login.

## Return Air to a normal desktop (2026-11-08)

The user requested a normal extended Air desktop instead of the reserved black
surface after continued lack of visuals. Set the installed manifest's
`reserved_display` false and changed installer/supervisor defaults to false.
Black reservation remains opt-in only. Normal placement/cursor behavior follows
the existing disabled-reservation path; spatial mode and stereo are retained.

Connected Single now places Air immediately right of LG (3440,0); Dual keeps
Air right of Dell/LG (6880,0). The paired unplugged profiles preserve desktop
count. Existing lg-only preference is preserved. Home config staged without live
reload; backup `.local/rollback/desktop-monitors-20261108-153259/`, prior manifest
`.local/rollback/native-install.before-normal-air.json`. New immutable release
9ac8509a18230bb1 includes the changed launcher default and the same previously
validated binaries. Installer verifies focused binary evidence
`.local/native-focused/1794176902132551815/`; the three updated quiet profile
checks and syntax/whitespace checks pass. No graphical tests or live output
mutations were run. Running e801652c14c3ad37 has its reservation preference fixed
at startup; the user must log out/in to remove that current black mask.

## Repair tracker stereo command/confirmation path (2026-11-08)

The user confirmed desktop stability but no spatial visuals since stereo. Running
9ac8509a18230bb1 never reached the presenter: negotiation timed out. Read-only
state showed a fresh driver heartbeat with `sbs_mode_supported=false`, a stale
OLED heartbeat, and Air physically configured at 3840x1080. Driver logs showed
SBS changes, a command error and subsequent MCU reconnect. The IMU remained
connected, but display control/wear events were unavailable.

Inspection found the MCU command receiver treated the next arbitrary report as
its command reply. Asynchronous OLED events/other replies caused failure. The
normal event reader also treated valid short reports as disconnection. Commands
now read full reports with bounded waits, observe OLED events, validate CRC and
length, and skip unrelated IDs while awaiting the matching reply. Short valid
reports are accepted; malformed traffic is ignored without disconnecting the
MCU. Command ACK wait is bounded once rather than nesting an old 300-attempt
loop around the new bounded receiver.

The tracker setter also wrote its desired mode into the same field overwritten
by the next ordinary MCU read. It reported that optimistic field as actual SBS
state. Requests now use an atomic queue; the sole controller thread applies them,
and only a successful mode query updates the separately confirmed state. Rapid
opposite requests supersede the queued request. MCU reconnection restores the
model's supported SBS capability after an earlier failed open disabled it.

Release 969a0d54accd9669 is staged for next login, retaining normal Air desktop
and Single/Dual profiles. Existing session/tracker/display modes were unchanged.
Focused evidence `.local/native-focused/1794177651033120510/` passes 29 Python
tests, tracker timestamp/startup regressions and invisible native GPU/math/pixel
checks. Tests compile actual MCU receiver/setter functions with report queues:
OLED-before-reply, unrelated IDs, corrupt reports, bounded timeout, request
preservation through an older cached read, honest confirmation and superseding
requests. Tracker/native builds and patch reverse checks pass. All three frozen
binary hashes match the evidence. No graphical or physical stereo negotiation
acceptance was run; this is a staged fix, not a claim of working stereo optics.

## Live MCU initialization and stereo recovery (2026-11-08)

After login into 969a0d54accd9669, auto mode never attempted entry: IMU was
calibrated but display control reported unsupported and OLED state was unknown.
A passive USB capture (glasses MCU interface 4, endpoints 6/7, not IMU endpoints
4/5) observed activation query success followed by firmware-version reply 0x26
with declared length 38: 20 data bytes instead of the 41-byte destination. The
new strict receiver incorrectly rejected this legitimate variable string. It
now zero-pads short strings while requiring complete scalar replies. A bounded
single-reader diagnostic confirmed actual MCU initialization and SBS support.
The original tracker crashed during reconnect (heap corruption); no duplicate
reader was launched while it was alive. The cause of that heap-corruption message
has not been proved.

The tracker handoff helper now supports an already-dead child and owns both
readiness and StereoMode transactions. It pauses the original session supervisor
using pidfds, starts exactly one replacement reader, publishes its screen/stereo
ownership and resumes the original supervisor on compositor exit or handoff
failure. A mocked lifetime test verifies resume/descriptor cleanup on failure.
No compositor/applications were restarted. Normal source-port ownership and
calibration prerequisites remain enforced.

Further physical wear capture showed put-on Open OLED + virtual-display-toggle
0x6c05 value 1, followed by Close OLED from SBS negotiation without removal.
Publisher wear state now gives the virtual wear signal priority over OLED logs
once observed, so mode blanking cannot impersonate taking the glasses off.
Unknown startup state remains unknown until an event; no current-state query is
claimed.

Thread diagnostics then identified recurring controller disconnects just after
opening. Firmware emits full 42-byte DP-vsync log messages during SBS; upstream
used unbounded strlen and treated a missing terminator/length mismatch as MCU
disconnection. Async text now copies only the CRC-validated declared span and
never disconnects for a full-length/padded string. A regression uses a 42-byte
unterminated log followed by nonzero guard bytes. Temporary debug=threads was
removed by the next normal tracker launcher configuration write.

Final release 3cee8562457369cc is staged and its tracker is live through handoff
monitor 189905 / reader 189932 (original compositor 110856 remains). Original
supervisor 110854 is intentionally paused while the handoff owns the session
controller duties; it resumes on handoff shutdown. Intermediate handoffs/readers
were stopped before replacement. Final focused evidence
`.local/native-focused/1794178760699567977/` passes 31 Python tests, tracker
regressions and invisible GPU/math/pixel checks. Native/tracker builds pass.
No graphical test windows or second USB reader were used. Physical automatic
entry is still being checked; these fixes alone are not optical acceptance.

The user's next physical attempt still produced no visuals. The fixed reader
remained calibrated/connected and confirmed SBS true, while OLED temporarily
read off. Inspection found automatic_update still required OLED-on to call
start() a second time after mode negotiation began, and tick rejected ARMING
while OLED was blank. Thus the earlier grace period did not actually let the
presenter start through the blank interval.

A compositor correction now keeps preview startup progressing for an already
requested transaction through the bounded mode-switch blank interval, including
ARMING. It cannot initiate a transaction from off/unknown alone. Actual entry
(input routing/desktop-monitor sleep) separately requires OLED-on plus existing
tracking/scene prerequisites. Expired blank negotiation still latches retries
rather than looping. New helper regressions cover initial-off rejection, pending
blank allowance, active-mode rejection and whole-transaction timeout.

Release cb59641078d6f0b5 is staged for next login:
focused evidence `.local/native-focused/1794178953490785480/` passes 31 Python
tests, tracker regressions and invisible GPU/math/pixel checks. Exact installed
binary hashes match. Running compositor 110856 still has the old gate; the latest
compositor fix requires a new login. Live reader 189932 remains connected and
calibrated under handoff 189905; no further physical cycles were requested.
Automatic stereo entry/optical validation is still unconfirmed.

## Stereo frames reach DRM, glasses still physically black (2026-11-08)

After login into cb59641078d6f0b5, the compositor successfully reached SPATIAL,
received healthy scene/presentation acknowledgements, and slept LG DP-4. The
user reported no visual image. A read-only grim capture of Air DP-1 during the
reported blank view (`.local/spatial-output-check.png`) contains the expected
left/right stereo application scene and spatial pointer, not an all-black image.
It contains private desktop content; keep it local/ignored.

The sole tracker confirmed actual MCU mode 3 (full SBS), fresh owned OLED-on,
and SBS=1. DRM debug state showed active Air CRTC with 3840x1080 framebuffer and
3840x1080@60 timing: 300000 kHz, totals 4000x1250. This matches both detailed
EDID timings. Air connector DPMS was On. DRM desktop CRTCs were actually off;
public output-management enabled reporting remained stale after direct DPMS,
so an initial suggestion that kanshi had woken the desktop was incorrect.

The user explicitly clarified that the lenses were completely black, and had
to exit the mode to read/reply. Leave auto off during further diagnosis; do not
ask the user to remain blind while awaiting chat questions. A fresh profile/
renderer change is not justified by this evidence alone. The remaining failure
is after software composition/DRM configuration; the actual adapter/wire signal
and device display path require investigation. Prior workstation notes describe
DP+USB but do not identify the adapter model. Asked the user for that model;
no new graphical test windows, compositor restarts or code changes were made in
this diagnostic turn. The physical stereo failure remains unresolved.

Hardware path identified by user: WJESOG DisplayPort-to-USB-C 6.6 ft 4K@60Hz
cable, plus AuviPal USB-C female/female coupler model USBC4 and the glasses'
USB-C cable. Ratings/model names are user-reported; no manufacturer-specific
full-SBS interoperability guarantee was verified. Do not recommend replacing
these parts based on the current software screenshot alone. Asked whether the
WJESOG plug can connect directly to the glasses with auto off, bypassing the
coupler/extension, while retaining the ordinary desktop for communication.

A bounded hardware-only stereo check was then performed after the user reconnected
the glasses with auto disabled. It used the existing tracker control channel,
changed only the identified Air output, opened no windows and left LG DP-4 DPMS
On. Both CRTCs were active: LG at 3440x1440 and Air at 3840x1080@60 (300 MHz,
4000x1250 totals). The user confirmed the glasses were still completely black.
The script restored prior hardware mode and Air layout successfully. This rules
out renderer content and desktop-monitor sleep as necessary causes of the
blackout. Coupler bypass also did not change the reported outcome.

Next comparison requested: select 3D with the glasses' own brightness+ button
while automatic mode remains off, then return to 2D using the same hold. This
separates a USB display-mode command issue from stereo transport/firmware failure.
No claim that cable bandwidth or firmware is the proven cause has been made.

## Manual 3D works; repair software mode-write framing (2026-11-08)

The user confirmed hardware-button-selected 3D shows an image. It was mode 3,
3840x1080@60 with the same 300 MHz EDID/DRM timing as the black software path.
Enabling automatic spatial mode while already in that manual 3D mode then showed
the spatial scene successfully. Thus the optical rendering and cable can work;
software mode selection remains the proven distinguishing action. Do not blame
the coupler, nominal bandwidth or monitor sleep based on previous black results.

Compared the tracker with primary source control implementation:
https://github.com/8796n/nyan-real-firmware-kit/blob/main/xreal/display.py
https://github.com/8796n/nyan-real-firmware-kit/blob/main/xreal/glasses.py
The reference sends report-id 0 plus a zero-padded 64-byte frame, sequence 0x9100,
and a little-endian four-byte mode to 0x08. Upstream sent a one-byte mode in a
short report. The mode write now matches the full framing, without changing
other commands or opening another reader. No firmware/flash operations are used.
Also note 0x07 reports DP-link mode, not proof of optical activation.

Release 302796f048bf602c is staged, retaining the unchanged current compositor.
Focused evidence `.local/native-focused/1794180389110467253/` passes 32 Python
tests, tracker regressions and invisible native checks. The new actual-function
regression checks report ID/padding, exact 32-bit mode placement/CRC/sequence,
partial write/ACK failure and both mono/stereo values. Tracker build and whitespace
checks pass. Asked the user to return to 2D with auto disabled before applying a
live tracker handoff and checking the corrected software transition with the
primary monitor awake. That physical check is pending.

Physical check of the corrected full-report/u32 mode write still produced black
lenses with the primary monitor awake; automatic restoration succeeded. Therefore
framing was a hypothesis, not the proven root cause or a validated fix. The
user again confirmed manual 3D, followed by automatic spatial mode, displays the
spatial scene. Asked whether to adopt the already-working selected hardware mode
(mono normally, stereo when physically selected), disabling automatic USB mode
switches, or continue researching the undocumented internal transition.

## Adopt the glasses-selected mode automatically (2026-11-08)

The user chose the working physical-mode policy instead of more USB switching
experiments. Native entry now reads the owned hardware mode: confirmed 2D with
1920x1080 uses mono; confirmed SBS with 3840x1080 uses stereo. Scale 1 and normal
transform are required; mismatched/unknown mode or transport waits. A hardware
mode change during use exits before the next matching entry. The prior mode
request/grace/timeout latch was removed. The compositor publishes hardware
request zero and retains the existing scene/tracking/OLED/lock entry conditions.

`native_stereo.py` defaults to passive publication/IPD persistence. It cannot
write modes or query/configure outputs during production updates, even if a
stale producer requests hardware enable. Shutdown/lock/exit preserve the physical
selection. Legacy transaction helpers require explicit diagnostic opt-in; no
production launcher opts in. IPD keys/guide and the normal Air desktop remain.

Release 9cf95e3e9ddd8a0b is installed for next login. Evidence
`.local/native-focused/1794180786636865679/` passes 33 Python tests, tracker
regressions, native geometry/pixel checks and tiny invisible GPU tests. Enabled
and feature-disabled builds and whitespace checks pass. All frozen binary hashes
match. Regressions cover mode/dimension mismatch rejection and production mode
publication without any hardware/output writes on entry/exit. No new graphical
nested tests ran. Manual 3D followed by spatial entry was physically confirmed
visible earlier; the new automatic mono/stereo-selection policy itself awaits
activation through the next login. Current compositor cb59641078d6f0b5 was not
restarted. Current tracker handoff remains owned by its monitor; normal next
login resumes standard single-supervisor ownership automatically.


## IPD adjustment visibility and renderer verification (2026-11-08)

User reported no perceptible effect from IPD adjustment. Running compositor was
9cf95e3e9ddd8a0b; logs confirmed selected stereo and live changes from 64 through
76.5 mm and back to 64. Air remained 3840x1080/scale 1/normal. This rules out
ignored shortcuts or mono entry for the reported sequence; no evidence of a
broken eye-offset shader was found. It does not prove perceptual calibration.

Presenter now applies settings through a separately testable real handler and
logs the received mode/value/guide state. IPD changes display a 2.5-second value
readout; initial entry does not show it. The optional guide includes equal-angular
near (0.6 m) and far (3 m) targets, and explains that software spacing changes
depth disparity, not optical sharpness/lens position. Removed redundant viewport
readiness announcements for value-only packets; actual size changes still report.
No compositor/input/hardware-mode behavior changed.

Quiet actual-handler regression verifies live 64→70→64 updates, signed eye
offsets, feedback expiry, guide state and invalid-value rejection. Invisible GPU
regression now checks monotonic disparity for 45/64/80 mm at both distances,
using the production eye-offset helper/shader. Increased its offscreen surface
from 32 to 256 pixels because the far-target differences quantized to the same
sample at 32 pixels. This does not open a desktop window.

Release c3584fc85be85905 installed with focused evidence
`.local/native-focused/1794182970145878999/`: 34 Python tests, tracker checks,
native geometry/pixels and invisible GPU checks passed. Compositor hash matches
the running release exactly; refreshed its helper launcher for the next spatial
entry, without restarting the current presenter or desktop. No logout needed.
Physical visibility/comfort of the new guide and feedback still needs user
observation. No claim that software settings measure anatomical IPD is made.


## IPD remains perceptually unchanged; add optical-routing diagnostic (2026-11-08)

User reports no visible difference across the extremes, despite the changing
number in the new guide. Live process is the c3584fc85be85905 presenter and its
logs confirm applied stereo values while guide is on. The issue is unresolved;
packet handling and invisible shader checks alone cannot establish per-lens
optical output or perceptual calibration. Do not dismiss the report as small
step size or claim the previous feedback change fixed the underlying issue.

Optional guide now adds an eye-specific LEFT EYE / RIGHT EYE label, and blue
screen-fixed reference ticks alongside the live white near/far targets. These
provide two separate physical checks: correct routing to each lens, and image
motion against a stable reference with one eye closed. Reference/label draws
bypass translation then restore the live offset before targets and remaining
scene content. No normal scene projection or hardware mode changes were made.
Actual-guide regression verifies distinct eye textures, zero-offset references,
and restored live offsets for both eyes at 45/80 mm. Quiet focused evidence
`.local/native-focused/1794183247290135551/` passes 35 Python tests plus tracker
and invisible native checks. Release 52b58572d5f079c7 installed, with identical
compositor hash and helper redirect for next spatial entry; no logout/restart or
graphical nested tests. User asked for physical routing/reference observation;
result pending. This is a diagnostic improvement, not a proven root-cause fix.

## Physical IPD motion confirmed; correct misleading diagnostic (2026-11-08)

User observed the white target moving relative to blue when adjusting IPD with
one eye closed, confirming a visible image response. They saw no eye labels and
understandably tried to align white and blue. Blue references deliberately use
zero translation, so alignment would require zero software IPD; moving toward
45 mm is not calibration. The check cannot measure anatomical IPD. Earlier
instructions/comfort-guide wording were misleading and are corrected.

Eye labels were placed at y=.36 m, z=1.4 m, above the per-eye vertical frustum.
Moved them to y=.25 and the instruction card to y=.08. The actual-guide test now
checks all four corners of every drawn quad against the production per-eye
frustum at both 45/80 mm; the old label placement fails that condition. Visible
instructions explicitly say not to align the marks and that this is a rendering
check, not IPD measurement. User settings were preserved; advised backslash to
restore the 64 mm starting value rather than choosing 45 to approach blue.

Release 5b2d317db4bb9845 installed with quiet focused evidence
`.local/native-focused/1794183499562748679/`: 35 Python checks, tracker and invisible
native checks pass. Same compositor hash; live helper redirect makes corrected
instructions/positions available next spatial entry without logout. No physical
mode switching or graphical nested tests. Per-lens labels remain physically
unverified; image motion is user-confirmed, anatomical calibration is not.

## Support measured asymmetric eye profiles (2026-11-08)

User supplied monocular far PD plus total far/near PD and requested the best
possible match. Implemented a software profile without claiming phone accuracy,
physical lens calibration, or tracker-to-face registration. Personal profile and
measurement source/values are in the ignored install manifest, backed up before
editing. Near PD is retained as metadata only. Eye positions use midpoint offset
plus/minus half the total; the configured asymmetry survives total adjustments.
Reset now restores the saved baseline rather than hardcoded 64 mm. The optional
check shows both derived monocular distances and reset value.

Protocol v5 sends total/centre/reset baseline to the presenter. Renderer and
compositor binocular visibility use the same shared head-right eye geometry.
No toe-in, optical warp, convergence adjustment or hardware mode write was added.
A session-owned .ipd settings publication permits later profile changes without
another login. It validates owner/freshness/finite bounds; changed tuples must
acknowledge the latest local control value. This prevents both repeated old
heartbeats and delayed acknowledgments from reverting newer key adjustments.
All changes invalidate scene acknowledgment before further clicks. Installer
preserves profile and measurement metadata when staging another release.

Release 8bd19b6905dbe81a staged for next login, with exact binary hashes matching
`.local/native-focused/1794184301051928608/`: 37 Python tests, tracker startup/
timestamp checks, native geometry/pixels, invisible GPU tests pass. Feature-off
build and whitespace checks pass. Regressions cover asymmetric signed offsets
and rotated baselines, real shader disparity/midpoint, guide bounds, invalid
profile/ownership/staleness, retained baseline/metadata, and delayed-publication
races. No graphical nested test or physical-mode switch was run.

Live compositor remains 9cf95e3e9ddd8a0b, presenter 5b2d317db4bb9845. Do not
redirect its v4 helper to the new v5 renderer; one fresh login is required for
both sides to use the same profile. Saved configured values and all three
installed binary hashes were verified. Physical acceptance after that login
remains outstanding; matched software values are not proof of perfect optics.

## Frame rate and head-following performance (2026-11-08)

User requested 60 FPS and smoother Waybar translation. Read-only nine-second
strace of outgoing private packet headers (no pixels/titles) measured the original
presenter at 16.7 FPS, median presentation interval 59.4 ms / p95 68.1 ms, and
42 static scene revisions across 151 displayed frames. Both native components
were compiled -O0. Synthetic conversion of a 3440x1440 RGB buffer improved from
24.79 to 5.05 ms with -O2. Initial optimized renderer-only release 1b5330d51a6bc025
measured about 21–23 FPS, unchanged 2x supersampling / 8x MSAA / 16x anisotropy.
Actual FPS/gaps and CPU-side frame/upload/swap timings now log every five seconds.

Release 60f4a475056e1e4e added optimized compositor, partial source-damage uploads
with reusable staging/storage, and per-rendered-frame widget following. Protocol
v6 sends follow/frozen policy and the frame's actual widget yaw. Only complete,
current-scene, newer displayed frames update compositor widget picking geometry;
head movement no longer creates repeated static scene epochs or synthetic focus
changes. Following freezes for buttons/native manipulation/popup grabs. Quiet
regressions cover the actual policy/adoption functions, geometry, preserved
application poses and invalid yaw, as well as partial upload/crop/padding/alpha,
texture reuse and forced full initialization on resize/replaced SHM storage.
User logged in to this release. It measured 60 FPS when quiet, but later fell to
26 FPS; user confirmed video/scrolling was active during the slowdown.

Inspection of installed wlroots 0.20.2 render/gles2/texture.c found the key active
content bottleneck: padded SHM rows trigger glReadPixels per row instead of a
single read. Source archives for wlroots and Wayland confirm no required four-
byte row alignment. Existing 12-byte RGB row alignment caused 1041/1395 reads
per capture for observed 1715-wide windows. Capture allocation now requests exact
width*bpp stride, with overflow validation. Conversion retains padded input,
RGB/BGR and opaque/alpha handling. Source-damage upload and all AA stay enabled.

A GPU/DMA-BUF staging experiment passed invisible NVIDIA allocation/copy/fence/
NPOT/alpha tests but failed live synchronization/copy and fell back to SHM. It
was removed from the final source/dependencies/tests, not advertised as a working
production path. The user toggled into eef24d78743f843a and completed the requested
video/scrolling observation. It was the packed SHM fallback that then measured
mostly 59.4–60 FPS, some samples 57.9–58.4 FPS, with common maximum gaps ~17 ms
and occasional longer frames. This is measured presentation under that activity,
not a guarantee for every workload. No explicit visual correctness/perceived-
smoothness response was supplied yet; asked about widget motion separately.

Final clean release 440fba8b47138b73 contains the working packed SHM path and no
experimental GPU path. Evidence `.local/native-focused/1794186894703062544/`
passes 39 Python checks, tracker startup/timestamp checks, native geometry/pixels
and invisible EGL tests. Enabled/feature-disabled builds and whitespace checks
pass. Exact three installed binary hashes and saved eye profile fields verified.
Current compositor remains 60f4a475056e1e4e with identical final compositor hash;
current presenter eef24d78743f843a is already using the successful packed SHM
fallback. Its helper is redirected to final 440fba8b47138b73 on next ordinary
spatial toggle; no further logout/restart was performed. No graphical nested
windows, input injection, display-mode switching or extra tracker was used.

The user subsequently confirmed Waybar movement is “noticeably smoother” when
turning their head. This supplies physical smoothness acceptance for the new
frame-following path, alongside the measured near-60-FPS activity samples.

## Temporary Super+Alt+wheel reading zoom (2026-11-09)

User requested incremental zoom while holding Super+Alt, with snap-back to 1×
on modifier release. Connected USB identity was rechecked as original Air
3318:0424. Its IMU supports rotation, not stable tracked translation; this
feature deliberately implements view magnification rather than inferred leaning.

Private packet protocol v7 carries compositor-owned zoom. Vertical wheel-up
increases magnification by 10% per ordinary 15-degree notch, wheel-down reduces
it, bounded to 1–3×. Horizontal/invalid/busy shortcut scroll is consumed without
application scrolling. The modifier-change hook observes aggregate seat modifiers
and resets when either Super or Alt is absent, without waiting for a mouse event.
Exit/lock clears zoom; opening the stereo guide resets it. Held buttons/native
manipulation, stale pose, calibration and graphics pauses block zoom increments.

The shader narrows both eye frusta by the same factor without changing panel
geometry or eye spacing. Compositor visibility checks share that factor, each
change requires a newly acknowledged scene, pointer angular gain compensates,
and cursor pixel size stays constant. Zoom/reset sends no client focus/motion.
Quiet control tests compile the actual compositor functions against mocks; the
invisible GPU regression measures magnified area, stereo image centres and exact
snap-back through the real shader. Existing capture-completion and watchdog
regressions remain passing.

Release f45d2c8bdc54d620 is staged using focused evidence
`.local/native-focused/1794240753457666510/`: 45 Python tests, tracker checks,
geometry/pixel/invisible GPU tests pass. Enabled and feature-disabled compositor
builds, test-tool build, patch reverse check and whitespace check pass. All three
installed binary hashes match that evidence. One pre-existing presenter title
snprintf truncation warning remains; no new build failure.

Actual running compositor remains PID 180448 from release 60f4a475056e1e4e,
verified via pgrep and /proc/180448/exe. No live helper redirect, desktop restart,
extra sensor reader, hardware mode write or graphical nested test was performed.
A saved-work logout/login is required for the matched v7 pair. Physical shortcut,
modifier-release and reading usability acceptance remain outstanding.

## Adaptive head-pose smoothing for small tremors (2026-11-09)

User reported unpleasant valid head-pose jitter, including movement associated
with heartbeat. Added presenter-owned `air-pose.h`, a speed-adaptive first-order
quaternion filter following the 1 Euro principle, before neutral/level-horizon
conversion. Initial minimum cutoff 0.6 Hz, speed coefficient 0.35 Hz per deg/s,
and speed-filter cutoff 2 Hz. Tiny/slow motion is smoothed more than fast turns.
This does not claim heartbeat sensing, perfect cancellation or removal of drift.

The receiver filters only the latest accepted pose per receive pass. Individual
queued datagrams have no trustworthy sample times, so packet-drain spacing is
not used for derivatives. First sample, recenter and >=250 ms sample gaps reset
directly rather than sweeping from old history. Idle/repeated/out-of-order filter
times do not advance it. Raw samples, actual reception freshness, owned source
port, sequence reset, presentation completeness and watchdog rules are retained.
The same filtered basis drives rendering, pose reports/visibility and displayed
widget yaw. Mouse integration is independent and no head-induced client motion
or focus is added.

Synthetic 0.03-degree-amplitude 1.2 Hz yaw oscillation across the wrap boundary
has output/input RMS ratio 0.467 at 60 Hz and 0.478 at 1000 Hz after settling.
A synthetic 60 deg/s turn has maximum angular lag 0.972/1.145 degrees at those
rates, with steady lag below 0.7 degrees. These are software signal responses,
not measured physical motion-to-photon latency or optical comfort. Quiet tests
also cover valid rotation matrices, all conversion branches, no fixed dead zone,
recenter while trailing, level horizon/optional roll, gap resets, freshness and
newest-packet batching. Existing capture-completion regression remains passing.

Release 7d44785f66eab0d5 staged with focused evidence
`.local/native-focused/1794241383745699373/`: 46 Python tests, tracker checks and
four native tests (geometry, pixels, invisible GPU, adaptive pose) pass. Native
build and test-tool build pass; whitespace checks pass. All three installed
binary hashes and login launcher hash verified. Compositor binary is unchanged
from the tested v7 zoom release f45d2c8bdc54d620.

Actual live compositor still PID 180448 / release 60f4a475056e1e4e (v6), checked
through /proc/180448/exe. Therefore no helper refresh to the v7 renderer was
attempted. Smoothing is bundled with the pending zoom update and needs the same
single saved-work logout/login, not an additional login after that update.
No compositor/tracker restart, extra reader, display mutation or graphical
nested test was performed. Physical smoothing strength/turn comfort remains
for user observation after login.

## Spatial workspace controls and input (2026-11-10)

Release `4b01a092d25388ae` is staged for the next login with private protocol
v8. The current session still runs `7d44785f66eab0d5`. The workspace overview
uses search and presented-frame picking. Layout presets, title-bar snapping,
pins, saved placements and sizes, reading scale, and dock summon are available
in spatial mode. Game Focus temporarily centers and fits the focused app group.
Wayland pointer drag-and-drop, lock and confinement use the compositor's seat.

Focused evidence is in `.local/native-focused/1794357898633053127/`. All 50
Python tests, the tracker checks, and four native tests passed. The native-enabled
and native-disabled compositor builds passed. The isolated workspace check in
`.local/native-headless/1794357901817545575/` exercised keyboard and mouse
selection, presets, reading scale, atomic layout save, real GTK drag-and-drop,
Escape cancellation, and a real Wayland pointer lock with relative motion.
During the animated headless 1280×720 mono sample the presenter averaged
59.8 FPS, with a 20 ms p99 frame gap and 24 ms p99 received-pose age at scanout.

These measurements cover isolated headless outputs. The physical Air was
disconnected during validation, so supported higher-refresh modes, physical
scanout timing, optical comfort, and game-specific behavior remain to be checked.
