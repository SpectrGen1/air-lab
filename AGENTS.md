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

Spatial controls use private protocol v2 and require a matching compositor/
presenter plus a fresh login. `air-math.h` owns origin-facing planes (including
elevation), separate X/Y pixel scales, capture-group frame bounds and grab-point
depth math. A native outer title bar moves the root group around the cylinder;
wheel while held changes radial distance. Edges/corners resize physical spans
about their centre, never the canonical client rectangle. Native-owned button
releases must be consumed even after mode exit or view destruction. Identify
grab targets by creation ID/workspace, not retained view pointers. Keep popup/
shadow capture extents inside the frame and head pose out of manipulation math.

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
- `bridge.py`, `index.html`, `test_bridge.py`, `scripts/run-lab.py`: retained browser
  experiment, independent from native integration and never real input injection.
- `.local/`: ignored builds, upstream, immutable releases, install manifest,
  rollback backups, original zip, private URLs, logs, captures and test evidence.

Native implementation lives in tracked `native/` files. Build scripts copy these
into the ignored upstream checkout. Keep hooks in the tracked patch reproducible;
never leave the only copy of a fix in `.local/upstream/`. Preserve upstream licenses.
No remote/push destination or project license has been selected.

## Validation

Run `python3 -m unittest -v`, `sh scripts/check-tracker.sh`, native build/geometry
tests via `sh scripts/build-native.sh`, and `python3 scripts/check-native.py` for
native changes. Build the test tools with `sh scripts/build-native-test-tools.sh`.
Also check the feature-disabled labwc build when changing integration hooks.
Run `AIR_NATIVE_TEST_RENDERER=gles2 python3 scripts/check-native.py` too for capture
changes: this NVIDIA GLES renderer offers BGR888 SHM, unlike the Vulkan test path.
Preserve three-byte pixels, padded row stride, RGB/BGR ordering and opaque alpha.
`AIR_NATIVE_TEST_SOAK=130` checks more than two minutes each in healthy preview
and active mode; it can be combined with `AIR_NATIVE_TEST_RENDERER=gles2`.
The timestamp rebase regression fails against original upstream sensor code and
passes with our patch; preserve it when upgrading the tracker.

The native integration runner opens sacrificial nested outputs and uses a unique
process marker. Input targets only that socket. Its disconnect fixture closes
only the unique parent window with app-ID `labwc` and title `labwc - WL-2` and
refuses ambiguity. Do not point its input/lock tools at the production socket.
Physical output teardown must destroy the disappearing scene output before
native recovery changes scene nodes, preserving upstream's listener ordering.

Only install binary hashes matching the most recent passing integration run.
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

Use one IMU reader. The session supervisor owns tracking and waits a full minute
of instructed stationary warmup after USB connection before native presentation.
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

Head orientation changes projection only. Mouse deltas change a workspace-fixed
pointer; clicking chooses keyboard focus. Renderer and picking use a committed
scene epoch; no new clicks with stale tracking or an unpresented revision.
Original clients stay in one labwc, with canonical rectangles retained. A private
presentation surface never gets normal focus or enumeration. Compositor-owned
escape/watchdog restores outputs and held-button state even with a hung helper.
Lock tears down capture, blanks the path and requires explicit re-entry.

Never use wildcard power-off, public user-space pointer injection, or a second
compositor as a way to migrate existing production windows. Stereo, DMA-BUF
optimization, full decorations/DnD/XWayland input support and physical daily-toggle
acceptance are remaining gates, not claimed outcomes. Keep durable verified facts
in the owning docs and this file concise.
