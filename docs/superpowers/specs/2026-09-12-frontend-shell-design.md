# Frontend Rewrite — Part A: Shell, Navigation, Guard Dial & Thread Bridge

## Context

This is sub-project 2 of 3 in MatCom Guard's rewrite (Backend rewrite: done, reviewed, merged to `main`. Frontend rewrite: this sub-project. Polish: last, not started).

The frontend's visual/creative direction was already brainstormed and approved by the user in an earlier session, via the visual brainstorming companion tool. That decision is **not** revisited here — it is a binding input to this spec:

- **"Night Watch" design system**: a dark slate-indigo palette (`#12141C` background, `#1B1E29` panels, `#E8E6DE` text, `#D98E3F` amber for alerts, `#4FB0A5` teal for safe/success, `#C0524A` red for critical), serif headings (Fraunces/Spectral) for a "guard's logbook" voice, humanist sans (IBM Plex Sans/Inter) for UI chrome, and monospace (IBM Plex Mono) for real telemetry data (PIDs, hashes, ports).
- A custom Cairo-drawn circular "guard dial" widget, replacing `GtkProgressBar`, used for scan progress across the app.
- A narrow icon-rail sidebar for navigation (replacing the current `GtkNotebook` tabs), driving a content-stack layout.

The exact approved mockups are preserved at `.superpowers/brainstorm/12-1789191114/content/{visual-style.html,night-watch.html}` for reference.

Because a full frontend rewrite (~9,000 lines across 13 files: 5 panels, 5 backend-integration adapters, plus main/coordinator/config files) is comparable in size to the backend rewrite (which took 7 tasks), the frontend sub-project is itself split into two parts, mirroring how the backend was split by module:

- **Part A (this spec)**: the app shell — window chrome, icon-rail navigation, the reusable guard-dial widget, and the thread-safety bridge that lets backend worker threads update GTK widgets safely. Ends with a real, running app whose 5 content pages are placeholders, plus one working end-to-end demo (a real port scan against `127.0.0.1:1-100`, with live dial progress and cancellation) proving the whole pipeline works.
- **Part B (future spec)**: migrate the 5 real panels into the Part A shell, one at a time, each restyled with the Night Watch system and wired through the Part A bridge.

## Decision: placeholders, not panel migration, in Part A

Two options were considered for Part A's scope: (1) wrap the *existing* 5 panels' widgets inside the new shell unchanged, so the app stays fully functional with old-styled content throughout, or (2) build the shell with placeholder pages and defer all real panel content to Part B. Option 2 was chosen — it keeps Part A's plan small and focused on infrastructure (shell, dial, bridge) without also having to adapt each old panel's assumptions about being a `GtkNotebook` child. The cost is that the app is not end-to-end "real" until Part B lands; this is acceptable since Part A is not being shipped to end users mid-way, and the demo scan proves the infrastructure works correctly before Part B builds on it.

## Goals

- Replace the current `GtkNotebook` tab navigation with the approved icon-rail + status-bar shell.
- Apply the Night Watch palette and typography to the shell chrome (window background, icon rail, status bar, dial colors) via GTK CSS, loaded from a new stylesheet file — the shell must visually match the approved mockup, not just be structurally present.
- Provide a reusable Cairo-drawn circular progress "guard dial" widget with a minimal, GObject-boilerplate-free API, usable by any future panel.
- Provide a generic, reusable GLib-based thread-safety bridge whose public function matches the backend's existing `ProgressCallback` signature exactly, so it can be passed directly as `cb` to any backend scan function without an adapter shim.
- Prove the shell + dial + bridge pipeline end-to-end with one real backend call (not a simulation): a demo port scan of `127.0.0.1:1-100` on the placeholder Dashboard page, with live progress on the dial and a working cancel button.
- Keep the application buildable with zero warnings (`-Wall -Wextra -std=c99 -g -Iinclude -DDEBUG`) and launchable (old panels compiled-but-unmounted, no dead build) throughout and after this change.

