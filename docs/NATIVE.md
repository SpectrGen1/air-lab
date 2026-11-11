# Native labwc spatial session

The native implementation is built and tested in disposable nested sessions.
The installed login launcher selects it. After reboot on 2026-11-08, the patched
compositor was verified running and the user confirmed visible native windows
after the NVIDIA capture fix. Physical input/power/recovery checks are still
required before calling this a validated daily desktop.

## Activate once, then toggle in-session

Save your work and log out of the current labwc session, then log back in.
This is the one initial patched-compositor login required by `DESIGN.md`.
Replacing a running compositor cannot preserve its existing client connections.
After this initial login, entering/exiting spatial mode keeps applications in
that same labwc process; it does not reconnect or relaunch them.

Air notifications and the additional one-minute warmup are disabled by user
preference. The presenter waits only for connection, the driver's initial
calibration and its owned UDP socket. Device startup can still take approximately
15 seconds. Legacy `ready_at` deadlines from an older running supervisor are
ignored; actual tracking-loss watchdogs and source-port validation remain active.
The installer silences Air notices from an older supervisor with an app-specific
mako rule and backs up that config under `.local/rollback/`. New supervisors and
tracker handoff monitors do not send notifications at all.
Tracking starts with the session and stops when the session ends. A readiness
file under the private runtime directory tracks connection, calibration and the
child-owned UDP socket. The renderer pins its receiver to this reserved source
port, rather than accepting arbitrary local UDP senders.
Do not start a second IMU reader alongside it.

The tracker preserves factory calibration at connection. Upstream's unconditional
one-second recalibration was removed: movement during that second can be learned
as a large false turn rate, which its later stationary estimator rejects even
after minutes of waiting. The timestamp-gap correction remains independently
necessary. This does not provide an absolute yaw reference or guarantee zero
long-term gyro drift. Recenter changes the neutral direction, not gyro bias.
Wait until windows are visible before recentering with your head upright.
The renderer exits spatial mode if the driver's packet sequence resets its
orientation reference; re-enter after the tracker reconnects rather than using an
obsolete neutral frame that can flip the workspace.

| Shortcut | Action |
| --- | --- |
| Super+Alt+X | Toggle automatic spatial mode; off immediately cancels startup/restores desktop and stays off until toggled on |
| Super+Alt+R | Recenter the viewing direction without moving the world pointer |
| Super+Alt+F | Summon the pointer into the current view |
| Super+Alt+Arrow | Move the focused panel in the spatial workspace |
| Super+Alt+Minus / Equal | Slow down / speed up the spatial pointer |
| Super+Alt+0 | Reset pointer speed to its configured default |
| Alt+Tab / normal workspace keys | Use normal labwc focus/workspace switching |

Every entry activates automatically after fresh tracking and a complete healthy
scene, including the first entry per login. No confirmation shortcut is needed;
turning automatic mode off during arming cancels it. Locking always exits spatial
mode. Unlocking automatically re-enters when auto-start is enabled and the
glasses remain connected and automatic mode remains enabled.

## Automatic display workflow

With `auto_start` enabled (the installed-session default), an extended original
Air display plus a confirmed internal OLED-on USB event opens the same preview
as Super+Alt+X. A connected display alone is insufficient, including at login. A 500 ms connection debounce allows output setup to
settle; this adds no calibration warmup. The private presenter still waits for
the session-owned tracker connection, initial calibration and UDP source port.
Desktop input and monitor power stay intact during startup. As soon as fresh
tracking and a complete healthy scene are presented, the compositor automatically
routes input and sleeps the previously-on non-Air monitors. The first connection
behaves like every later connection; Super+Alt+Return is no longer required.
The dock remains at its neutral yaw during startup. In SPATIAL, the renderer
samples horizontal following once per frame. Presentation feedback carries the
actual displayed widget yaw; compositor picking adopts that pose without a new
static scene epoch or synthetic pointer/focus events. Following freezes during
button/popup grabs. Readiness and click checks never advance it. This avoids both
startup starvation and the old acknowledgment/timer loop that slowed following.

Removing the glasses display closes preview/spatial mode and restores the
previously-on desktop monitors. While connected and unlocked, automatic mode
keeps requesting spatial mode. Turning automatic mode off restores the desktop immediately and stays off until
Super+Alt+X enables it again. Renderer/tracking failures retry after five seconds
only while automatic mode is enabled. Locks prevent entry;
unlocking debounces for 500 ms and resumes automatically, including after the
initial login lock. Automatic entry waits for active mouse buttons, desktop drags
and pointer constraints to finish. `auto_start` chooses the login default.

The trigger is the compositor's identified Air display connection, not a second
USB reader or a guessed head-motion/wear signal. If this hardware removes and
re-adds its display on take-off/put-on as reported, those events drive the
workflow. Physical confirmation of that behavior remains outstanding. The
existing output-loss restoration handles removal immediately, retaining its
scene-output teardown ordering.

`.local/native-install.json` records `auto_start` (default true). Set it to false
before the next login to start with automatic mode disabled. Super+Alt+X changes
the current session preference without editing the login default. The supervisor passes it as
`AIR_SPATIAL_AUTO_START`; a running compositor must be replaced by a saved-work
logout/login to apply the new logic or change this setting. No public input
injection or extra tracker is used.

A healthy preview and active spatial session have no duration limit. The
two-minute timeout applies only while the presenter has not started. Pose loss
with continuing presentation still recovers after 700 ms. If graphics/presentation
stalls, a bounded pause retains the scene for up to ten seconds from the stale
sample/frame, with new pointer actions blocked. Both fresh tracking and actual
scanout are required to resume. Persistent failure restores the desktop; these
are recovery checks, not session-duration limits. Logs distinguish pause/resume
from actual recovery. Presentation feedback maintains liveness even while a window
is being recaptured. Incomplete capture invalidates click eligibility until the
matching complete scene is presented. A displayed frame whose own pose was older
than 250 ms is incomplete even if capture was healthy when drawing started.

