# Air Spatial Lab

A native spatial desktop for the **original XREAL Air + labwc**. The mouse moves
on a workspace-fixed cylinder while head tracking controls the view. Air Lab
presents and controls live desktop windows, manages monitor power, and supports
side-by-side stereo with adjustable software IPD.

## Native desktop integration

The native labwc integration is built and tested; its session launcher is staged
for the next login. Save your work, log out once, and log back in to activate it.
Subsequent spatial toggles keep your existing applications connected to the same
labwc. Air notifications and the extra one-minute warmup are disabled. Presentation
starts once the tracker is connected, initially calibrated and supplying its
owned UDP feed; device startup itself can still take about 15 seconds.
The latest compositor update uses smaller panels and a level horizon, and keeps
a healthy preview open without a session timer. It requires a new login to take
effect; actual feed loss or renderer failure still restores the desktop.

Automatic spatial mode now uses the glasses’ internal OLED-on/off USB events
as well as the connected Air display. Taking them off can restore the desktop
even when Linux still reports the display enabled. Unknown OLED state waits for
a hardware event; no reliable initial-state query has been identified. Every entry automatically
routes input and sleeps the previously-on desktop monitors once tracking and a
complete rendered scene are healthy. No confirmation shortcut is required.
Unlocking automatically resumes spatial mode when the glasses remain connected.
Super+Alt+X turns automatic mode off and immediately restores the desktop; it
stays off until toggled back on. Failures retry after five seconds while enabled.
`auto_start` sets the initial preference at login. This follows
display connection events; physical put-on/take-off behavior still needs checking
with the updated compositor.

- **Super+Alt+X**: toggle automatic spatial mode; turning it off immediately exits. Mako reports the new state.
- **Super+Alt+I**: show/hide the optional stereo depth check (not an IPD measurement).
- **Super+Alt+, / .**: decrease/increase IPD by 0.5 mm; **Super+Alt+\** restores the saved baseline. Changes save automatically; a brief readout confirms the value applied by the renderer.
- **Super+Alt+R** recenters. **Super+Alt+F** summons the pointer.
- **Super+Alt+L** opens the window overview. Search by app, title, or workspace.
  Use the arrows and Enter or click a row to bring that window in front of you.
- **Super+Alt+C** centers the focused window. **Super+Alt+1 / 2 / 3** select
  Focus, Side by Side, or Code + Reference. Drag a title toward a snap position
  and release when its preview appears.
- **Super+Alt+S** saves the current spatial layout. **Super+Alt+4** recalls it.
  Saved placements, pins, and spatial resize dimensions are available on future logins.
- **Super+Alt+B** toggles the larger reading profile. It saves automatically.
  **Super+Alt+D** brings the Waybar dock into view for eight seconds.
- **Super+Alt+G** toggles Game Focus for the focused app. Native Wayland apps can
  use pointer lock and relative motion in spatial mode.
- The focused window has a brighter frame and controls for Center, Pin, and
  Minimize. Pin protects the window's placement when applying a preset.
  Drag-and-drop between visible Wayland windows uses the normal seat.
- **Super+Alt+Arrows**: move the focused panel; ordinary Alt+Tab/workspace switching remains available.
- **Super+Alt+mouse wheel:** temporary 1–3× view zoom; scroll up to magnify, down to reduce. Releasing either modifier snaps back to 1×.
- **Super+Alt+Minus / Equal:** slower/faster pointer; **Super+Alt+0:** reset speed.
- **Spatial title bar:** left-drag to move around the cylinder; scroll while
  holding it to move closer/farther. Drag a floating window's outer edge to
  resize one dimension or a corner to resize proportionally. Exiting spatial
  mode restores pre-entry desktop rectangles and moves windows off the glasses
  when a regular monitor is available.
  Panels always face the viewer origin.

Spatial mode also presents your Waybar modules as three floating rows: launcher/
taskbar, clock/status and system controls. The dock follows horizontal head turns,
centred two feet forward and one foot down; look down to use it. Buttons,
scrolling, popup menus and the wofi launcher use the real desktop input path.
Your normal desktop bar/configuration stay intact. This native update needs
the matching compositor/presenter and a new login; physical usability still needs
checking in the glasses.

Rendering uses supersampling, multisampled edges, mipmapped anisotropic filtering
and higher-resolution title text. Status messages stay in logs; no persistent HUD
banner covers the glasses view. Optimized builds, packed capture rows and partial
texture updates target the display’s 60 Hz refresh without reducing AA quality.
Head-following widgets update with rendered frames. Offscreen windows use fewer
capture updates and the presenter samples head orientation again after texture
uploads. Frame-gap and pose-age percentiles are logged without a HUD.

Head orientation uses adaptive smoothing to reduce tiny tremors while following
larger turns more quickly. Recenter resets smoothing immediately; tracking-loss
checks still use the actual incoming sample times. Initial strength needs a
physical comfort check after the next login.

Automatic spatial mode follows the glasses' selected display mode: normal 2D
uses mono; select 3D with the glasses' button for stereo at 1920x1080 per eye.
The session does not change hardware modes on entry, exit or lock. Software mode
switches produced black lenses on this setup despite valid rendered frames;
manual 3D plus spatial rendering was confirmed visible. IPD defaults to 64 mm when no profile exists, and saves when adjusted.
A measured profile supports separate left/right PD, with Reset returning to
its saved far-view baseline. Protocol v8 requires a matching compositor/presenter
and one fresh login; later saved profile changes load automatically. This is software eye spacing; optics and comfort still need
calibration. `stereo: false` disables stereo rendering; select 2D on the glasses
when using that setting.

Outside spatial mode the glasses show a normal extended desktop. Waybar's
Single layout includes LG plus connected glasses; Dual includes Dell, LG and
connected glasses. The glasses sit immediately to the right of the desktop
monitors, and unplugging them preserves the selected desktop monitor count.

Read [NATIVE.md](docs/NATIVE.md) for the build, tests, stock-session fallback,
physical acceptance checks and remaining feature gates. See
[SETUP.md](docs/SETUP.md) for tracker dependencies and hardware checks.

## Reviewing an unexpected exit

Air Lab keeps a per-login event log at
`~/.local/state/air-lab/events/latest.jsonl` (or under `$XDG_STATE_HOME`).
It records why spatial mode exited, whether desktop recovery finished, and the
exit code or signal of the compositor, presenter, and session-owned tracker.
The session-start entry points to the detailed labwc and tracker logs. See
[diagnostics](docs/NATIVE.md#persistent-exit-diagnostics) for review commands.

## Tests

```sh
python3 -m unittest -v
python3 scripts/check-native-focused.py
```

The focused runner checks the built tracker and native binaries. It also
supports an isolated headless workspace check. See [NATIVE.md](docs/NATIVE.md)
for build prerequisites and physical acceptance.

### Upstream references

* XREAL hardware matrix: https://docs.xreal.com/XREALDevices/XREAL%20Glasses
* XR driver and original Air support / drift note: https://github.com/wheaney/XRLinuxDriver
* Exact driver output mode, CLI flags and wire format: https://xrlinuxdriver.readthedocs.io/en/latest/opentrack-app/
* labwc release notes and initial capture limitations: https://github.com/labwc/labwc/blob/0.20.3/NEWS.md
* labwc window-capture examples: https://github.com/labwc/labwc/discussions/3299
* Monitor power versus logical layout: https://labwc.github.io/integration.html