## Non-goals (this spec)

- Migrating any of the 5 real panels' content or business logic into the new shell — that is Part B's entire scope.
- Deleting the 5 old panel files or 5 old integration files — they remain compiled (their entry points are non-`static`, so leaving them uncalled produces no `-Wunused-function` warnings) until Part B replaces each one individually.
- Any change to backend code: no file under `src/*.c` outside `src/gui/`, nor any header outside `include/gui*.h`, is touched. Part A only adds new GUI-side files and calls one existing, unmodified backend entry point (`scan_ports_range`) for its demo.
- Deciding the fate of `gui_backend_adapters.c`'s non-widget business logic (USB change detection, PDF export, text wrapping) or `gui_system_coordinator.c`'s background evaluation thread — both are pure data/logic layers, largely GTK-widget-independent, and their disposition is an open item for Part B, not decided here.
- Real icon assets for the rail buttons — Part A uses the same emoji glyphs already visible in the approved mockup as a placeholder; a real icon strategy (GTK icon theme names vs. custom SVGs) is an open item for Part B.

## Architecture

**New files:**

| File | Responsibility |
|---|---|
| `include/gui_shell.h` / `src/gui/gui_shell.c` | Builds the `GtkApplicationWindow`: top status bar (title + "SISTEMA SEGURO"-style badge), a narrow (44px) icon-rail sidebar of 5 `GtkToggleButton`s behaving as a radio group, and a `GtkStack` content area. Exposes `GtkWidget *gui_shell_create(GtkApplication *app)` and `GtkWidget *gui_shell_get_page_container(int page_index)` — the extension point Part B uses to swap a placeholder for a real panel. |
| `include/guard_dial.h` / `src/gui/widgets/guard_dial.c` | The Cairo dial. Public API: `GtkWidget *guard_dial_new(void)`, `void guard_dial_set_progress(GtkWidget *dial, int current, int total, const char *phase)`. Internally a plain `GtkDrawingArea` with a `draw` signal handler and a small state struct attached via `g_object_set_data`/`g_object_get_data` — no GObject subclassing, since this widget is only ever used within this app. |
| `include/gui_thread_bridge.h` / `src/gui/gui_thread_bridge.c` | The thread-safety bridge. One public function, `void gui_thread_bridge_post(const ProgressUpdate *update, void *user_data)`, matching `ProgressCallback`'s exact signature from `include/progress.h` so it can be passed directly as `cb` to any backend function. `user_data` points to a caller-owned `GuiThreadBridgeTarget` (see below). |
| `include/gui_demo_scan.h` / `src/gui/gui_demo_scan.c` | Owns the Part A demo end-to-end proof: the "Probar escaneo"/"Cancelar" buttons, the `pthread` that calls `scan_ports_range`, the `cancel_flag`, and `on_demo_scan_progress`. Exposes one function, `GtkWidget *gui_demo_scan_create_widget(void)`, that `gui_shell.c` embeds into the Dashboard placeholder page. Kept separate from `gui_shell.c` (whose job is chrome/navigation only) because this entire file is temporary scaffolding — Part B deletes it outright once the real Dashboard panel replaces the placeholder. |
| `resources/style/night-watch.css` | GTK CSS implementing the Night Watch palette/typography for the shell chrome. Loaded at startup via `gtk_css_provider_load_from_path` + `gtk_style_context_add_provider_for_screen`, using a relative path the same way `matcomguard.conf` is loaded via `CONFIG_PATH` today. |

**Modified files:**

| File | Change |
|---|---|
| `src/gui/gui_main.c` | Removes `create_main_notebook()` and its per-tab creation calls; the window is now built via `gui_shell_create()`. `initialize_complete_backend_system()`/`cleanup_complete_backend_system()` calls are unchanged — the backend lifecycle does not change in Part A. |
| `Makefile` | Adds the 3 new `.c` files to `SRC`. |