Panels start at two-thirds of the default physical width and height. This
initial presentation scale leaves the client geometry intact. The desktop camera
defaults to a level horizon: yaw/pitch turn the viewpoint, while reported roll is
excluded after neutral-frame conversion. This is a viewing preference, not a
claim that the physical sensor axes were recalibrated. Full head roll remains
available. `.local/native-install.json` records `panel_scale` (default 2/3,
accepted range 0.25–2) and `head_roll` (default false); both apply at login.
This compositor update requires one saved-work logout/login; refreshing only the
renderer cannot change panel geometry or picking in an already-running labwc.

Alt+Tab changes focus but does not bring the selected panel into the viewing
direction. Turn toward its position or use Super+Alt+Left/Right to move it.
Face forward and use Super+Alt+R if the neutral view was set while the Air rested
on the table. A panel locator remains a usability improvement to implement.

## Head-pose smoothing

The presenter filters raw orientation with a speed-adaptive first-order filter
using shortest-arc unit-quaternion interpolation. Small slow motion receives
stronger smoothing; faster motion raises the cutoff to reduce added lag. This
follows the [1 Euro filter principle](https://gery.casiez.net/1euro/), adapted to
rotations. Initial parameters are 0.6 Hz minimum cutoff, 0.35 Hz per degree/second
of smoothed angular speed, and a 2 Hz speed filter. They are software tuning,
not measured heartbeat cancellation or physical latency acceptance.

Filter the newest accepted pose once per receive pass, using monotonic reception
time. Drained packet spacing is not sensor sample timing. The filter does not
advance during idle polls, modify raw sensor samples, renew pose freshness or
relax source ownership/watchdogs. Recenter, first sample and gaps of at least
250 ms reset directly to the new orientation. Existing sequence-reset handling
still exits mode on a tracker-reference reset. Level-horizon conversion follows
smoothing; optional physical roll remains available.

The filtered basis is shared by the rendered camera, compositor pose reports and
per-frame widget following. There is no separate pointer filter or client motion
from head movement. No new protocol or compositor change is needed beyond the
already staged v7 zoom release. A matching live compositor can use a renderer-only
refresh; never redirect the older live v6 helper to a v7 renderer.

Quiet tests cover rotation validity, yaw wrap, synthetic tremor attenuation and
turn lag at 60/1000 Hz, tiny deliberate offsets without a dead zone, recenter,
pauses, repeated/out-of-order timestamps, newest-packet batching and retained
freshness rules. Physical comfort/latency and tuning remain user acceptance.

## Temporary reading zoom (protocol v7)

While spatial mode is active, hold Super+Alt and scroll vertically to magnify
around the current viewing direction. Wheel-up increases apparent size by 10%
per ordinary 15-degree wheel notch; wheel-down reduces it, bounded to 1–3×.
Continuous scroll uses proportional increments. Releasing either modifier snaps
back to 1× without needing another mouse event. Exit/lock also clears the zoom.
The shortcut consumes scroll over applications, frames and empty space. Zoom
changes are blocked during held-button/manipulation grabs, the stereo diagnostic,
stale tracking and watchdog pauses; modifier release still resets the view.

This is temporary perspective magnification (a narrower rendered frustum), not
tracked camera translation, application reflow or a change to panel geometry.
Both stereo eyes use the same factor with the existing eye offsets. Head motion
and the world pointer stay independent. Pointer angular gain compensates for zoom
and the glyph retains its viewport pixel size. Visibility checks use the same
narrowed frusta as rendering; each factor change invalidates scene acknowledgment
until the matching new scene is presented. No focus/motion event is synthesized
by zoom or reset. The guide resets zoom before opening.

Private packet protocol v7 requires matching compositor/presenter and a fresh
login. Do not redirect a live v6 helper to this renderer. Quiet control/visibility
and invisible GPU tests do not establish physical shortcut/readability acceptance.

## Spatial window controls

Mouse gain is calibrated to the renderer viewport: at the default speed, one
supplied mouse-motion unit is approximately one viewport pixel at the centre.
The existing device acceleration remains in effect. Vertical movement uses
angular elevation, and horizontal travel compensates for elevation, so motion
does not become slower or uneven up/down the cylinder. Packet splitting does
not change the integrated path. This is workspace-relative motion; head pose
does not enter mouse integration. Perspective away from the view centre remains
normal perspective rather than a second screen-relative pointer frame.

The cursor glyph has a consistent on-screen pixel size at different panel
depths. Its hotspot stays on the compositor's actual world-space intersection;
over empty space it sits on the cylinder, not a sphere. You can tune speed
without leaving spatial mode using the shortcuts above. Session adjustments
persist across toggles; `.local/native-install.json`'s `pointer_speed` (default 1,
range 0.25–3) selects the starting/reset speed at login.

Once spatial mode is active, use the outer spatial
frame, which sits outside the captured application (including its shadows and
popup extensions). It is separate from any title bar drawn by the application.

- Left-drag the spatial title bar to translate around and up/down the cylinder.
- While holding that bar, scroll up to move closer and down to move farther
  away. The held point stays on the mouse ray; physical panel size is unchanged
  by depth changes. Radial distance is limited to 0.45–4 metres.
- Drag a floating window's outer frame to request new client dimensions about
  its centre. The app reflows at the new size. Straight edges change one axis
  and corners preserve the aspect ratio to logical-pixel rounding. Resize
  requests respect client size hints. While a replacement capture is pending,
  the previous image keeps its original dimensions and new clicks wait for
  the updated scene.
- Release the left button to finish. Super+Alt+X disables automatic mode and restores the desktop even
  during a held drag. A removed target cancels the grab and consumes its eventual
  release, so another application never receives an unmatched button-up. On
  exit, pre-entry floating windows recover their saved sizes and positions.
  Windows that would be on the glasses move to a regular monitor when available.

Every panel faces the fixed viewer origin in full 3D, including elevated panels.
Mouse motion controls the grab; head motion only changes the viewpoint. Drag a
dialog's own title bar to reposition it relative to its parent. Scrolling during
that drag changes only the dialog's depth. Each dialog starts 15 mm in front of
its parent's current spatial plane and follows changes to the parent's position
and scale. Resizing a dialog changes its own client dimensions. Poses persist per window and
workspace across spatial toggles for this compositor's lifetime. New logins or
recreated application windows receive new poses.

