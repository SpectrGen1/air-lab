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

Leave the Air motionless on a table for a minute after login or USB reconnection.
Tracking starts with the session and is stopped when that session ends. A
readiness file under the private user runtime directory tracks connection,
calibration, warmup and the child-owned UDP socket; requesting spatial mode early
waits for this warmup. The renderer pins its connected receiver to that reserved
source port and validates packet sources, rather than accepting arbitrary local
UDP senders.
Do not start a second IMU reader or the old browser pose bridge alongside it.

| Shortcut | Action |
| --- | --- |
| Super+Alt+X | Start native preview; cancel an arming preview; exit active spatial mode |
| Super+Alt+Return | Confirm the first healthy preview: route input and sleep previously-on non-Air monitors |
| Super+Alt+Escape | Force desktop, even if a client inhibits ordinary shortcuts or the renderer hangs |
| Super+Alt+R | Recenter the viewing direction without moving the world pointer |
| Super+Alt+F | Summon the pointer into the current view |
| Super+Alt+Arrow | Move the focused panel in the spatial workspace |
| Alt+Tab / normal workspace keys | Use normal labwc focus/workspace switching |

The presenter shows confirmation instructions on the glasses. Later entries
in the same compositor can activate automatically after a healthy preview;
a second toggle during arming still cancels it. Locking always exits spatial
mode, and unlocking does not re-enter it. Re-entry requires the explicit toggle.

Alt+Tab changes focus but does not bring the selected panel into the viewing
direction. Turn toward its position or use Super+Alt+Left/Right to move it.
Face forward and use Super+Alt+R if the neutral view was set while the Air rested
on the table. A panel locator remains a usability improvement to implement.

The configured connector is DP-4, but the compositor verifies the Air display
identity and USB `3318:0424`, and can find the same Air after connector renaming.
It refuses a non-extended/overlapping physical layout. Wayland-backend outputs
are allowed separately for disposable nested tests.

## Architecture

- **Patched labwc 0.20.3** owns views, original rectangles, focus, real seat input,
  per-workspace poses, input regions, grabs, mode transitions and output recovery.
  Its source revision is `e0a3970b0c70e321814e8004ff7c6dbc7aeaf884`.
- **air-presenter** is a native Wayland/EGL/GLES mono renderer. It is a layer-shell
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

Original window geometry, maximization and workspace state remain canonical
and are not rewritten to arrange panels. Minimized windows are excluded. Panel
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

This is the first native integration, not completion of every delivery gate.
Mono presentation is implemented. Stereo/eye calibration and OpenXR/DRM leasing
are separate milestones and are not enabled.

Normal client clicks, typing, scrolling, XDG popups, transient dialogs and text
selection work through the existing seat. Server-side decoration capture/input,
spatial drag-and-drop, pointer lock/confine, tablet/absolute input and compositor
move/resize gestures are not complete native features. Move/resize requests,
DnD and pointer constraints explicitly return to the desktop; repeat that gesture
there. Use the panel-move shortcuts to reposition a panel without changing the
original desktop rectangle. XWayland override-redirect popups, large/complex
subsurface trees and unusual cursor buffer transforms still need their own
validation. Do not claim these feature gates passed from the GTK test.

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

The geometry unit test covers head/pointer independence, cylinder seam,
back/parallel rays, unbounded/fractional coordinates and transient offsets.
The labwc patch also builds with `-Dair-spatial=disabled`.

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
