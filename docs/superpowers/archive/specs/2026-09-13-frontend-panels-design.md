# Frontend Rewrite — Part B: Real Panels, Backend Adapters, and Coordinator Migration

## Context

This is the second half of sub-project 2 of 3 (Backend rewrite: done, merged. Frontend Part A — shell, icon-rail, guard-dial widget, thread-safety bridge, Night Watch CSS, and a working demo scan — done, merged. Frontend Part B: this spec. Polish: not started).

Part A ended with a real, working shell and one real end-to-end demo (a live port scan on the Dashboard placeholder), but 4 of the 5 content pages were still empty placeholders, and the app's 10 pre-existing GUI files (5 panels, 5 integrations) were left compiled-but-unmounted, untouched, for exactly this sub-project to replace.

**Binding decision, made explicit for this sub-project because it came up three times during brainstorming and the user confirmed it consistently:** this rewrite replaces old code with new code that matches the current architecture even where the old code is functionally correct or the new abstraction is an imperfect fit. This spec does not offer "keep it as-is" as a default option the way earlier sub-projects sometimes did — where an old piece doesn't naturally fit a new contract (see the coordinator's periodic thread, below), the design finds the closest sensible fit within the new architecture's actual constraints rather than leaving the piece untouched.

## Goals

- Replace all 5 old panel files and the parts of their paired integration files that are GUI/threading-specific with new panels built on Part A's infrastructure (`guard_dial`, `gui_thread_bridge`, `gui_shell`).
- Rewrite `gui_backend_adapters.c` (data-adaptation/business logic — no GTK code) to match the current codebase's standards, even though it has no direct relationship to the visual layer being replaced.
- Redesign `gui_system_coordinator.c`'s perpetual background thread using the same condition-variable-wakeup discipline already established in the backend rewrite's process-monitor task, plus `ProgressCallback`/`gui_thread_bridge_post` for reporting its state to the GUI — extracted into a small, shared `gui_periodic_worker` module also reused by USB's automatic-monitoring thread.
- Rewrite `gui_config_dialog.c`, restyled with Night Watch CSS (no new threading concerns — it's a modal form).
- Replace the icon-rail's 5 emoji glyphs (Part A's placeholder, per the originally approved mockup — Dashboard/USB/Processes/Ports/Logs, the 5 `GtkStack` pages) with Cairo-drawn icons matching the guard-dial's existing rendering approach, colored by active/inactive state. (The Settings/Config button lives in the native header bar, not the icon rail — it is not one of the 5, and the header bar itself stays out of scope for this sub-project, exactly as it was for Part A.)
- Delete `gui_demo_scan.h`/`.c` — the real Dashboard page supersedes the demo it existed only to prove out.
- Keep the app buildable with zero warnings and launchable throughout.

## Non-goals