The frame highlights on hover and uses move/resize cursors. These controls are
compositor-owned; their mouse buttons and wheel events are not sent to the
application. Ordinary client clicks, selection and scrolling still use the seat.
This changes the compositor and private protocol (version 3), so install the
matching compositor/presenter and log in again; do not hot-swap only the renderer.

The configured connector is DP-4, but the compositor verifies the Air display
identity and USB `3318:0424`, and can find the same Air after connector renaming.
It refuses a non-extended/overlapping physical layout. Wayland-backend outputs
are allowed separately for disposable nested tests.

## Workspace overview, layouts and reading profile

Super+Alt+L opens the window overview. It includes windows on other workspaces and
minimized windows. Type to filter by title, app ID or workspace, use arrows or
scroll to navigate, and press Enter or click a row to center and focus the
window. Esc closes the overview. The renderer acknowledges each complete menu
revision before its rows accept clicks. Head turns update visibility labels
without moving the desktop pointer or changing keyboard focus.

Super+Alt+C centers the focused window. Super+Alt+1, 2 and 3 apply Focus, Side by
Side and Code + Reference to the current workspace. The focused window gets the
primary slot. Side by Side gives two windows equal space and Code + Reference
gives the primary window more room. The focused frame includes Center, Pin and
Minimize. Pinned windows keep their pose when a preset is applied. While moving
a top-level title bar, the snap preview marks the available center, left or
right slot before release.

Super+Alt+S saves the current layout and Super+Alt+4 recalls it. Air Lab keeps
live poses and spatial resize dimensions through wear cycles. The saved layout
also restores matching apps after a new login. It matches workspace, app ID and
title, with an app-only fallback when one layout entry has that app on that
workspace. Windows with identical titles and app IDs share the same identity
for matching. The private file in `$XDG_STATE_HOME/air-lab/workspace-v1`
(default `~/.local/state/air-lab/workspace-v1`) includes window titles. An
explicit save replaces that layout atomically. Flat desktop rectangles still
restore independently when spatial mode exits.

Super+Alt+B saves a reading profile that increases the apparent size of app
panels by 35%. It works in mono or stereo and can be combined with Focus or
temporary zoom. Super+Alt+D raises the look-down dock for eight seconds from its
usual position and keeps it raised while a dock interaction is held.

## Drag-and-drop and games

Use standard pointer drag-and-drop between visible windows. A grabbing cursor
marks the drag. Move the pointer onto the destination, then release. Escape
cancels the drag. Recovery and lock cancel a pending drop before releasing seat
buttons. A destination must still be visible in the current scene on release.

Super+Alt+G centers the focused app group and temporarily hides other panels.
Exit with the same shortcut or switch apps. The game returns to its prior
placement. Wayland pointer lock keeps absolute position and focus on the locked
app while sending relative mouse motion. Pointer confinement uses surface-local
coordinates. XWayland and individual game compatibility still need testing.

## Architecture

- **Patched labwc 0.20.3** owns views, original rectangles, focus, real seat input,
  per-workspace poses, input regions, grabs, mode transitions and output recovery.
  Its source revision is `e0a3970b0c70e321814e8004ff7c6dbc7aeaf884`.
- **air-presenter** is a native Wayland/EGL/GLES mono/stereo renderer. It is a layer-shell
  presentation surface with no keyboard interactivity or input region. It is
  absent from toplevel enumeration and cannot become a normal focused window.
- **Private IPC** uses an inherited Unix socketpair plus a separately inherited
  private Wayland connection created by the compositor. Client identity is the
  actual private `wl_client`, not an app-ID claim or a public token. Protocol
  packets carry version, mode generation and scene epoch. Presented acknowledgments
  come from `wp_presentation` feedback for the relevant rendered epoch.
- **Capture** uses ext foreign-toplevel/image-copy capture with SHM, no embedded
  cursor. Changed content is uploaded into reusable GL textures. Head turns reuse
  textures; no PNG encoding/decoding is in the presentation path. This is a CPU
  SHM-to-GL upload path, not a measured zero-copy implementation.
  The NVIDIA GLES path advertises 24-bit BGR888; conversion supports RGB888/BGR888
  with explicit padded row stride as well as four-byte formats. Unsupported SHM
  formats produce an error and recovery, rather than a silent black preview.
  Frame ready/failed events complete buffer ownership; image-copy capture does
  not use `wl_buffer.release`. Constraint failures can reallocate/retry without
  stalling the capture stream.
- **Input** intersects the world cylinder ray with panel planes in labwc, then
  uses the particular capture group's scene subtree to find the real surface
  and its input region. The ordinary seat path receives child-local coordinates.
  Looking changes projection only; it does not move a flat cursor or select focus.
  New clicks/scrolls require a recent trusted orientation and matching presented
  scene revision. Implicit grabs use unbounded plane coordinates, retain the last
  valid point near a parallel plane, and deliver matching releases offscreen.
- **Surface groups** retain subsurfaces and XDG popups, with explicit capture-origin
  offsets. Native builds keep each transient toplevel's capture group separate
  and attach its spatial pose to its parent, rather than combining different
  toplevel origins into one ambiguous capture. Frame callbacks are sent for XR
  content even while its normal primary output sleeps.
- **Cursor** images/hotspots are exported separately over anonymous SHM FDs.
  Client cursor surface commits are observed, including a set-cursor request
  arriving before its first buffer commit. The ordinary output cursor is hidden
  while spatial mode owns pointing.

Window geometry, maximization and workspace state remain canonical. Panel
placement is separate, and frame resizing updates client dimensions through
the normal configure path until desktop geometry is restored. Minimized windows
are excluded from capture. Panel
positions are stored per view creation ID/workspace, independently of stacking.
Destroyed view pointers are invalidated before free. Geometry/scene changes
invalidate click eligibility until the renderer presents the matching revision.

Power snapshots include connector identity and prior enabled state. Only named,
previously-on non-Air outputs sleep. External DPMS/output reconfiguration is
suppressed during spatial ownership. Restore finds powered-off outputs directly
(the usual labwc name lookup intentionally filters them out), restores a viable
output before removing the presenter, and retries transient restoration failures.
A real disconnect uses labwc's existing output-remap policy. The disconnect hook
runs after destroying the disappearing scene output, preserving upstream's
output-teardown listener ordering.

