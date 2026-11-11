# Air Spatial Lab

## Start here

Read `README.md`, `DESIGN.md`, `docs/NATIVE.md`, and `docs/STATUS.md`. Inspect Git
status and remotes before edits. This independent repo is `/home/work/projects/air-lab`;
workstation notes and the earlier raw probe are in the separate os-agent repo.

The user authorized native integration and system configuration. The native
implementation is built, tested in nested sessions, and installed. After reboot
on 2026-11-08 the patched compositor ran, and the user confirmed visible spatial
windows after the NVIDIA BGR888 capture fix. Recheck the
actual running compositor via `pgrep -a -x labwc` and `/proc/<pid>/exe`, not just
`labwc --version` (PATH still selects stock `/usr/bin/labwc`). Physical input,
power and recovery acceptance checks remain outstanding; see `docs/NATIVE.md`.
Do not equate synthetic/nested test results with physical Air validation or
completion of every design gate.

The smaller-panel/persistent-preview/level-horizon update changes labwc itself;
it requires a fresh login to activate. Keep both sides' geometry consistent—do
not scale only the renderer to simulate changing panel size in the old session.
`panel_scale` defaults to 2/3; `head_roll` defaults false. The latter deliberately
levels the camera horizon after neutral conversion, not a sensor-axis correction.
`AIR_FRAME_PRESENTED` reports actual scanout liveness separately from a complete
scene acknowledgment; incomplete scenes revoke new-click eligibility. A healthy
preview has no session-duration timeout. Feed/renderer watchdogs still apply.
Image-copy capture does not use `wl_buffer.release`: ready/failed completes
ownership. Preserve the completion-handler regression in `test_capture_completion.py`;
otherwise a resize failure can strand a buffer and stop subsequent captures.

Spatial controls/widgets use private protocol v8 and require a matching compositor/
presenter plus a fresh login. `air-math.h` owns origin-facing planes (including
elevation), separate X/Y pixel scales, capture-group frame bounds and grab-point
depth math. Dragging a title bar moves that window and its descendants.
A transient keeps an independent offset from its immediate parent's current
spatial pose, with a 15 mm default depth offset toward the viewer. Scrolling
during a title drag adjusts that window's depth. Frame drags request actual
client sizes about the window centre through labwc's configure path. Corners
preserve aspect ratio to pixel rounding and straight edges resize one axis.
Respect size hints and keep logical-pixel density steady as the app reflows.
On exit, restore saved floating-window rectangles after output power recovery.
Move windows from the glasses to a usable regular monitor when available.
Native-owned button releases must be consumed even after mode exit
or view destruction. Identify
grab targets by creation ID/workspace, not retained view pointers. Keep popup/
shadow capture extents inside the frame and head pose out of manipulation math.

Workspace layouts persist to the desktop user's XDG state directory. The
versioned atomic file contains titles and saved poses, separate from desktop
restore snapshots. Keep overview input compositor-owned and require its
displayed revision for mouse selection. Spatial pointer DnD uses the normal
serial-validated wlroots grab. Require a visible matching target at release and
cancel the drag before forced seat release. Game Focus limits presentation to
one app group. Pointer lock retains relative input for the focused owner and
never uses a flat desktop warp while spatial is active.

User preference: only explicit Super+Alt+X state changes notify via Mako
(app-name Air Spatial Controls); no tracker/readiness notices or artificial
one-minute warmup.
Supervisor/handoff publish connection, calibration and owned source-port state;
renderer startup requires those live prerequisites but ignores legacy `ready_at`
deadlines. Upstream initial calibration still takes roughly 15 seconds before
its usable feed; retain actual tracking-loss/source ownership checks. The
installer silences Air notices from an older running supervisor via an Air-only
mako rule, with a rollback backup. Do not restart tracking to apply this policy;
refreshing the helper launcher is enough when compositor hashes match.
Spatial XDG popup constraints use parent-window coordinates, not sleeping output
usable areas. Synchronize seat coordinates before presses and after summon.
Keep the real right-click/action/Escape/outside-click test. Test input timestamps
must be monotonic and reflect waits, or GTK menu timing produces false failures.

