# Air Spatial Lab

An interaction prototype for the **original XREAL Air + an existing labwc session**. The mouse moves a point on a workspace-fixed cylinder. The ray starts between the eyes. The camera rotates independently. This prototype uses a mono viewport, not OpenXR or calibrated stereo. It never forwards clicks to other programs, changes monitor power, or edits your labwc configuration.

## Local setup on spectre

Verified 2026-11-07: USB sensor communication, extended 1080p/60 display,
real labwc window capture, and head tracking in the glasses. The user confirmed
that the view stays steady after fixing a startup timestamp issue and allowing
full stationary calibration. The tracker is built locally; no service
installation is needed for this preview.

```sh
cd /home/work/projects/air-lab
python3 scripts/run-lab.py                  # simulator + browser
python3 scripts/run-lab.py --pose           # local Air tracker + preview + browser
```

Use one command at a time. Leave the glasses motionless on a table for 60 seconds after
starting tracking; the launcher opens the browser after this warmup. Move the browser to the Air, select **Glasses test · 40°**,
click **Fullscreen**, face forward and press **R**. **Esc** releases the mouse;
**Ctrl+C** in the launching terminal stops the preview and its tracker.
`--no-browser` prints the private URL without opening it. Add `--window ID`
up to three times for read-only previews.

For a fresh checkout, first run `sh scripts/build-tracker.sh` to build the pinned
upstream tracker. See [SETUP.md](docs/SETUP.md) for dependencies, diagnostics and
recovery, and [STATUS.md](docs/STATUS.md) for validation and remaining design gates.
`DESIGN.md` remains the native-integration contract; it is not implemented by
this preview. Local builds, logs, captures and the original archive live under
the ignored `.local/` directory.

The generic XRLinuxDriver commands below apply to an independently installed
driver. The local launcher supplies isolated settings automatically; do not run
those commands or a second tracker alongside it.

## 1 · Try the interaction without a headset

From the extracted directory:

```sh
python3 bridge.py
```

Open the private loopback address printed in the terminal in a desktop browser. No Python packages required. `index.html` can also be opened directly in browsers that permit local scripts; the bridge is required for hardware and capture. `Esc` releases the mouse; `Ctrl+C` stops the bridge.

Click the stage once to capture relative mouse input. Move over a line in the center panel and click again to leave a marker. Without moving the mouse, turn the simulated head with the arrow keys. **Ray** and **Marked** should stay on the same panel coordinate. Turn back; the cursor and marker coincide. `I/J/K/L` move the pointer without capturing the mouse, `Space` marks the current target, `Q/E` roll the simulated head. `F` explicitly summons the pointer to the current view; `R` recenters the view without moving the pointer in the workspace. New clicks outside the viewport are ignored. Coordinates in the readout are on the demo's 960×540 panel, not source-image or Wayland surface pixels.

## 2 · Use an original Air as the camera

Connect the glasses to a port/adapter supporting both video and USB data. Start in **ordinary 2D display mode, extended, not mirrored**. Leave the physical monitors on. Use an existing working XRLinuxDriver installation that offers `--opentrack-app`. A separate OpenTrack application is not needed: this bridge is the UDP receiver. Do not concurrently run a second IMU reader or a head-to-mouse service. If the driver is new to this system, follow the driver's upstream installation/permissions documentation, and verify your version's CLI rather than installing blind.

These commands change the driver's stored output configuration. Record the previous mode, destination, and enabled state so you can restore them afterward. Configure the destination before selecting the output mode. If the installed driver is disabled, enable it separately using `xr_driver_cli -e`.

```sh
xr_driver_cli --opentrack-app-ip 127.0.0.1
xr_driver_cli --opentrack-app-port 4243
xr_driver_cli --opentrack-app
python3 bridge.py --pose
```

Open the new private address, put the browser on the **Air output**, choose *Glasses test · 40°* and fullscreen the stage. The advertised 46° hardware FOV is not the horizontal FOV setting used by this demo. The 40° horizontal starting point assumes fullscreen 16:9, not a measured optical calibration. Wait for `UDP HEAD`, look forward, then press `R`. Check all three axes: turning right should move a fixed window left; looking up should move it down; tilting right should rotate the scene the opposite way. Axis inversions are available in the calibration section outside fullscreen. A stale feed freezes the view and displays `POSE STALE`. This preview has no prediction or reprojection; do not use its latency to judge the eventual native renderer.

After testing, stop the bridge and restore the earlier driver mode. For example, `xr_driver_cli -d` disables the driver **only if disabled was the previous state**; use your former output mode instead when you previously used another feature. The bridge itself does not change driver configuration.

## 3 · Substitute actual labwc windows for the mock panels

Run from a terminal **inside the labwc session**. No sudo. `labwc --version` checks the compositor; the authoritative capture test is on the active Wayland socket, not just the installed version string.

```sh
labwc --version
wlr-randr                               # read-only: compare output connectors and modes
wayland-info | grep -E 'ext_(foreign_toplevel_(list|image_capture_source_manager)|image_copy_capture_manager)_v1|zwlr_output_power_manager_v1'
lswt -c ti                             # title and foreign-toplevel identifier
grim -h                                # verify this build has -T
```

Choose the identifier for an open editor. Test a still first (example writes a screenshot to the current directory; delete after use). Then pass the ID as one argument to the bridge. Start a fresh bridge, not a second receiver on the same UDP port.

```sh
id='REPLACE_WITH_THE_EDITOR_IDENTIFIER'
mkdir -p .local
grim -T "$id" .local/editor-test.png
python3 bridge.py --pose --window "$id"
```

Repeat `--window` up to three times: center, left, right. Omit `--pose` to use actual previews with simulated head motion. The bridge requests a PNG roughly once per 1.25 s *plus capture time* for each selected window while the tab is visible. It is a diagnostic, not a streaming architecture. It makes no claim that a hidden/minimized surface will keep producing frames. A rejected capture, missing tool, stale ID, or timeout shows a capture warning. Do not select the presentation browser itself. Each snapshot is fit to the panel with its aspect preserved. All paint, clicking and marks inside the browser are local to the demo.

The HTTP and UDP listeners bind to `127.0.0.1` only; HTTP uses a random capability path, Host checks, and disables caching. The bridge does not expose a directory or accept new capture IDs over HTTP. Do not share your URL or choose sensitive windows. Close the browser tab/stop the bridge after use. It does not implement the production session-lock lifecycle; do not use this preview as a privacy/security control.

## Tests and limits

```sh
python3 -m unittest -v
```

Tests use synthetic UDP and a stub `grim`. No headset, labwc, or desktop power manager is simulated as real hardware. This is not an installable compositor patch, global hotkey, or daily-driver session. `DESIGN.md` describes that next step.

### Upstream references

* XREAL hardware matrix: https://docs.xreal.com/XREALDevices/XREAL%20Glasses
* XR driver and original Air support / drift note: https://github.com/wheaney/XRLinuxDriver
* Exact driver output mode, CLI flags and wire format: https://xrlinuxdriver.readthedocs.io/en/latest/opentrack-app/
* labwc release notes and initial capture limitations: https://github.com/labwc/labwc/blob/0.20.3/NEWS.md
* labwc window-capture examples: https://github.com/labwc/labwc/discussions/3299
* Monitor power versus logical layout: https://labwc.github.io/integration.html