On lock, opaque lock trees are created immediately before restoration, rather
than using the usual delayed blank. Restored scanout is composed with the lock
frame. The private presentation client is disconnected and terminated, dropping
its captured buffers. New toplevel-capture requests are denied while locked.
This describes this implementation's path, not a security certification of all
other capture applications in a desktop session.

## Supported boundary and remaining gates

The native path supports mono and glasses-selected side-by-side stereo. OpenXR,
optical calibration and DRM leasing are separate milestones.

Normal client clicks, typing, scrolling, XDG popups, transient dialogs, text
selection and pointer drag-and-drop use the existing seat. The spatial frame
implements move, resize and depth controls. Desktop server-side decoration
capture, tablet/absolute input and client-initiated desktop move/resize gestures
remain incomplete. XWayland override-redirect popups, large/complex subsurface
trees and unusual cursor buffer transforms still need their own validation.

Right-click XDG context menus are constrained in parent-window coordinates while
spatial mode owns the display, rather than against a sleeping monitor's empty
usable area. Their captured popup surfaces receive real pointer input. Pointer
coordinates are synchronized before presses and after explicitly summoning the
pointer, so a menu opens at the visible hotspot. The regression opens a GTK
context menu, clicks a real action, and dismisses subsequent menus with Escape
and an outside click while the flat output sleeps. XWayland override-redirect
menus remain a separate unvalidated gate.

The automated test's simulated output disconnect is not an actual USB/DP unplug
of the Air. Physical unplug, NVIDIA physical display sleep/wake, long-running
video, all workspace/application combinations, ten-minute drift/readability,
frame-time percentiles and real motion-to-photon measurements remain physical
acceptance checks after the initial login. A compositor crash still disconnects
its clients; a helper watchdog cannot undo that.

## Build and validate

```sh
cd /home/work/projects/air-lab
sh scripts/build-tracker.sh
sh scripts/build-native.sh
sh scripts/build-native-test-tools.sh
python3 -m unittest -v
sh scripts/check-tracker.sh
python3 scripts/check-native.py
AIR_NATIVE_TEST_RENDERER=gles2 python3 scripts/check-native.py
AIR_NATIVE_TEST_AUTO_START=1 python3 scripts/check-native.py
# Longer optional check: 130 seconds in active mode.
AIR_NATIVE_TEST_RENDERER=gles2 AIR_NATIVE_TEST_SOAK=130 python3 scripts/check-native.py
```

Existing workstation dependencies were sufficient; no packages were installed.
Native build dependencies are Meson, Ninja, a C compiler, wlroots 0.20 development
files, Wayland/Wayland EGL, EGL/GLES2, Cairo and normal labwc build dependencies.
The disposable integration tests additionally use GTK 3/PyGObject, `grim`, and
`wlopm`. Protocol XML for test input/close tools is fetched from pinned upstream
revisions into `.local/` and generated locally with its copyright notices intact.

`check-native.py` creates two isolated nested outputs with a unique process
marker. Synthetic input targets only that socket. Nested host input is ignored under the
unique test marker, so physical pointer-enter events cannot perturb automation. The output-disconnect step
closes the unique parent window titled `labwc - WL-2` with app-ID `labwc` and
refuses ambiguous matches. Close other nested labwc WL-2 windows before running
it. Its final JSON result records the tested binary hashes. It checks:

- clicks/keys into the original editor with its flat output asleep;
- no pointer/keyboard events from head-only motion, retained typing focus, and
  rejection of unrelated UDP sources;
- continued editor animation/frame callbacks during sleep;
- offscreen press rejection and out-of-panel drag/release;
- fractional scale 1.25 and a modal transient;
- black lock output, no retained helper, restoration and explicit re-entry;
- emergency escape with a frozen helper and exactly one held-button release;
- stale tracking, hung-helper termination, and output-disconnect recovery while
  preserving the existing editor process.
- tracker reference-reset recovery without retaining an obsolete viewing frame.

The geometry unit test covers head/pointer independence, cylinder seam,
back/parallel rays, unbounded/fractional coordinates and transient offsets.
The labwc patch also builds with `-Dair-spatial=disabled`.

## Persistent exit diagnostics

The native supervisor creates a private JSON Lines file under
`${XDG_STATE_HOME:-$HOME/.local/state}/air-lab/events/` for each login and
updates `latest.jsonl`. It retains the latest 20 event files. Existing
timestamped labwc session logs and tracker logs keep their current locations.

```sh
events="${XDG_STATE_HOME:-$HOME/.local/state}/air-lab/events/latest.jsonl"
jq -c 'select(.event == "spatial_exit" or .event == "process_exit"
             or .event == "watchdog_pause" or .event == "desktop_restored")' "$events"
```

The event file uses UTC timestamps and correlates each entry attempt by
`generation`. A `spatial_exit` records the reason, arming or active phase,
render readiness, pose and frame age, and whether a pointer grab was active.
A value of `-1` for an age means there was no valid timestamp. OLED-off and
missing OLED telemetry have distinct reasons, as do a reported display-mode
change and unavailable display-mode status. In the last reported values, `-1`
means unavailable, `0` means mono or OLED off, and `1` means stereo or OLED on.

Check nearby `process_exit` events for the presenter PID, actual exit status
or signal, and whether the compositor requested termination during recovery.
The supervisor records compositor and tracker exit status even if those
processes cannot write their own messages. The initial `session_start`
entry identifies the release and detailed log paths. An abrupt loss of the
supervisor or power can leave a session with no final `session_end` record.
The structured log contains lifecycle diagnostics and includes no captured
pixels, keystrokes, or window titles.

## Installation and recovery

```sh
python3 scripts/install-native-session.py
```

Installation requires matching hashes from the most recent successful native
integration run. Binaries and launcher scripts are copied into an immutable
local release under `.local/releases/`. `.local/native-install.json` records
selected paths, power policy, validation and the original login-launcher backup.
Only Brian's existing `~/.local/bin/start-labwc-logged` launch command changes;
its logging remains intact. `/usr/bin/labwc`, greetd, the kernel, kanshi and
labwc's rc.xml are not replaced or reconfigured. The current desktop is never
restarted by this script.