Automatic preview requires the identified Air display plus its internal OLED-on signal (`auto_start`, default
true in the session manifest). `air-auto.h` continuously reconciles connected,
unlocked glasses with spatial mode. Appearance/unlock debounce for 500 ms;
failures retry after 5 seconds if the display remains usable and auto is enabled.
No confirmation shortcut is required: tracking and a complete scene must be
healthy before routing input/sleeping monitors. Both login and later locks stop
entry while locked; unlocking resumes automatically when enabled. Super+Alt+X
toggles the in-session automatic-mode preference. Disabling immediately restores
the desktop/cancels arming and stays off across lock and glasses events until
X enables it again. Super+Alt+Escape has no spatial binding. `auto_start` is the
login default; runtime toggles do not edit it. Output loss
retains scene-output destruction before restoration; put-on/take-off detection
uses CRC-checked MCU screen events from the existing tracker. `air-screen.h`
also provides shared report validation: MCU replies/events may be short valid
reports, not necessarily padded to 64 bytes. Command receive skips unrelated
reports with a bounded wait and observes OLED events before matching reply IDs.
`xreal-controller-poll.patch` queues requested modes atomically, separately from
confirmed poll results; ordinary MCU reads must not overwrite requests or make
an unconfirmed write appear successful. Successful MCU reconnect restores the
device's supported SBS capability after a previous failed connection.
`air-screen.h`
checks reader PID/freshness in `tracker.json.screen`; unknown/off prevents auto
entry and restores an active mode. `air-screen-publish.h` taps all existing MCU
reads, including short command replies, without a second HID/IMU reader. Original
Air opening is reported by the `Open OLED 2D` firmware log; off also emits 6c04.
Initial state remains unknown until an event: no verified current-state query is
available. Physical mode/power acceptance remains outstanding. Test with
`AIR_NATIVE_TEST_AUTO_START=1 python3 scripts/check-native.py` for initialization,
tracker readiness, override, removal/reappearance and desktop-client survival.

Floating OS widgets use compositor-owned temporary Waybar rows and a private
widget-capture manager (protocol v6). `air-widgets.c` owns layer capture/popups,
process lifetime and the same-user `wofi` allowlist during active mode. Hide the
flat widget scene to avoid duplicate bars. The dock follows yaw only at .6096 m
radial distance; centre height is -.3048 m, other rows ±.045 m. Freeze following
through pointer/popup grabs and cancel held releases when a widget disappears.
Widget yaw is sampled once per rendered frame in SPATIAL; keep it fixed in
ARMING and through button/popup grabs. Protocol v6 frame feedback carries the
actual displayed yaw. Compositor adopts that pose for picking without a new
static scene epoch or head-induced focus/motion. Readiness/picking never advance
head-following themselves. Reject stale/out-of-order frame pose timestamps.
The automatic-widget integration injects continuous pose variation at startup.
Head-only widget revisions must not synthesize client focus/motion; application
panels stay head-independent. Keep renderer and picking on the same committed
geometry. `start-native-widgets.py` reuses the user's JSONC modules/styles without
editing their desktop bar. Widget processes are ephemeral, restarted after a
crash and removed on exit/lock. `AIR_NATIVE_TEST_WIDGETS=1` exercises actual
Waybar/wofi actions with harmless fixtures; add the GLES renderer path for capture.
`prepare_capture` must retry even without a dirty texture after wanted becomes
true again, or closing a static popup can strand capture and block all clicks.

Pointer gain comes from the renderer viewport width at the fixed 40° horizontal
FOV, not the old .0012 radians/count. `amouse_move` integrates angular elevation
and elevation-compensated horizontal travel without head input, including packet
splitting and vertical limits. Empty-space pointer positions stay on the cylinder.
The displayed glyph uses constant viewport pixel size with its hotspot at the
world hit. Super+Alt+Minus/Equal changes speed in-session; Super+Alt+0 restores
the configured `pointer_speed` (default 1, range .25–3). Viewport resize forces a
new scene acknowledgment before clicking. These compositor changes need login.

