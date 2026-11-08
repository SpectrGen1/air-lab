# Native spatial mode: implementation handoff

## Contract

A normal labwc session owns the clients throughout. Entering XR never reconnects, reparents to another compositor, or relaunches a client. Open windows on the current workspace receive separate poses; other workspaces remain reachable, minimized windows remain minimized. A user's editor cursor, terminal job, and browser session survive both transitions. Real monitors sleep, but their logical desktop arrangement is retained when possible. One initial login into a patched labwc is required for the integrated solution; subsequent toggles are in-session.

Head and pointer are independent. Turning the head alone must not send a Wayland pointer motion, hover transition, button, scroll, or keyboard-focus event. A window moving under a stationary pointer is a distinct scene change and may legitimately cause a hit update. Viewpoint rotation changes projection only. Looking is not input. Health/staleness of tracking is separate from whether the physical mouse is idle.

## Ownership and transport

* **labwc (C patch, optional build feature):** owns canonical 2D views and original rectangles; mapped surface groups and their scene stack; ephemeral spatial ID plus generation; per-workspace panel transforms; relative mouse deltas from the real seat; press/release, keyboard focus, lock state and physical output transitions. Its global input path handles the escape binding even when the presentation helper has hung. Guarantee this at the compositor, not through an ordinary client shortcut. Start a nested labwc with sacrificial clients for patch development, not as the method for moving an already-running production session.
* **Presentation helper (native renderer):** consumes committed panel geometry, content and spatial cursor position. Reads fused orientation, samples it as close to presentation as possible, renders flat panels per view. The native helper must be a compositor-identified presentation surface: never enumerate it as content, never grant it normal keyboard focus, never draw the native 2D cursor on the glasses in addition to the spatial one. A regular focusable fullscreen application is sufficient only for the read-only experiment. Keep the renderer replaceable. Do not run a second full Wayland compositor just to host the windows.
* **Private handshake:** spawn helper with an inherited socketpair, not a public input-injection protocol. Use versioned messages for `output_ready`, `panel_map/update/remove`, `buffer/damage`, `scene_commit(epoch)`, `pointer`, `presented(epoch, stamp)`, `tracking_health` and `shutdown`. Accept acknowledgments only for the relevant generation/frame. The renderer does not tell labwc to click arbitrary clients. The compositor computes actual targets; for offscreen-click protection it needs a recent trusted view orientation. Apply the same scene revision to rendering and picking, especially across resizes or disappearing targets. A local token is not a substitute for enforcing session-lock policy.
* **Initial texture path:** ext foreign toplevel list → toplevel image capture source → image copy capture, no embedded cursor. Prototype with SHM if required, then try mutually supported DMA-BUF format/modifier and buffer synchronization, retain/release correctly, and reuse unchanged textures on head turns. Avoid CPU-to-GPU copies where possible; do not promise zero-copy until measured. Use a composed surface group from labwc when the external capture path cannot reproduce popups and subsurfaces. Keep explicit offsets from captured-buffer origin to logical group origin. Support fractional scaling, buffer transforms, crops, decorations, client input regions, and popup extensions beyond parent bounds.

## Picking and event routing

Panels have independent physical width/height and radial distance as well as
azimuth and cylinder height. Each plane is perpendicular to the line from its
centre to the fixed viewer origin, including when raised or lowered. Head turns
change the camera projection; they do not reorient or translate panels in the
workspace. The interaction pointer remains on its workspace-fixed cylinder.

Provide a spatial frame outside the captured surface group. Left-dragging its
top bar translates the panel on the cylinder; scrolling while that bar is held
changes radial distance, keeping the held point on the mouse ray. Edge drags
resize one physical axis and corner drags resize both about the panel centre.
These controls transform the spatial presentation without resizing or moving
the canonical desktop client. Consume their button/wheel events in labwc; do not
forward them as application input. Preserve matching releases through cancellation,
target destruction and force-desktop recovery. Transient groups stay attached to
their root when moved or changed in depth.

Workspace frame X right, Y up, Z forward; R positive. Store azimuth and physical cylinder height. Mouse updates theta and height; head pose is not an argument to this operation. Let `d = normalize(R sin θ, h, R cos θ)`. From the cyclopean eye origin O, intersect the ray with each eligible panel using `t = dot(C-O,n)/dot(d,n)`. Reject parallel/back-facing/behind hits. In panel coordinates, use logical width and height, reject non-input regions, then choose the nearest eligible hit (the next panel can win through an input hole). Resolve coplanar ties with scene stacking. Render the cursor at this hit for both eyes; over empty space use the actual cylinder point. Use child-local coordinates and the appropriate cursor image/hotspot from the client. Do not subtract camera yaw from pointer azimuth before hit testing.