For a renderer-only update with an identical compositor binary, run
`python3 scripts/refresh-native-presenter.py` after installation. It backs up and
redirects the old session's helper launcher to the new tested renderer for its
next spawn, recording the redirect in the install manifest. Binaries remain
frozen; the old release's helper launcher is the deliberate exception. Exit the
preview and toggle again to use the update without logging out. A changed
compositor still requires a new login.

For a tracker update in an existing login, `python3 scripts/restart-native-tracker.py`
closes the private preview, stops the old reader, then starts the selected release
under a handoff monitor. It follows the compositor and original supervisor using
pidfds, stops the replacement when either exits, and publishes readiness/source
port for the new child. No artificial warmup is added. The original supervisor
may briefly publish a false readiness state for its exited child; this can only delay readiness. The monitor
republishes the replacement's state. `live_tracker` in the install manifest and
`.local/tracker/handoff.log` record this temporary ownership. Next login uses the
ordinary supervisor again. Do not launch a second handoff concurrently.

For a single stock session from a logged-in VT:

```sh
/home/brian/.local/bin/air-spatial-stock
```

This sets `AIR_USE_STOCK=1` and selects `/usr/bin/labwc` through the normal logged
launcher, without starting the tracker. To restore stock as the normal login:

```sh
python3 /home/work/projects/air-lab/scripts/restore-stock-session.py
```

The restore script checks for newer user edits before restoring the original
backup. It does not terminate the current session. Logs remain under
`~/.local/state/labwc-session/` and `.local/tracker/state/xr_driver/driver.log`.
The runtime readiness file and both children are owned by the session supervisor;
parent-death handling prevents an orphan tracker after supervisor loss.

## Floating Waybar widgets

Spatial mode starts three compact Waybar rows using the existing Waybar module
configuration and stylesheet: applications/taskbar from `modules-left`, status
from `modules-center`, and system controls from `modules-right`. The spatial taskbar lists clients from all outputs because their canonical
windows stay on the sleeping desktop outputs. The desktop Waybar and its
configuration are not modified. JSONC comments, trailing commas
and relative includes are supported. Rows run as temporary compositor-owned
processes only while spatial mode is starting/active; they restart after a crash
and are removed on mode exit, output loss or lock. Their configuration/style
live in inherited anonymous files rather than generated files in the user's
Waybar directory. Changes to the user's Waybar config apply on the next toggle.

The dock follows horizontal head yaw at a radial distance of **0.6096 metres
(two feet)**. The centre row is **0.3048 metres (one foot) below** the viewer;
the other rows sit 0.045 metres above/below that centre row. Pitch and roll do not
move the dock with the view, so looking down about 26.6 degrees reveals it. Each
row faces the viewer origin and is capped at 0.42 metres wide, preserving its
pixel aspect ratio. These are mono virtual geometry distances, not calibrated
stereoscopic focus distances. Physical optical/ergonomic acceptance is pending.

A private Wayland capture manager exports only registered widget scene trees to
the compositor-owned presenter. Ordinary clients cannot bind it. Existing
application capture remains separate. Each widget has a composed mirror of its
live layer/subsurface tree and popup menus; its flat scene node is hidden to
avoid duplicate bars over the presentation. Real seat events route to the
original Waybar surfaces, preserving existing click/scroll actions and menus.
Active widget menus render and pick ahead of neighboring dock rows, so a tall
menu stays interactive when it overlaps another row. The widget class has no
move/resize chrome. Normal application panels remain
fixed in the workspace.

The selected `wofi` launcher is also captured as a temporary front panel when
it opens during active spatial mode. This is an explicit namespace/same-user
allowlist entry, supporting both existing Waybar launcher actions and OS launcher
shortcuts. It accepts keyboard search and Escape; desktop exit dismisses its
layer. Unrelated desktop layers, notifications, lock surfaces and the presenter
are never included by this widget path. Other layer-shell launchers are not
currently supported. New ordinary application windows use the existing native
window path.

Widget geometry is committed by the compositor and used identically for
rendering and picking. Following pauses during mouse-button or popup grabs.
Yaw-only scene revisions do not synthesize client pointer/focus events. Explicit
mouse movement or clicks synchronize through the ordinary seat. A destroyed
widget cancels held pointer state and consumes its eventual release, preventing
an unrelated application from receiving it. Action-only clicks preserve the
last application keyboard focus; interactive menus/search use normal layer and
popup focus semantics. Source buffers and textures for replaced widgets are
released, and capture retries after every draw, including a retry deferred by
a scene transition or a closing static menu.

`.local/native-install.json` records `widgets` (default true) and the frozen
`widget_launcher`. Setting `widgets` false disables widget startup on the next
login. The compositor/presenter private protocol is now **version 3**, requiring
a matching tested release and saved-work logout/login. Never refresh only the
renderer against the older compositor. The geometry and pixel tests, Python
regressions and isolated integration checks exercise the new path:

```sh
sh scripts/build-native-test-tools.sh
AIR_NATIVE_TEST_WIDGETS=1 AIR_NATIVE_TEST_AUTO_START=1 python3 scripts/check-native.py
AIR_NATIVE_TEST_WIDGETS=1 AIR_NATIVE_TEST_RENDERER=gles2 python3 scripts/check-native.py
# Render the current user's real profile in isolation, without clicking its actions:
AIR_NATIVE_TEST_WIDGETS=1 AIR_NATIVE_WIDGET_PREVIEW=1 python3 scripts/check-native.py
```

The integration fixture uses actual Waybar/wofi with harmless action markers.
It checks click, scroll, menu selection, keyboard focus, horizontal following,
launcher search/dismissal, capture recovery after menu resizing, held-click
cancellation on Waybar crash/restart, process cleanup and desktop recovery.
Physical hardware use, other applications' external menus, alternate launchers
and long-running performance still require acceptance checks.

### Continuous automatic mode