Renderer quality: no visible status/HUD banner; AIR_STATUS is log-only, including
messages from older compositors. Render buffers use 2x resolution per axis with
Wayland buffer_scale=2 while logical viewport, picking and pointer gain stay
unchanged. Prefer 8x EGL MSAA, falling back to 4/2/0 as supported. `air-filter.h`
uses trilinear mipmaps (including NPOT where supported), maximum advertised
anisotropy and high-precision UVs; regenerate mipmaps only on texture uploads.
Titles use 2x Cairo rasterization with grayscale AA, not display subpixel AA.
The invisible EGL-device test checks actual shaders, minified checker content,
resized NPOT colours and diagonal coverage without any desktop/sensor connection.
Do not confuse that with optical/performance or Wayland presentation acceptance.

Spatial cursor visibility is compositor-owned: video/CSS hide requests, blank or
unsupported buffers and destroyed cursor surfaces select a visible theme/default
arrow (with a built-in fallback). Keep commit listeners on pending/transparent
client surfaces so their later images can restore. Route cursor-shape requests
through the spatial path, preserving focus checks and ordinary desktop behavior.
`air-cursor.h` checks image alpha only, ignoring row padding; implicit-opaque
XRGB/XBGR cursors must retain opaque alpha and correct channel mapping. Presenter
hide/blank packets retain the existing spatial glyph. Quiet regression compiles
the real routing functions against mocks, without browser/input/Wayland tests.

Stereo uses packet protocol v6 and follows the glasses' physical mode.
Production never sends SBS mode writes or changes Air's output configuration.
`native_stereo.py` publishes actual owned mode and persists IPD; its transaction
helper is diagnostic opt-in (`switch_modes=True`) only. Confirmed 2D/1920x1080
uses mono; confirmed SBS/3840x1080 uses parallel per-eye 1920x1080 rendering.
Require scale 1/normal transform; unknown or mismatched hardware/transport waits.
A mode change during active use restores desktop before a matching next entry.
Stereo disabled in the manifest requires physical 2D. The user confirmed manual
3D + automatic spatial rendering is visible; USB-triggered 3D was black even
without the coupler or monitor sleep. The selected policy preserves the hardware
mode through exit/lock; do not reintroduce mode writes under normal automation.
Output-management application skips fully unchanged heads without pending state
so Air hotplug doesn't gratuitously modeset desktop monitors.
IPD defaults 64 mm, bounded 45–80 mm, Super+Alt+, / . adjusts 0.5 mm, backslash
restores the saved baseline, Super+Alt+I opens/closes an optional guide (no persistent HUD). The
supervisor merges IPD into the latest manifest; runtime toggles still don't alter
auto_start. Parallel eye offsets follow head-right; cursor and picking remain
cyclopean with visibility required in both eyes. IPD adjustments show a 2.5-second renderer-confirmed value; the optional guide
has 0.6 m/3 m depth targets and fixed blue references. Never instruct users to
align these: this is a rendering check, not IPD measurement/calibration. All guide
corners must fit the per-eye frustum at 45/80 mm. This is depth disparity, not optical sharpness or
physical lens movement. IPD changes need a new scene
acknowledgement; calibration blocks new presses. Per-eye MSAA/SSAA uses isolated
viewports/scissors and one presentation acknowledgement only after both eyes.
Quiet GPU disparity/maths tests and mocked mode transactions are not physical
stereo/comfort acceptance; neither optics calibration nor lens IPD is claimed.

Air now uses a normal extended desktop (`reserved_display`, default false), as
requested after the black reserved output complicated visual recovery. The
manifest is false; existing session policy needs next login to change. Normal
Air placement is x=3440 in Single and x=6880 in Dual. Optional opt-in reservation
remains implemented for explicit configuration only. When enabled, a
compositor-owned black scene covers ordinary desktop/layer content; only the
private presenter is mirrored above it, with session-lock trees above both.
Update this ordering inside output-frame before composition, not only a timer.
Clean up mask/mirror on output loss/shutdown; surface destruction clears mirror
references. Ordinary placement avoids Air while desktop outputs are usable;
on desktop return, Air windows/pointer migrate to a usable ordinary display.
If Air is the only usable display, retain its desktop as recovery fallback.
This is a connected but visually reserved output, not DRM leasing/non-desktop.
Kanshi no longer has dual-xreal. Each desktop choice has an Air-connected and
`-unplugged` variant; every connected head must match a directive. Omitting Air
causes `(none)` and can leave its output disabled. Do not use an optional-head
directive: kanshi retains a matching current profile on hotplug without applying
it to the new head. Connected variants enable Air beside the desktop without prescribing
its mode, preserving session-owned SBS transitions. Waybar maps both variants
to Single/Dual; its helper keeps the chosen pair first across hotplug.
System edits are backed up under `.local/rollback/desktop-monitors-*`; installer
script stages config/scripts without a live reload. Toggle notices use a separate
app name from the suppressed readiness source; only explicit X toggles emit them.

