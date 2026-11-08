# Air Spatial Lab

## Start here

Read `README.md`, `DESIGN.md`, `docs/SETUP.md`, and `docs/STATUS.md`.
Inspect Git status and remotes before edits. This is an independent repository
at `/home/work/projects/air-lab`, not part of the adjacent os-agent repository.

The current deliverable is the browser interaction/capture experiment for the
original XREAL Air. `DESIGN.md` is the future native labwc contract. Keep observed
prototype behavior separate from planned features; do not start compositor or
monitor-power work until the user wants to proceed beyond baseline validation.

## Layout and commands

- `bridge.py`: loopback HTTP/SSE/UDP bridge; Python standard library only.
- `index.html`: browser projection, cylinder pointer, picking, controls.
- `test_bridge.py`: synthetic packets and stub grim integration tests.
- `scripts/build-tracker.sh`: pinned local upstream XRLinuxDriver build.
- `scripts/run-tracker.py`: isolated project config, UDP-only tracker.
- `scripts/run-lab.py`: paired process lifecycle and browser launch.
- `patches/`: reproducible fixes against the pinned tracker dependencies.
- `docs/`: durable setup, evidence, design review and outstanding gates.
- `.local/`: ignored archive, upstream checkouts, builds, logs, screenshots,
  private URLs and transient window identifiers. Never commit captures or tokens.

Run `python3 -m unittest -v`; for launcher changes also check shell/Python syntax
and exercise start/stop. Changes to geometry/pose handling need browser checks
that head turns preserve the pointer's panel hit coordinate, plus physical
direction/readability checks. Do not claim synthetic tests validate hardware.
Use the commands and dependencies in `docs/SETUP.md`; no application package
installation is needed for the basic simulator.
After a tracker build, run `sh scripts/check-tracker.sh`. Its synthetic timestamp
regression fails against the original upstream integration and passes with the
local timestamp-rebase patch. Preserve that patch and its test when upgrading.

## Workstation facts

Verified 2026-11-07: `spectre`, Brian, Gentoo/OpenRC, Wayland/labwc 0.20.2,
wlroots 0.20.2, NVIDIA. Original Air `3318:0424`; observed DP-4 at 1080p/60,
extended to the right of Dell and LG. Recheck USB and output identities live.
The existing scoped udev rule and kanshi `dual-xreal` profile worked without edits.
`grim -T` and compositor capture protocols work; `lswt`, `wayland-info` and a
globally installed `xr_driver_cli` were absent. Tracker is local, not a service.

The earlier hardware baseline is
`/home/brian/projects/os-agent/deploy/xreal-air/README.md`; the raw probe is
external and should only run with this lab's tracker stopped. Other workstation
notes live in that repo's `kb/system/`. Read applicable guidance there before
changing files in that separate repository.

Use one IMU reader at a time. Keep upstream pinned until a deliberate upgrade.
Leave the Air motionless on a table through the launcher's 60-second warmup;
early UDP availability is not settled bias calibration. The first live preview
showed severe incoming-pose drift; see the measurements and timestamp fix in
`docs/STATUS.md`. Do not claim long-term drift stability from a brief test.
After the patch and warmup, the user confirmed the continuous rotation was gone
and the view stayed steady. Longer design-gate drift tests remain outstanding.
The tracker wrapper disables metrics and directs its otherwise automatic
license HTTP requests to a closed local proxy; expected license errors are
documented. Do not casually replace this with upstream's default mouse mode.
Run interactive programs detached with `setsid -f`; never restart the live
desktop as a routine validation step.

## Invariants

Head rotation changes projection only. Mouse movement updates the workspace
pointer independently. Looking alone must not move the Wayland pointer or focus.
The prototype never forwards clicks to real applications or controls outputs.
For the native solution, clients must remain connected to their original labwc;
seat routing, lock handling, escape recovery and output changes belong there.
Keep physical monitors enabled during the preview. Do not introduce wildcard
power-off, global graphics environment variables, or production compositor
replacement as setup shortcuts. Follow the delivery gates in `DESIGN.md`.

Record durable verified changes in the owning docs; keep this file concise.
No remote/push destination or project license was chosen during initial setup.