`src/desktop.c:get_cursor_context()` and `src/input/cursor.c:cursor_process_motion()` are starting points, not a ready-made 3D API. Split coordinate lookup from the remaining desktop policy. Intersect in 3D, hit test within the particular view subtree/group, form a normal cursor context, run the real seat path. Audit all helpers that still read the flat global cursor, notably pressed drags, SSD edge tests, drag icons and compositor move/resize. During an implicit grab, project onto the grabbed plane at unbounded local coordinates until release; avoid jumps near a parallel plane, retaining the last valid point as needed. Explicit popup grabs/drag-and-drop use normal seat rules; DnD can change destination surfaces while the source drag remains active. Never drop the matching release solely because the pointer left the field of view. No generic user-space pointer warps as the main implementation.

Keyboard focus stays with the last clicked/explicitly switched application. `Alt+Tab` can show a locator/bring-to-front; do not warp the physical pointer by default. Provide separate actions for **summon pointer**, **recenter neutral workspace direction**, **move panel**, and **force desktop**. A temporary display without spatial pointer capture is an acceptable explicit fallback for games/VMs with locked relative pointers until they are implemented correctly.

## Output and session transaction

DESKTOP → ARMING → SPATIAL → RESTORING. A second request during ARMING cancels; make transitions idempotent. Snapshot physical connector identity, current power, modes/positions/scales/transforms, per-view workspace/geometry/state, seat focus & flat cursor. Preserve state in place rather than rewriting windows just to render them. Identify the Air and confirm it is extended; combining output/USB identity with explicit user selection is better than trusting connector name alone. For systems whose normal output is cloned in hardware, require a separate extended glasses path. For normal desktop output, a fullscreen native presenter on the target is the easy path; Monado/OpenXR direct presentation/DRM lease is a distinct backend requiring a successful on-machine test.

Show a healthy updating glasses frame and get user confirmation on first entry. Only then route input and power-save previously-on non-Air monitors by name. Do not use a wildcard power-off; do not turn off every logical output or blindly disable them with output management. Suppress competing wake-on-input/autoconfiguration only for the duration of the mode. Monitor disconnects that actually remove outputs need a topology policy: virtual backing output or geometry snapshot/remap, not dangling pointers. Ensure XR-visible clients get correct frame callbacks and output/scale feedback even when their flat primary output sleeps. Test video/editor animation after blanking, not just frozen initial images.

On manual exit, unplug, stale tracker, missing presentation heartbeat or helper exit: release spatial grabs and held buttons, restore a viable physical output before removing the presenter, restore prior normal focus/cursor and intentional power states, handle windows created/closed in XR. On lock, blank the spatial path before exposing any other surface and allow only the lock UI; clear sensitive retained buffers and never reveal the desktop during recovery. Require explicit re-entry after unlock. A helper watchdog can handle helper crashes; if the Wayland compositor itself dies, existing client connections cannot be promised to survive. Test from nested sessions / disposable login first; retain a VT or SSH recovery path.

## Delivery gates

1. **Hardware and interaction:** separate USB data and scanout checks; mono head-tracked high-contrast text; a stationary mouse hits the same editor pixel across head rotations. Check direction signs, yaw drift over ten minutes, and readability during an ordinary head turn. Record observed refresh and pose-to-submit age. Measure actual motion-to-photon separately; CPU frame duration is not that metric.
2. **One live editor:** blink caret / type, click a character, select text beyond a panel edge, open context menu; verify the original desktop client receives the correct events. Include fractional scale (e.g. 1.25) and nonzero popup origin.
3. **Three windows and optical ergonomics:** open an editor, browser, terminal; focus editor, look at terminal and type; only the editor changes. New window maps near the active region, transient/modal stays with parent. Test alt-tab, scroll, XWayland popup, DnD and pointer lock by separate feature gates. Benchmark frame-time percentiles, missed display intervals and PNG-free transport. Stereo is a separate milestone: query advertised mode, verify eye order and per-eye projection, and verify the same world pointer in both eyes.
4. **Safe daily toggle:** enable physical power transition last. A real editor/video continues to animate in XR with physical monitors sleeping. Hotkey back restores layout; unexpected glasses unplug or killed renderer restores a usable monitor without relying on the helper. Lock/unlock leaks zero captured frames; stale feed never produces a surprise click. Iterate on all four sides of the cylinder seam and at vertical limits.

### Relevant upstream code

* https://github.com/labwc/labwc/tree/0.20.3/src
* https://wlroots.pages.freedesktop.org/wlroots/wlr/types/wlr_scene.h.html
* https://wlroots.pages.freedesktop.org/wlroots/wlr/types/wlr_seat.h.html
* https://github.com/swaywm/wlr-protocols/blob/master/unstable/wlr-virtual-pointer-unstable-v1.xml
* https://monado.freedesktop.org/getting-started.html
* https://monado.freedesktop.org/direct-mode.html
* https://github.com/badicsalex/ar-drivers-rs