## Layout

- `native/common/`: shared private protocol and world-space math.
- `native/labwc/`: compositor integration source; optional build feature.
- `native/presenter/`: native Wayland/EGL/GLES mono presentation and SHM capture.
- `native/tests/`: geometry, GTK client, and disposable nested test tools.
- `patches/labwc-native-spatial.patch`: hooks into pinned labwc 0.20.3.
- `scripts/build-native.sh`, `check-native.py`: build and end-to-end validation.
- `scripts/start-native-*.py`: session/renderer lifecycle and calibration readiness.
- `scripts/install-native-session.py`, `restore-stock-session.py`: reversible
  next-login selection; never restart the current desktop from these scripts.
- `scripts/build-tracker.sh`, `run-tracker.py`: pinned XRLinuxDriver, UDP-only.
- `.local/`: ignored builds, upstream, immutable releases, install manifest,
  rollback backups, logs, captures and test evidence.

Native implementation lives in tracked `native/` files. Build scripts copy these
into the ignored upstream checkout. Keep hooks in the tracked patch reproducible;
never leave the only copy of a fix in `.local/upstream/`. Preserve upstream licenses.
No remote/push destination or project license has been selected.

## Validation

User preference (2026-11-08): long graphical nested tests are intrusive. Do not
run them again without explicit permission. Prefer quiet focused checks; report
any resulting validation limit instead of opening test windows automatically.

Run `python3 -m unittest -v`, `sh scripts/check-tracker.sh`, native build/geometry
tests via `sh scripts/build-native.sh`, and `python3 scripts/check-native.py` for
native changes. Build the test tools with `sh scripts/build-native-test-tools.sh`.
Also check the feature-disabled labwc build when changing integration hooks.
Run `AIR_NATIVE_TEST_RENDERER=gles2 python3 scripts/check-native.py` too for capture
changes: this NVIDIA GLES renderer offers BGR888 SHM, unlike the Vulkan test path.
Preserve three-byte pixels, padded row stride, RGB/BGR ordering and opaque alpha.
`AIR_NATIVE_TEST_SOAK=130` checks more than two minutes in healthy active
mode; it can be combined with `AIR_NATIVE_TEST_RENDERER=gles2`.
The timestamp rebase regression fails against original upstream sensor code and
passes with our patch; preserve it when upgrading the tracker.

The native integration runner opens sacrificial nested outputs and uses a unique
process marker. Input targets only that socket. Its disconnect fixture closes
only the unique parent window with app-ID `labwc` and title `labwc - WL-2` and
refuses ambiguity. Do not point its input/lock tools at the production socket.
Physical output teardown must destroy the disappearing scene output before
native recovery changes scene nodes, preserving upstream's listener ordering.

Normally install binary hashes matching the most recent passing integration run.
With the user-requested no-graphical-tests workflow, `check-native-focused.py`
records quiet checks and exact binary hashes. Its optional
`--workspace-headless` uses isolated headless outputs and checks live GTK
input without opening windows on the real desktop. The installer option `install-native-session.py
--focused-validation PATH` explicitly records limited validation. Never label it
a graphical integration pass.
Inspect `.local/native-install.json` for installed paths/rollback. The user's
`~/.local/bin/start-labwc-logged` selects the frozen native session on next login;
`~/.local/bin/air-spatial-stock` or `AIR_USE_STOCK=1` selects stock for a session.
`restore-stock-session.py` restores the prior launcher while preserving newer edits.
For renderer-only updates, `refresh-native-presenter.py` can redirect the running
session's old helper launcher to the tested new release. It requires identical
compositor hashes, backs up the launcher, and records the redirect in the install
manifest; it never rewrites a running binary or restarts desktop clients.
Never log out the user merely to validate a build; they must save work for the
one initial compositor login required by the design.