- Any change to backend code (`src/*.c` outside `src/gui/`, any header outside `include/gui*.h` and this and Part A's headers).
- Any change to Part A's `guard_dial`, `gui_thread_bridge`, or the shell's navigation/stack structure beyond the icon-rendering change described here.
- A general-purpose, dynamically-registered shutdown mechanism for panel worker threads — exactly 3 panels need a shutdown hook (USB, Processes, Ports); `on_window_destroy` calls each by name, the same way it already calls `gui_demo_scan_shutdown()` today. No registry, no plugin system.
- Real GTK icon-theme/symbolic-icon integration — icons are drawn directly with Cairo (see Architecture), not loaded from theme resources.

## Architecture

**File plan** (every "new" file replaces the "old" file(s) listed, which are deleted in the same task that adds their replacement):

| New | Replaces |
|---|---|
| `src/gui/panels/gui_dashboard_panel.c` (+ `.h`) | `src/gui/window/gui_stats.c` |
| `src/gui/panels/gui_usb_panel.c` (+ `.h`) | `src/gui/window/gui_usb_panel.c`, the GUI/threading parts of `src/gui/integration/gui_usb_integration.c` |
| `src/gui/panels/gui_process_panel.c` (+ `.h`) | `src/gui/window/gui_process_panel.c`, the GUI/threading parts of `src/gui/integration/gui_process_integration.c` |
| `src/gui/panels/gui_ports_panel.c` (+ `.h`) | `src/gui/window/gui_ports_panel.c`, the GUI/threading parts of `src/gui/integration/gui_ports_integration.c` |
| `src/gui/panels/gui_logs_panel.c` (+ `.h`) | `src/gui/window/gui_logging.c` |
| `src/gui/gui_config_dialog.c` (+ `.h`, rewritten in place) | `src/gui/window/gui_config_dialog.c` |
| `src/gui/gui_backend_adapters.c` (+ `.h`, rewritten in place) | `src/gui/integration/gui_backend_adapters.c` |
| `src/gui/gui_system_coordinator.c` (+ `.h`, rewritten in place) | `src/gui/integration/gui_system_coordinator.c` |
| `src/gui/gui_periodic_worker.c` (+ `.h`) | — new, shared by the coordinator and USB's auto-monitor |
| `src/gui/widgets/gui_icons.c` (+ `.h`) | — new, Cairo-drawn rail icons |

**Deleted, not replaced:** `include/gui_demo_scan.h`, `src/gui/gui_demo_scan.c` — the real Dashboard panel supersedes the demo.

**Unchanged:** all backend files; Part A's `guard_dial`, `gui_thread_bridge`, `gui_shell.h`/`.c` (except the icon-rendering change below); the public data-shape headers `include/gui.h` (the `GUIUSBDevice`/`GUIProcess`/`GUIPort` structs and callback typedefs stay as they are — new panels build on the same shapes) and `include/gui_internal.h`.

**Shell change (Part A file, narrow edit):** `gui_shell.c`'s icon-rail currently builds each rail button via `gtk_toggle_button_new_with_label(page_icons[i])` (an emoji string). This changes to building each button with an empty label and packing a small `GtkDrawingArea` (from the new `gui_icons` module) inside it, connected to the button's own `"toggled"` signal so the icon repaints in the amber/muted color matching the button's checked state.

## Component Design

### Shared list-row pattern (USB, Processes, Ports)

One visual pattern, reused three times, built once. Each entry is a card (`#1B1E29` background, `#2A2E3D` border — the same tokens the Dashboard's stat-cards already use) inside a scrollable `GtkListBox`:

- A status dot on the left, colored from the already-established semantic palette: `#4FB0A5` (normal), `#D98E3F` (suspicious/warning), `#C0524A` (critical/whitelist-violated).
- A primary line in the sans UI font: device name / process name / `port + service`.
- A secondary line in the monospace font (real telemetry, per the binding Night Watch typography rule): `mount_point` + file-change counts / `PID · CPU% · MEM%` / `puerto N/tcp`.
- A right-aligned status badge, visually the same component as the header's "SISTEMA SEGURO" badge, carrying the row's own status text.

### Scan-driven panels (USB, Processes, Ports)

Each is a production evolution of Part A's `gui_demo_scan` pattern, not a new design: a header area with an "Escanear" button, the same `guard_dial` widget (reused directly, not reimplemented) showing that panel's scan progress, and a "Cancelar" button — wired through `gui_thread_bridge_post` to a real backend call (`scan_ports_range`, `create_device_snapshot_ex`, or a `process_monitor` cycle) exactly as the demo already proved works. On completion, the panel's `GtkListBox` is rebuilt from the backend result, translated through `gui_backend_adapters.c`'s (rewritten) adapter functions into `GUIUSBDevice`/`GUIProcess`/`GUIPort` rows using the shared list-row pattern above.

USB additionally keeps its existing automatic-monitoring behavior (detecting a newly connected device without a manual click) — redesigned as described below, not dropped.

### `gui_periodic_worker` — shared condition-variable-driven background loop

A small, reusable module extracted from the same discipline the backend rewrite already established for `process_monitor.c` (Task 6: replace polling-sleep with an interruptible condition-variable wait). Two consumers need exactly this shape — a perpetual "do a bounded unit of work, then wait up to N seconds unless woken early, repeat" loop — and neither is a `ThreadPool` job (no parallelizable batch, no fixed end):

- `gui_system_coordinator.c`'s security-evaluation loop.
- The USB panel's automatic device-detection loop.

```c
typedef void (*GuiPeriodicWorkFn)(volatile sig_atomic_t *cancel, void *user_data);

typedef struct GuiPeriodicWorker GuiPeriodicWorker;

GuiPeriodicWorker *gui_periodic_worker_start(int interval_seconds, GuiPeriodicWorkFn work, void *user_data);
void gui_periodic_worker_stop(GuiPeriodicWorker *worker); // signals + joins, near-instant
```

Internally: a condition variable and a mutex-protected `should_stop` flag, `pthread_cond_timedwait` for the interval instead of `sleep()`, `pthread_cond_signal` (a single always-known worker per instance, so `signal` — not `broadcast` — is correct here, unlike the shared-pool case the backend rewrite fixed) to wake `stop` immediately. `work` receives a `cancel` flag it can check for early exit mid-unit-of-work, and reports its own state to the GUI via `gui_thread_bridge_post` from inside its own body — `gui_periodic_worker` itself doesn't know about `ProgressUpdate` or the bridge, it just runs `work` on schedule and wakes it up promptly on stop.

`gui_system_coordinator.c`'s `work` callback becomes the existing evaluation logic (`evaluate_system_security_level`, `update_aggregate_statistics`, `detect_cross_module_correlations`, `update_gui_with_global_state`), reporting its current security level as a `ProgressUpdate` (`phase` = a human-readable state description, `current`/`total` = 0/0, matching `progress.h`'s documented convention for indeterminate/continuous monitoring) through the bridge instead of calling GUI update functions directly across threads — closing the same "GTK touched from a worker thread" gap this whole rewrite exists to eliminate, which the *current* `gui_system_coordinator.c` has (its `coordinator_thread_function` calls `update_gui_with_global_state()` directly from its own thread).

### `gui_backend_adapters.c` — rewritten, same responsibility

Every function `include/gui.h`'s `GUIUSBDevice`/`GUIProcess`/`GUIPort` and the backend's own structs (`DeviceSnapshot`, `ProcessInfo`, `PortInfo`) already define the shape of: this file's job stays "translate backend data into GUI-shaped data, and support USB-change-detection / snapshot caching / PDF export," rewritten to current code standards (checked allocations, UTF-8-safe string handling, no behavior change to the adaptation logic itself — the `GUIUSBDevice`/`GUIProcess`/`GUIPort` struct shapes are a non-goal to change, since they're a stable public contract other files also use).