With `auto_start: true`, connected usable glasses and an unlocked session mean
spatial mode. Appearance and unlock use a 500 ms debounce. A login lock or later
lock prevents entry while locked; unlocking automatically resumes without a
shortcut or reconnect. Tracking and complete-scene health still gate input and
monitor sleep. Failures restore the desktop immediately and retry after five seconds while
automatic mode remains enabled. Super+Alt+X disables auto mode and restores the
desktop until explicitly toggled back on. `auto_start` sets the login default. This supersedes the older one-attempt and
manual-reentry-on-unlock policy described in historical evidence above.

### Internal OLED state / wearing workflow

Passive USB observation during the user's on/off sequence confirmed that the
original Air remains connected and DPMS-on while its own OLEDs turn off. Its
MCU sends `Open OLED 2D` (6c09 firmware log) on opening and `Close OLED` plus
6c04/zero on closing. These are screen-state events, not a claimed direct wear
sensor API. Other screen-off causes, including the glasses' button, also exit.

The existing tracker now observes CRC-checked full MCU reports before command
reply filtering, preserving events arriving during display queries. Its ordinary
MCU read loop drains events with a 100 ms read timeout rather than one event per
second, while polling display mode at most once per second. It atomically
publishes `tracker.json.screen` with reader PID, monotonic timestamp and state.
The compositor requires a matching tracker PID and heartbeat no older than three
seconds; off/unknown restores active mode and prevents automatic re-entry.
No extra sensor reader, input injection or graphical test process is used.

Startup defaults unknown until an actual OLED event arrives. No verified
current-state query was found, so already-worn-at-login entry is not guaranteed
until an event occurs; do not infer it from display connection or IMU motion.
Quiet focused validation covers captured on/off packets and CRC/truncation, an
async OLED event during an actual short-reply receive helper, publisher state,
PID mismatch, future/stale stamps, and existing geometry/tracker/Python checks.
Graphical integration and physical automatic entry/sleep remain untested for
this build, following the user's request to avoid intrusive tests.

### Single automatic-mode shortcut

Super+Alt+X now toggles the in-session automatic policy, including while arming
or with a frozen renderer. Disabling restores the desktop through compositor
cleanup and prevents retries across lock/unlock and OLED/display changes. Enabling
clears stale debounce state and lets the ordinary poll wait for OLED-on, unlocked
session, idle input, healthy tracking and a complete scene. Repeated press events
while X is held do not repeatedly flip the preference; the matching release is
consumed. Super+Alt+Escape has no spatial action. Runtime toggles do not rewrite
the manifest's initial `auto_start` preference.

### Spatial anti-aliasing and unobstructed view