## Workstation and tracking

Verified baseline 2026-11-07: `spectre`, Brian, Gentoo/OpenRC, NVIDIA, original Air
`3318:0424`. Stock labwc/wlroots 0.20.2; native build pins labwc 0.20.3 commit
`e0a3970b0c70e321814e8004ff7c6dbc7aeaf884` against installed wlroots 0.20.2.
Observed Air DP-4 at 1080p/60, extended right of Dell/LG. Recheck identities live.
Existing udev and kanshi setup worked without changes. No packages were installed.

Use one IMU reader. The session supervisor owns tracking; there is no added stationary-warmup timer
or readiness notification.
Recenter is not bias calibration. Initial severe incoming-pose drift was reduced
by timestamp rebasing, but the 2026-11-08 native test found another cause: XREAL
connection blindly averages the first 1,000 samples into calibration. Motion at
connection can create a false bias above FusionOffset's 3°/s acceptance threshold;
waiting later cannot recover it. Preserve factory calibration with
`patches/xreal-preserve-factory-calibration.patch`, tested by compiling the actual
connection function against mocks. That regression fails on original upstream.
Resetting the old reader while stationary helped temporarily, but a reconnect
ran its bad calibration again. `restart-native-tracker.py` now provides a live
handoff to a fixed release; it stops the prior reader before starting the new
one and monitors compositor/supervisor pidfds for cleanup. Check `live_tracker`
in `.local/native-install.json`; the original session supervisor still remembers
its exited child. The handoff publishes the replacement's readiness/source port.
The fixed reader's 35-second stationary yaw change was -0.0088° (0.0179° range),
versus +294.887° beforehand. Longer checks remain outstanding. Renderer sequence
reset detection exits spatial mode if the driver's orientation reference resets,
rather than retaining an obsolete neutral frame and flipping the workspace.
The user confirmed upright, steady windows after looking around and holding
still with the fixed reader. Preserve this distinction from the older fixes.
Native tracking is
pinned to the UDP socket owned by the session child, not an arbitrary local sender.
The wrapper disables
metrics and directs upstream's automatic license HTTP requests to a closed local
proxy. Expected license errors are documented. Its inherited project lock prevents
another launcher from rewriting a running reader's config.

The older probe/runbook is `/home/brian/projects/os-agent/deploy/xreal-air/README.md`.
Stop the session-owned reader before running that diagnostic. Read applicable
os-agent guidance before editing that separate repo. Run interactive programs
with `setsid -f`; keep graphics environment overrides confined to nested tests.

## Invariants

Head orientation changes application projection only; the explicit OS widget
class also follows yaw through committed geometry. Mouse deltas change a workspace-fixed
pointer; clicking chooses keyboard focus. Renderer and picking use a committed
scene epoch; no new clicks with stale tracking or an unpresented revision.
Original clients stay in one labwc. Corner resizing updates canonical client geometry. A private
presentation surface never gets normal focus or enumeration. Compositor-owned
escape/watchdog restores outputs and held-button state even with a hung helper.
Lock tears down capture and blanks the path. Auto-start may resume after unlock
only with confirmed internal OLED-on state and fresh tracking/scene readiness.

Never use wildcard power-off, public user-space pointer injection, or a second
compositor as a way to migrate existing production windows. Physical stereo acceptance, DMA-BUF
optimization, full decorations/DnD/XWayland input support and physical daily-toggle
acceptance are remaining gates, not claimed outcomes. Keep durable verified facts
in the owning docs and this file concise.