### `gui_icons.c` — Cairo-drawn rail icons

```c
typedef enum {
    GUI_ICON_DASHBOARD, GUI_ICON_USB, GUI_ICON_PROCESSES,
    GUI_ICON_PORTS, GUI_ICON_LOGS
} GuiIconType;

void gui_icon_draw(cairo_t *cr, GuiIconType type, double size, double r, double g, double b);
GtkWidget *gui_icon_widget_new(GuiIconType type, GtkToggleButton *state_source);
```

`gui_icon_draw` renders one icon (bar chart, USB/drive shape, lightning bolt, plug, document lines) with basic Cairo primitives at the given size and color — the same technique `guard_dial_draw` already uses, just simpler shapes. `gui_icon_widget_new` wraps this in a small `GtkDrawingArea`, connecting to `state_source`'s `"toggled"` signal to redraw in amber (`#D98E3F`) when active or muted gray (`#7d8496`, matching the CSS's existing unchecked rail-button color) when not — `state_source` is the rail button itself, matching the pattern already used for the guard dial's own state. Only these 5 icons are in scope — there is no 6th "settings" icon (see Goals).

## Data Flow — one representative example (USB manual scan)

1. User clicks "Escanear" on the USB panel. The panel's own `on_scan_button_clicked` (same shape as `gui_demo_scan`'s) spawns a `pthread` calling `create_device_snapshot_ex(device_name, previous_snapshot, hash_pool, gui_thread_bridge_post, &target, &cancel_flag, &was_cancelled)` — the real, already-reviewed backend entry point, using the `was_cancelled` out-param the backend's own final review added specifically so a cancelled scan is never silently mistaken for a complete one.
2. Each file-hash worker (or the top-level scan) posts progress through `gui_thread_bridge_post`; the panel's guard dial updates live on the main thread, exactly like the demo.
3. On completion, the panel calls `gui_backend_adapters.c`'s `adapt_device_snapshot_to_gui`/`detect_usb_changes`/`evaluate_usb_suspicion` to build `GUIUSBDevice` rows, then rebuilds its `GtkListBox` using the shared row pattern.
4. Independently, `gui_periodic_worker`-driven automatic detection (30s interval, matching the existing `start_usb_monitoring(30)` behavior) runs the same scan path in the background without the user clicking anything, updating the same list when it finds a change.

## Error Handling

- Every new `malloc`/`realloc` checked before use (project-wide invariant, unchanged).
- Every string field carrying real backend data (device names, process names, service names — unlike Part A's ASCII-only demo phase text) is truncated UTF-8-safely, not with a blind `strncpy` that could split a multi-byte sequence — directly resolving the gap Part A's final review flagged and deferred for exactly this reason.
- USB, Processes, and Ports panels each expose their own `_shutdown()` function (same shape as `gui_demo_scan_shutdown()`), and `on_window_destroy` calls all three in sequence before backend cleanup — replacing the single `gui_demo_scan_shutdown()` call Part A added.
- `gui_periodic_worker_stop()` must be called for both `gui_system_coordinator`'s instance and USB's auto-monitor instance during the same shutdown sequence, before the widgets/state they touch are torn down — same ordering invariant Part A's final review already established and documented for the demo scan.

## Testing Strategy

- **Unit-testable (TDD, no GTK/display needed):** every function in `gui_backend_adapters.c` (pure data transformation — `adapt_*_to_gui`, `detect_usb_changes`, `evaluate_usb_suspicion`, timestamp/status formatters); `gui_periodic_worker`'s start/stop timing (mirroring the backend's own `test_process_shutdown.c` — measuring that stop is near-instant, not interval-length); each panel's row-formatting functions extracted as pure `(struct) -> two strings` helpers, independent of the `GtkListBox` widget code that displays them.
- **Manual visual verification only (established Part A pattern):** panel layouts, icon rendering, the config dialog's appearance, and the full scan/cancel/list-refresh cycle observed end-to-end via the WSLg + screenshot workflow already used throughout this project.

## Migration Strategy

Each panel (and the config dialog, adapters, and coordinator) is its own task: write the new file(s), delete the old file(s) it fully replaces, wire it into the shell/app, verify, commit — the same one-piece-at-a-time discipline Part A used for its own 4 modules. `gui_periodic_worker` and `gui_icons` are built first, since the coordinator, USB panel, and shell's icon-rail change all depend on them.

## Open Items

None outstanding — Part A's three deferred open items (backend-adapters' fate, coordinator's threading model, icon strategy) are resolved above. Polish (sub-project 3) still owns: final visual QA pass across all panels together, updated README screenshots, and any remaining test-script/documentation cleanup.