The presenter no longer draws a HUD/status strip, including status packets from
older compositors. Messages remain in logs. It renders at 2x resolution per axis
(3840x2160 for the Air's 1920x1080 logical viewport) using Wayland buffer scale 2.
The compositor downsamples to the physical display; scene geometry, logical
viewport reporting, input picking and pointer size/gain remain unchanged. Resize
recomputes the render scale; devices exceeding viewport/renderbuffer limits fall
back to 1x. EGL chooses 8x MSAA where available, then 4x/2x/no MSAA.

Application, image, widget, popup, cursor and decoration textures use trilinear
mipmaps plus maximum supported EXT anisotropic filtering. ES2 devices lacking
full NPOT support retain linear filtering for NPOT captures. Mipmaps regenerate
only when pixels upload, not on each head movement. UV interpolation uses high
precision when supported. Owned title text is rasterized at twice its logical
resolution with grayscale AA and unhinted metrics for perspective projection.
Captured client pixels retain their original resolution; this cannot recreate
missing source detail. No temporal history/ghosting or sharpening is introduced.

The invisible EGL-device regression compiles the same shader source and texture
filtering code, verifies a heavily minified checker resolves without aliasing,
checks solid-colour NPOT resize, and verifies fractional diagonal-edge coverage.
On this workstation it runs on the NVIDIA RTX 4090 with 8x MSAA and 16x anisotropy.
It creates only a tiny offscreen buffer, no Wayland connection/window/input or
sensor reader. Optical sharpness, full-resolution performance and presentation
remain physical acceptance checks; no graphical integration was run.

### Persistent spatial pointer over video

Browser/video cursor auto-hide no longer erases the spatial pointer. Null cursor
requests, absent/transparent images, unsupported uploads and destroyed client
cursor surfaces fall back to a visible default arrow. A built-in arrow handles
a missing or blank cursor theme. Ordinary visible client cursor images and
server/client cursor-shape requests still select link/text/resize shapes.
Pointer focus/input rules, geometry and desktop-mode cursor behavior are unchanged.

Fallback for a pending or transparent surface keeps its commit listener, so a
later visible client image can restore without another set_cursor request.
Four-byte cursor uploads now explicitly preserve ARGB/XRGB/ABGR/XBGR channel and
implicit-alpha semantics. The presenter also retains its last visible glyph on
legacy hide/blank packets. Quiet regression compiles actual compositor routing
with mocks and covers null/blank/pending/commit/destroy/unreadable/unsupported
surfaces, alpha thresholds/padding, opaque formats and desktop bypass. No browser
or graphical integration was opened; actual YouTube acceptance is pending.

### Side-by-side stereo and adjustable IPD

Stereo rendering is permitted by default (`stereo: true`, `ipd_mm: 64`).
Automatic entry follows the physical glasses selection: confirmed 2D plus a
1920x1080 transport uses mono; confirmed SBS plus 3840x1080 uses stereo. Scale 1
and normal transform are required. Unknown or mismatched mode/dimensions wait
for the physical transition to settle; mode changes during use restore desktop
before a matching next entry. `stereo: false` requires physical 2D mode.

In stereo, parallel cameras at `center_mm ± IPD/2` render 1920x1080 per eye at the existing
40° horizontal FOV. Both share a head sample, scene epoch and capture set, with
isolated viewports/scissors and one acknowledgement for the complete pair.
The matching protocol-v6 compositor/presenter is required.

The supervisor's `native_stereo.py` publishes fresh, owned tracker mode and
persists IPD. Production does not write the hardware mode or output layout, even
on entry, exit, lock or shutdown. Only explicit diagnostic construction with
`switch_modes=True` enables the old transaction helper; no production launcher
sets it. The compositor always publishes hardware request zero. The user
confirmed manual 3D plus spatial mode is visible, while both upstream and
full-report/u32 USB writes produced black lenses. Readback/ACK and a correct DRM
framebuffer do not prove optical activation. The user selected this working
physical-mode policy instead of further USB-transition experiments.

Controls (all with Super+Alt held):

- **I:** show/hide the optional stereo depth check. New clicks are blocked while it is open.
- **, / .:** decrease/increase software IPD by 0.5 mm, clamped to 45–80 mm.
- **\:** restore the saved baseline (`ipd_baseline_mm`, defaulting to the initial IPD).

The value saves automatically into the current manifest without replacing a
concurrently staged release or other settings. This is virtual eye spacing,
not a measurement of the user's IPD and not a lens adjustment. The guide is a
rendering diagnostic, not a calibration procedure, with equal-angular-size targets at 0.6 m
and 3 m to compare depth disparity. Each viewport has its own LEFT/RIGHT EYE
label for checking physical eye routing. Blue reference ticks bypass eye
translation; white targets use live IPD. With either eye closed, adjustment
should move the nearby white target relative to its fixed blue tick. Do not
align these marks: the blue tick has zero eye translation and coincidence would
require zero software IPD, outside the supported range. Use a measured IPD;
64 mm is a starting value, not a measurement. Labels and guide corners fit the
per-eye viewport at both supported IPD extremes. Adjustments show a 2.5-second readout of the
value received by the renderer, without a persistent HUD. This changes binocular
depth disparity; it does not adjust optical sharpness or physical lens spacing. Application
geometry and cyclopean picking stay unchanged. The cursor is drawn at the same
world hit in each eye, giving it matching disparity. Pointer gain and glyph
size use per-eye width. New presses require the hit to be in both eye frusta;
IPD/guide changes require a new committed scene acknowledgement.

Quiet checks cover rotated baselines, actual GPU disparity, selected-mode
admission, IPD persistence and fresh ownership. A production-policy regression
asserts even a stale hardware-enable request cannot trigger output queries or
mode writes; manual SBS remains untouched on exit. The user verified visible
spatial rendering after manually selecting 3D. Stereo fusion/comfort, calibrated
optics and performance remain unverified. No graphical nested tests ran for
this update. This remains direct SBS rendering, not OpenXR or optical distortion
calibration.

### Toggle notifications and reserved glasses output

Super+Alt+X emits one Mako notification naming the new automatic-mode state,
using app-name `Air Spatial Controls` and a replace/coalescing tag. Disabling
still restores monitors before notification. Readiness/tracker notifications
remain suppressed under their distinct `Air Spatial Lab` app-name. IPD controls
are now Super+Alt+comma/period; brackets no longer adjust IPD. Backslash reset
and the optional Super+Alt+I guide are unchanged.

The default is `reserved_display: false`: Air shows a normal extended desktop
outside spatial mode. Changing this session setting requires a fresh login.
Optional `reserved_display: true` keeps the Air as a connected output needed for direct
mono/SBS rendering, but covers desktop windows, wallpaper, bars, notifications
and menus with a black compositor scene outside spatial mode. A mirror of only
the authenticated private presenter is drawn above black; authentication trees
remain above both. Ordering is enforced immediately before output composition.
Mirror/source/output destruction clears ownership, and shutdown destroys the
reservation. While an ordinary monitor is usable, normal output selection avoids
Air and desktop return moves any Air windows/pointer to a usable ordinary monitor.
When the glasses are the sole usable display, normal desktop remains a recovery
fallback. `reserved_display: false` selects ordinary extended-desktop behavior
for the next login. This is a reservation within labwc, not a DRM lease or a
physically removed desktop output; the logical connector remains available.

The conflicting `dual-xreal` kanshi profile was removed. Existing `dual-static`
and `lg-only` retain their Dell/LG settings and enable Air at 6880,0 for Dual or
3440,0 for Single,
without prescribing its mode. Corresponding `-unplugged` variants omit Air.
Kanshi requires every connected head to match; simply omitting Air produces no
matching profile and leaves a newly connected Air disabled. Distinct variants
force profile reapplication on hotplug instead of retaining an optional-head
profile without enabling the new head. Waybar displays both variants as the
same Single/Dual desktop choice.
Waybar monitor-toggle calls `set-desktop-monitor-mode.py`, which writes the
chosen desktop profile pair first before reload/switch, preserving that preference
when Air hotplug/SBS negotiation reevaluates profiles. The system staging script
`install-desktop-monitor-policy.py` backs up affected files, preserves modes and
does not reload or change live outputs. Initial backup:
`.local/rollback/desktop-monitors-20261108-150450/`. Live layout picks up the staged
policy at the next Waybar toggle or login. Spatial mode still owns monitor power
while active; it does not accept competing output reconfiguration.

Quiet checks cover toggle state/notification calls, profile transforms/priority,
and output selection including the only-screen fallback. Native/disabled builds
pass. Actual Mako placement, reserved-output composition and physical hotplug
behavior remain user acceptance checks; no graphical tests or live mode changes
were run.

The prior automatic USB stereo-negotiation policy is superseded by following
the glasses' selected mode. There is no automatic hardware switching or related
timeout latch in the current policy. Wear, lock and scene/tracking prerequisites
still control entry and monitor power.

Output-management requests include every head. Fully unchanged heads, with no
pending output state, now skip the mode commit. Actual enabled/mode/layout/scale/
transform/adaptive-sync differences still apply, as does queued compositor state.
This avoids modesetting desktop monitors during Air-only mode/layout requests.
These compositor changes require a fresh login; no live compositor replacement
or physical stereo negotiation test was performed.


### Measured software eye profile (protocol v5)

`ipd_mm` is the current total separation; `ipd_center_mm` offsets the baseline
midpoint from the configured head centre along head-right. For monocular far PD,
set total = left + right and centre = (right - left)/2. Left camera position is
-total/2 + centre; right is +total/2 + centre. `ipd_baseline_mm` is the saved reset
value. Default centre is zero for profiles without separate measurements.
Total/baseline bounds remain 45–80 mm; centre is limited to ±5 mm as an input
validation bound, not an anatomical/comfort recommendation.

Total adjustments retain the saved asymmetry. Near PD is measurement metadata,
not a mode that changes with panel depth. Perspective uses the fixed far-view
eye positions for all content distances. The native renderer and both-eye
visibility checks use the same rotated baseline; central pointer rays/canonical
panel geometry are unchanged. The optional check shows derived left/right
values and reset baseline. Protocol v5 carries centre/baseline with total.

The existing supervisor publishes the current manifest preference to the private
runtime `.ipd` sidecar (compositor PID, monotonic timestamp, total, centre, reset
baseline, acknowledged control value). Native polls at 250 ms, accepting only the current owner, fresh
publication and finite bounded values. Only changed tuples replace local values;
old repeated heartbeats cannot undo a key before persistence. A publication
must acknowledge the latest local control value, so even a delayed response to
an earlier key cannot replace a newer adjustment. Changed profiles
require a newly acknowledged scene before clicks. Installer preserves profile
fields and optional measurement metadata across release installs.

This is software view geometry, not a measurement of the tracking sensor's
physical relation to the face, a lens-distortion/FOV calibration, or proof of
perfect optical registration. Camera-phone measurements remain user supplied.
Per-eye poses are separate from per-eye fields of view in the OpenXR model too:
https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrView.html
No undocumented physical lens correction, automatic convergence or toe-in was
introduced. Physical fit/image/comfort acceptance remains outstanding.


### Frame rate and capture performance (protocol v6)

Builds select `debugoptimized` (-O2 plus debug symbols/assertions) for compositor
and presenter. The earlier -O0 binaries made CPU pixel conversion unnecessarily
expensive. Capture buffers now use tightly packed rows (`width * bytes_per_pixel`),
including three-byte RGB. wl_shm permits this. The installed wlroots 0.20.2 GLES
readback uses one glReadPixels when the stride equals its packed stride, but one
call per row otherwise. The old 12-byte alignment caused 1041/1395 read calls
per capture for observed 1715-pixel-wide windows, severely slowing video/scrolling.
Primary implementation evidence is in the installed source archives:
`/var/cache/distfiles/wlroots-0.20.2.tar.gz` render/gles2/texture.c and
`/var/cache/distfiles/wayland-1.24.0.tar.xz` src/wayland-shm.c; wlroots types/wlr_shm.c
also validates the stride without requiring four-byte alignment.

Presenter accumulates/clips source damage and reuses its conversion allocation.
Unchanged texture dimensions use glTexSubImage2D; first upload, resize or replaced
SHM allocation requires full texture initialization. Mipmaps regenerate after
updates. Format/channel/alpha and padded-input support remain in air-pixels.h;
only capture allocation requests tight rows. 2x supersampling, 8x MSAA where
available and 16x anisotropy are retained.

Head-following widget poses are frame data rather than repeated static scene
changes. Protocol v6 pointer packets specify follow/frozen policy and frozen yaw;
frame feedback records the yaw used for both eyes. Only a complete frame for the
current scene with a newer timestamp updates compositor widget geometry. Actual
layout changes still require scene acknowledgment. Picking uses the displayed
widget pose; application geometry remains independent of head movement.

`air presenter: perf` logs every five seconds from presentation feedback: actual
presentation FPS, maximum gap, missed-refresh intervals, and average CPU-side
elapsed frame/upload/swap time. These times include driver waits and are not GPU
timer-query measurements. There is no on-screen FPS banner.

The current presenter prioritizes Waybar and the focused app when uploading
textures. It retains compatible textures during short upload deferrals, samples
head orientation after capture preparation, and does the uploads once per frame
before drawing both eyes. Offscreen content gets lower-rate frame callbacks and
can hold a completed capture in shared memory until it approaches view. The
first texture and replacement sizes still participate in scene readiness.
Presentation feedback adds p95/p99 frame gaps, p95/p99 pose age at scanout and
discarded frame counts. The age measures the received software pose, not
physical motion-to-photon latency.

The compositor logs advertised refresh modes for the active resolution and
paces client callbacks for the mode already selected by the display system.
Air Lab does not change the glasses mode during the automatic entry workflow.
High-refresh physical validation requires the connected Air and its cable path.

An experimental GPU/DMA-BUF staging implementation passed invisible NVIDIA
allocation/copy tests but failed live producer synchronization/copy and fell back.
It was removed. Current performance improvement is the packed SHM path, not a
claimed production DMA-BUF implementation. Physical measurements are workload
samples, not a guarantee of zero missed frames across all applications.


### Game loading stalls and watchdog recovery

Loading screens were observed to trigger tracking/presentation watchdog exits,
not a fullscreen eligibility or pointer-grab failure. A renderer GPU stall can
stop both drawing and its old after-draw pose reports; a compositor GPU stall
can delay receipt of healthy reports. These do not require a mode transition.
`air-watchdog.h` retains the scene for a bounded graphics pause while preserving
normal pose-loss recovery when scanout remains live. Taking glasses off, output
loss, lock, presenter exit/disconnect, unsupported grabs and Super+Alt+X continue
using their normal immediate paths. A permanently stale pose or presentation
exits after at most ten seconds. No old timestamps are renewed to prolong this.

During the pause, the compositor skips periodic scene/pointer traffic, consumes
mouse motion and rejects new presses/scrolls. Held release handling remains in
the existing path. Resume requires both pose and presentation younger than 250
ms, and existing scene/capture acknowledgment still gates new actions.

The renderer reports the actual owned UDP pose before GPU work and without
waiting for frame callbacks, at most every 8 ms. After a poll gap of 250 ms,
queued datagrams are drained without aging them as new samples; the next new
packet restores the pose. Tracker sequence-reset detection still applies to the
drained queue. Receiver pose freshness includes the IPC send timestamp and age,
so a delayed queued report cannot become fresh on receipt. Pose messages are
replaceable under socket backpressure and stop queuing above 16 KiB to leave
room for reliable viewport/presentation messages. Real socket failures still
terminate the helper. Display feedback checks the pose actually used for that
frame, not a newer sample received later, before asserting completeness.

The update changes compositor recovery behavior and requires a fresh login.
Focused tests simulate loading/recovery, genuine tracking loss, bounded hangs,
packet backpressure, stale queues and input gating without opening game/test
windows. Actual game-loading acceptance remains a separate physical check.