Latest live recovery: firmware-version replies may be shorter than their 41-byte
buffers; zero-pad strings while requiring scalar data. Full-length MCU async
logs may have no NUL; use validated declared lengths, never strlen/disconnect.
Wear 0x6c05 events override OLED logs once observed, because SBS itself closes
OLED. `restart-native-tracker.py` now owns stereo too and pauses the original
supervisor for the handoff, resuming it on exit/failure. Recheck live ownership
in the manifest; never resume a paused original supervisor while its handoff
still owns the reader/controller. See latest STATUS entry for evidence and limits.
Original Air mode-write framing was compared with a full report-id-0 HID frame
and four-byte little-endian value. That change passed wire-format unit checks
but did NOT fix the physical black screen; do not present it as a proven fix.
The user verified manual 3D plus automatic spatial mode displays correctly;
old software-selected 3D stayed physically black even with desktop monitors on.
Reference control framing: nyan-real-firmware-kit xreal/display.py and glasses.py.
Mode 0x07 reports the DP link state; ACK/readback plus a DRM screenshot does not
prove an optical image. Validate the software transition physically, with primary
monitor awake and automatic restoration, before claiming stereo transport works.


Software eye profiles now carry total, midpoint offset and reset baseline over
protocol v6. Per-eye positions are centre ±total/2; centre=(right PD-left PD)/2.
Rendering and both-eye visibility share `aeye_offset_center`. Existing profiles
remain symmetric; measured profiles retain their asymmetry through total edits.
`ipd_baseline_mm` is the reset target; near PD is metadata only. Supervisor
publishes owned/fresh `.ipd` settings; compositor applies changed tuples only so
old heartbeats cannot undo unsaved key adjustments. New compositor requires
fresh login; do not redirect a v4 live helper to a v5 renderer. See NATIVE.md.
Phone PD cannot establish perfect optics or tracker-to-face registration; do not
claim software profile equality proves optical calibration. Personal measurements
are stored only in the ignored install manifest, not repository documentation.


Performance: `build-native.sh` selects debugoptimized (-O2 with debug symbols)
for both native components. Use packed `width*bpp` SHM rows: wlroots GLES falls
back to one glReadPixels per row when stride differs, which collapsed active
video/scrolling FPS for 1715-pixel windows. Do not reintroduce RGB row padding.
CPU conversion still accepts externally padded rows. Capture upload uses source
damage, persistent staging memory and glTexSubImage2D for unchanged texture size;
resize/replaced buffers require a full upload. Mipmaps/AA stay enabled. Renderer
logs actual presentation FPS/gaps and CPU-side frame/upload/swap times every 5 s,
without a HUD. Native checks remain quiet; no graphical tests without permission.
The attempted DMA-BUF path passed invisible allocation tests but failed live
synchronization/copy and was removed. The working packed SHM path measured near
60 FPS during user-reported video/scrolling; do not claim a deployed GPU capture
path. See latest STATUS evidence and installed release before further tuning.


Loading stalls: `air-watchdog.h` preserves a spatial scene during a graphics pause
for up to ten seconds from stale pose/frame timestamps, while new pointer input
is blocked. Pose loss with live frames retains 700 ms recovery. Resume needs both
fresh tracking and actual presentation; never fake liveness timestamps. Lock,
OLED/output loss, helper failure, manual X and unsupported grabs remain normal
recovery paths. Pose reports run before GPU drawing independently of callbacks;
discard queued UDP after long poll gaps and include IPC queue time in freshness.
Pose backpressure is lossy/bounded, critical messages remain reliable. Frame
completeness must use the pose actually drawn at its presentation time. This
compositor change needs login; see STATUS for installed evidence/acceptance.

Temporary reading zoom uses packet protocol v7 (matching compositor/presenter,
fresh login). Super+Alt+vertical scroll magnifies 1–3×; releasing either modifier
resets to 1× via the modifier event hook, without another scroll. This narrows the
frustum, not camera translation. Keep both-eye visibility, pointer gain and cursor
pixel size consistent; factor changes need a new presented scene acknowledgment.
Zoom/reset must not synthesize client focus/motion. No graphical tests without
permission; preserve the quiet controls and invisible GPU snap-back regressions.

Head-pose smoothing is presenter-owned in `air-pose.h`: speed-adaptive quaternion
filtering before neutral/level-horizon conversion. Filter the newest pose once per
receive pass; queued-packet drain timing is not sensor timing. Recenter/first
sample/250 ms gaps reset directly. Keep true sensor freshness, source ownership,
sequence reset and watchdog rules; idle polling must not evolve or refresh pose.
Shared filtered orientation drives rendering, reported visibility and widget yaw.
Synthetic jitter/turn checks are not physical heartbeat/comfort acceptance.
