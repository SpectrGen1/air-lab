# Air Spatial Lab

## Start here

Read `README.md`, `DESIGN.md`, `docs/NATIVE.md`, and `docs/STATUS.md`. Inspect Git
status and remotes before edits. This independent repo is `/home/work/projects/air-lab`;
workstation notes and the earlier raw probe are in the separate os-agent repo.

The user authorized native integration and system configuration. The native
implementation is now built, tested in nested sessions, and staged for the next
login. The original running desktop was deliberately not restarted. Recheck the
actual running compositor via `pgrep -a -x labwc` and `/proc/<pid>/exe`, not just
`labwc --version` (PATH still selects stock `/usr/bin/labwc`). Physical acceptance
checks after the initial patched login remain outstanding; see `docs/NATIVE.md`.
Do not equate synthetic/nested test results with physical Air validation or
completion of every design gate.

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
by timestamp rebasing and stationary warmup; the user confirmed the browser view
became steady. Longer drift/optical checks remain outstanding. Native tracking is
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