**Untouched (compiled, unmounted, deferred to Part B):** `gui_usb_panel.c`, `gui_process_panel.c`, `gui_ports_panel.c`, `gui_config_dialog.c`, `gui_stats.c`, `gui_status.c`, `gui_logging.c`, `gui_usb_integration.c`, `gui_process_integration.c`, `gui_ports_integration.c`, `gui_backend_adapters.c`, `gui_system_coordinator.c`.

## Guard Dial Widget Design

A `GtkDrawingArea` sized ~140×140px (matching the approved mockup), whose `draw` handler renders, via Cairo:
1. A full background ring (`#2A2E3D`, matching the mockup's track color).
2. A progress arc from the top (`-90°` start, matching the mockup's `rotate(-90 70 70)`), length proportional to `current/total`, in amber (`#D98E3F`) — the same math as the plan's `PortInfo`-style percentage calculations, expressed once as `guard_dial_compute_arc_fraction(int current, int total)` so it is unit-testable without Cairo or a display.
3. A dashed inner ring purely decorative (matching the mockup's tick marks, `stroke-dasharray:"2 4"`), giving the "instrument" look the design calls for.
4. Centered percentage text in the serif heading font, and the phase string below it in monospace — both drawn via Pango layouts (`pango_cairo_show_layout`), not raw Cairo text, for correct font/metric handling.

State (current/total/phase string, copied into a fixed-size buffer, not a stored pointer) lives in a small struct attached to the widget via `g_object_set_data_full` with a matching free function, so `guard_dial_new()` needs no separate "destroy" call — GTK's normal widget destruction frees it.

## Thread Bridge Design

```c
typedef void (*GuiUiProgressHandler)(const ProgressUpdate *update, void *ui_user_data);

typedef struct {
    GuiUiProgressHandler handler;   // called on the main thread with the delivered update
    void *ui_user_data;             // passed through to handler unchanged
    GObject *watch;                 // optional (may be NULL): if non-NULL, weak-tracked;
                                     // the handler is skipped if this object was destroyed
                                     // before the update was delivered
} GuiThreadBridgeTarget;

// Matches ProgressCallback's signature exactly — pass this function itself
// as `cb` to any backend scan function, with `user_data` pointing at a
// GuiThreadBridgeTarget the caller owns for the scan's duration.
void gui_thread_bridge_post(const ProgressUpdate *update, void *user_data);
```

`gui_thread_bridge_post` runs on whichever thread calls it (a backend worker thread — potentially several different worker threads concurrently, for a pooled scan). It heap-allocates a small message containing a copy of `update` (phase copied into a fixed `char[128]` buffer, not `strdup`'d, matching the backend's own style) and the `GuiThreadBridgeTarget` contents, then calls `g_idle_add(bridge_dispatch, message)` — thread-safe by GLib's own design, no additional locking needed in the bridge itself. `bridge_dispatch` runs on the main thread: if `target.watch` is set and has been finalized (tracked via `g_object_add_weak_pointer`), the message is dropped; otherwise `target.handler(&copy, target.ui_user_data)` is called, which is where actual GTK widget calls happen. The message is freed and the function returns `G_SOURCE_REMOVE`.

## Data Flow — Demo Scan

The placeholder Dashboard page includes one real, working feature (not a mockup): a button that runs `scan_ports_range(1, 100, 4, ...)` against `127.0.0.1` and shows live progress on a guard dial, with cancellation.

1. "Probar escaneo" spawns a `pthread` running a small function that calls `scan_ports_range(1, 100, 4, gui_thread_bridge_post, &target, &cancel_flag, &result)`, where `target = { .handler = on_demo_scan_progress, .ui_user_data = dial_widget, .watch = G_OBJECT(dial_widget) }`.
2. Each of the port scanner's internal pool workers calls `gui_thread_bridge_post(&update, &target)` as it finishes a port — from a worker thread.
3. The bridge copies the update, calls `g_idle_add`.
4. On the main thread, `on_demo_scan_progress` calls `guard_dial_set_progress(dial_widget, update->current, update->total, update->phase)`.
5. "Cancelar" sets `cancel_flag = 1`; the already-reviewed backend cancellation contract takes it from there.
6. When the spawned `pthread` returns from `scan_ports_range`, it posts one more `g_idle_add` (a plain one, not through the full bridge — there is no `ProgressUpdate` to deliver, just "done") to re-enable the "Probar escaneo" button and log the result count.

## Error Handling

- **Window closed while the demo scan is running**: `on_window_destroy` sets `cancel_flag = 1` and `pthread_join`s the demo thread before calling `cleanup_complete_backend_system()`/`gtk_main_quit()` — the same responsible-shutdown pattern already established for the process monitor in the backend rewrite.
- **Widget destroyed before a queued update is delivered**: covered by `GuiThreadBridgeTarget.watch` — in Part A this can only happen if the window itself is closing (the dial lives for the app's lifetime otherwise), which is already handled by the join above; the `watch` mechanism exists so Part B's panels (which can have shorter-lived widgets) inherit safety for free.
- **`malloc` failure when building a bridge message**: the update is dropped (logged, not delivered) rather than risking a dangling pointer or partial write — losing one progress tick is harmless; corrupting GTK's widget tree is not.
- Every new file follows the codebase's existing invariants: malloc/realloc checked before use, no thread touches a GTK widget except via the bridge's main-thread dispatch.

## Testing Strategy

GTK has no meaningful unit-test story without a display. The split:

- **Unit-testable (TDD, no GTK/Cairo/display needed):** `guard_dial_compute_arc_fraction(current, total)` (the pure percentage-to-arc-length math, including edge cases `total == 0` and `current > total`), and the bridge message's copy logic (`bridge_message_create`/`bridge_message_free`, tested by calling them directly rather than through `g_idle_add`, asserting the copied `phase` buffer and values are correct and that freeing doesn't double-free).
- **Manual smoke test (this repo's established pattern from the portfolio-cleanup pass):** build under WSL, launch via WSLg with `GDK_BACKEND=x11`, screenshot the new shell (icon rail, status bar, placeholder pages), and exercise the demo scan button and cancel button by hand, confirming the dial animates and cancellation stops it promptly.

## Migration Strategy (Part A → Part B)

Part A leaves the 10 old panel/integration files compiled but unmounted, and the demo scan as the only "real" page content. Part B's own plan (written separately, after this one is implemented and reviewed) will, for each of the 5 pages in turn: write a new panel file styled with the Night Watch CSS and using `guard_dial`/`gui_thread_bridge_post` where relevant, mount it via `gui_shell_get_page_container(index)`, delete the old panel file it replaces, and decide file-by-file whether the corresponding old integration file's logic is reused as-is (if it's pure data transformation, e.g. `gui_backend_adapters.c`) or rewritten (if it's tangled with old-panel-specific widget code). When Part B replaces the Dashboard placeholder specifically, it also deletes `gui_demo_scan.h`/`.c` outright — that file's only purpose is to exist until then.

## Open Items for Part B

- Whether `gui_backend_adapters.c`'s business logic (USB change detection, PDF export, text wrapping) is kept as-is (re-styled call sites only) or rewritten — it contains no GTK widget code, so keeping it is the likely default, but this is Part B's call once it looks closely at each caller.
- Whether `gui_system_coordinator.c`'s background evaluation thread should be re-expressed using the backend's new `ThreadPool`/`ProgressCallback` contracts, or remains its own independent polling thread — it is not part of any of the three rewritten backend modules, so this was never in scope for the backend sub-project either.
- Real icon strategy for the 5 rail buttons (Part A keeps the mockup's emoji glyphs as placeholders).
