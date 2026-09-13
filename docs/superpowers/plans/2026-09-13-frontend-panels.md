# Frontend Rewrite Part B — Real Panels, Backend Adapters, Coordinator Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace all 5 old GTK panels (Dashboard, USB, Processes, Ports, Logs) and their paired integration files, plus the config dialog, `gui_backend_adapters.c`, and `gui_system_coordinator.c`, with new code that mounts into Part A's shell (`gui_shell_get_page_container`) and is styled with the Night Watch CSS — while wiring every scan-driven panel to the real, already-merged backend (`scan_ports_range`, `create_device_snapshot_ex`, `process_monitor`) through `guard_dial`/`gui_thread_bridge_post`, exactly as Part A's demo scan proved out. Delete `gui_demo_scan.*` (superseded by the real Dashboard). Preserve 100% of existing interactive functionality (process kill+confirm, adjustable port ranges with quick/full presets, known-services descriptions, 4-tab persisted config dialog, USB refresh-vs-deep-scan distinction, automatic USB monitoring) — restyled and rewired, never dropped.

**Architecture:** Two new shared modules (`gui_periodic_worker` — condition-variable interruptible background loop, extracted from the backend rewrite's own precedent; `gui_icons` — Cairo-drawn rail icons, same technique as `guard_dial_draw`) are built first. Every scan-driven panel (USB, Processes, Ports) follows `gui_demo_scan.c`'s exact shape: a `pthread` doing real backend work, `gui_thread_bridge_post` carrying `ProgressUpdate`s to a `guard_dial` on the main thread, and a shared "list-row" `GtkListBox` pattern (status dot + sans primary line + monospace secondary line + status badge) for results. `gui_system_coordinator.c` is rewritten on top of `gui_periodic_worker`, closing its current cross-thread GTK-touching bug. `gui_backend_adapters.c` is rewritten in place with identical external behavior, moved to `src/gui/`. The three old `src/gui/integration/*.c` files are deleted; the small amount of non-GUI statistics/state logic they held (e.g. `get_usb_statistics_for_gui`) moves into the new panel files that absorb them, since the panels are now each module's sole owner.

**Tech Stack:** GTK+3, Cairo, GLib (`g_idle_add`, `g_main_context_invoke`, `GWeakRef`), pthreads, the existing backend (`port_scanner.h`, `device_monitor.h`, `process_monitor.h`, `threadpool.h`, `progress.h`).

**Spec:** `docs/superpowers/specs/2026-09-13-frontend-panels-design.md`

## Global Constraints

- Build/test loop: WSL Ubuntu-20.04, `gcc -Wall -Wextra -std=c99 -g -Iinclude -DDEBUG`, zero warnings required for every commit.
- On any ad-hoc `gcc` command (outside the Makefile), library flags (`` `pkg-config --cflags --libs gtk+-3.0` ``, `-lpthread`, etc.) go **after** the source files — this toolchain links with `--as-needed` by default; getting this backwards was a real bug found and fixed in Part A.
- No backend file (`src/*.c` outside `src/gui/`, or any header outside `include/gui*.h`) may be touched.
- Part A's `guard_dial.{c,h}`, `gui_thread_bridge.{c,h}` are unchanged. `gui_shell.c`/`.h` gets exactly one edit (the icon-rail, Task 3) — no other change to it is in scope.
- `include/gui.h` and `include/gui_internal.h` are unchanged — every new file builds on the exact struct shapes and `extern` declarations already there.
- Every new `malloc`/`realloc` is checked before use.
- Every string field carrying real backend data (device names, process names, service names) is truncated UTF-8-safely (never split a multi-byte sequence with a blind `strncpy` cutoff) — use the `utf8_safe_truncate` helper added in Task 4.
- Each of USB/Processes/Ports exposes its own `_shutdown()` function (same shape as `gui_demo_scan_shutdown()`); `on_window_destroy` in `gui_main.c` calls all three, then `gui_periodic_worker_stop()` on both the coordinator's and USB's worker instances, all *before* `cleanup_complete_backend_system()` and before GTK starts destroying widgets — same ordering invariant Part A established for the demo scan.
- Night Watch palette tokens reused throughout: `#12141C` bg, `#1B1E29` panel, `#2A2E3D` border, `#E8E6DE` text, `#D98E3F` amber/warning, `#4FB0A5` teal/normal, `#C0524A` red/critical.
- Spanish UI strings throughout, matching the existing app's language and the old files' exact wording wherever this plan preserves old behavior verbatim.

---

## Task 1: `gui_periodic_worker` — shared condition-variable background loop

**Files:**
- Create: `include/gui_periodic_worker.h`
- Create: `src/gui/gui_periodic_worker.c`
- Test: `tests/unit/test_gui_periodic_worker.c`
- Modify: `Makefile` (add `src/gui/gui_periodic_worker.c` to `SRC`; add a `test-gui-periodic-worker` target)

**Interfaces:**
- Produces: `GuiPeriodicWorker` (opaque), `GuiPeriodicWorkFn` (`typedef void (*GuiPeriodicWorkFn)(volatile sig_atomic_t *cancel, void *user_data);`), `gui_periodic_worker_start(int interval_seconds, GuiPeriodicWorkFn work, void *user_data)`, `gui_periodic_worker_stop(GuiPeriodicWorker *worker)`. Every later task that runs a perpetual background loop (Task 5's coordinator, Task 8's USB auto-monitor) consumes this.

- [ ] **Step 1: Write the header**

```c
// include/gui_periodic_worker.h
#ifndef GUI_PERIODIC_WORKER_H
#define GUI_PERIODIC_WORKER_H

#include <signal.h>

/**
 * Una unidad de trabajo perpetua: hace un ciclo de trabajo, chequea `cancel`
 * en los puntos donde tenga sentido salir temprano, y retorna -- el worker
 * espera `interval_seconds` (o hasta que gui_periodic_worker_stop() lo
 * despierte) antes de volver a invocarla.
 */
typedef void (*GuiPeriodicWorkFn)(volatile sig_atomic_t *cancel, void *user_data);

typedef struct GuiPeriodicWorker GuiPeriodicWorker;

/**
 * Arranca un hilo que llama a `work(cancel, user_data)` inmediatamente y
 * luego cada `interval_seconds` segundos, hasta que se llame
 * gui_periodic_worker_stop(). NULL si `work` es NULL, `interval_seconds` <= 0,
 * o falla la creación del hilo.
 */
GuiPeriodicWorker *gui_periodic_worker_start(int interval_seconds, GuiPeriodicWorkFn work, void *user_data);

/**
 * Señala al hilo que se detenga (marcando `cancel` y despertando la espera
 * de inmediato, sin importar cuánto falte del intervalo), le hace join, y
 * libera `worker`. Retorna casi de inmediato -- nunca espera el intervalo
 * completo. Seguro con `worker == NULL` (no-op).
 */
void gui_periodic_worker_stop(GuiPeriodicWorker *worker);

#endif // GUI_PERIODIC_WORKER_H
```

- [ ] **Step 2: Write the failing test**

```c
// tests/unit/test_gui_periodic_worker.c
#include "gui_periodic_worker.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static atomic_int call_count = 0;

static void counting_work(volatile sig_atomic_t *cancel, void *user_data) {
    (void)cancel;
    (void)user_data;
    atomic_fetch_add(&call_count, 1);
}

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    // Se invoca inmediatamente al arrancar, sin esperar el primer intervalo.
    call_count = 0;
    GuiPeriodicWorker *w = gui_periodic_worker_start(3600, counting_work, NULL);
    assert(w != NULL);
    usleep(100000); // 100ms: tiempo de sobra para una primera invocación inmediata
    assert(atomic_load(&call_count) == 1);

    // stop() no debe esperar el intervalo completo (3600s) -- debe ser casi instantáneo.
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    gui_periodic_worker_stop(w);
    clock_gettime(CLOCK_MONOTONIC, &end);
    assert(elapsed_seconds(start, end) < 1.0);

    // Parámetros inválidos
    assert(gui_periodic_worker_start(0, counting_work, NULL) == NULL);
    assert(gui_periodic_worker_start(1, NULL, NULL) == NULL);

    // stop(NULL) es un no-op seguro
    gui_periodic_worker_stop(NULL);

    printf("test_gui_periodic_worker: OK\n");
    return 0;
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -g -Iinclude -o tests/unit/test_gui_periodic_worker tests/unit/test_gui_periodic_worker.c src/gui/gui_periodic_worker.c -lpthread`
Expected: FAIL — `src/gui/gui_periodic_worker.c` does not exist yet (compiler error).

- [ ] **Step 4: Write the implementation**

```c
// src/gui/gui_periodic_worker.c
#include "gui_periodic_worker.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct GuiPeriodicWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile sig_atomic_t should_stop;
    int interval_seconds;
    GuiPeriodicWorkFn work;
    void *user_data;
};

static void *worker_thread_main(void *arg) {
    GuiPeriodicWorker *w = (GuiPeriodicWorker *)arg;

    for (;;) {
        w->work(&w->should_stop, w->user_data);

        pthread_mutex_lock(&w->mutex);
        if (w->should_stop) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += w->interval_seconds;

        while (!w->should_stop) {
            int rc = pthread_cond_timedwait(&w->cond, &w->mutex, &deadline);
            if (rc == ETIMEDOUT || w->should_stop) {
                break;
            }
            // Señal espuria o wakeup temprano no relacionado con stop: reintentar
            // el wait con el mismo deadline hasta que expire o should_stop se marque.
        }
        int stop_now = w->should_stop;
        pthread_mutex_unlock(&w->mutex);

        if (stop_now) {
            break;
        }
    }

    return NULL;
}

GuiPeriodicWorker *gui_periodic_worker_start(int interval_seconds, GuiPeriodicWorkFn work, void *user_data) {
    if (interval_seconds <= 0 || work == NULL) {
        return NULL;
    }

    GuiPeriodicWorker *w = malloc(sizeof(GuiPeriodicWorker));
    if (!w) {
        return NULL;
    }

    w->should_stop = 0;
    w->interval_seconds = interval_seconds;
    w->work = work;
    w->user_data = user_data;

    if (pthread_mutex_init(&w->mutex, NULL) != 0) {
        free(w);
        return NULL;
    }
    if (pthread_cond_init(&w->cond, NULL) != 0) {
        pthread_mutex_destroy(&w->mutex);
        free(w);
        return NULL;
    }
    if (pthread_create(&w->thread, NULL, worker_thread_main, w) != 0) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->mutex);
        free(w);
        return NULL;
    }

    return w;
}

void gui_periodic_worker_stop(GuiPeriodicWorker *worker) {
    if (!worker) {
        return;
    }

    pthread_mutex_lock(&worker->mutex);
    worker->should_stop = 1;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);

    pthread_join(worker->thread, NULL);

    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->mutex);
    free(worker);
}
```

Note: add `#include <errno.h>` is not needed since `ETIMEDOUT` comes from `<pthread.h>`'s transitive include of `<time.h>`/`<errno.h>` on glibc, but to be explicit and portable add `#include <errno.h>` to the top of the file alongside the others.

- [ ] **Step 5: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -g -Iinclude -o tests/unit/test_gui_periodic_worker tests/unit/test_gui_periodic_worker.c src/gui/gui_periodic_worker.c -lpthread && ./tests/unit/test_gui_periodic_worker`
Expected: `test_gui_periodic_worker: OK`

- [ ] **Step 6: Wire into the Makefile**

Add to `SRC` (after `src/gui/gui_thread_bridge.c`):
```makefile
		src/gui/gui_periodic_worker.c \
```

Add a test target (after `test-gui-thread-bridge`):
```makefile
test-gui-periodic-worker: tests/unit/test_gui_periodic_worker.c src/gui/gui_periodic_worker.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_gui_periodic_worker tests/unit/test_gui_periodic_worker.c src/gui/gui_periodic_worker.c -lpthread
	./tests/unit/test_gui_periodic_worker
```

Add `test-gui-periodic-worker` to the `test-unit` target's dependency list and to the `.PHONY` line, and add `tests/unit/test_gui_periodic_worker` to `clean`'s `rm -f` list.

- [ ] **Step 7: Full build sanity check**

Run: `make clean && make all 2>&1 | tail -30`
Expected: builds with zero warnings (the new file isn't used by anything yet, so this only proves it compiles standalone and doesn't break the existing build).

- [ ] **Step 8: Add the new test binary to `.gitignore`**

Add `tests/unit/test_gui_periodic_worker` to `.gitignore` (matching the existing entries for `test_guard_dial_math`/`test_gui_thread_bridge`).

- [ ] **Step 9: Commit**

```bash
git add include/gui_periodic_worker.h src/gui/gui_periodic_worker.c tests/unit/test_gui_periodic_worker.c Makefile .gitignore
git commit -m "feat: add gui_periodic_worker, a shared interruptible background-loop module"
```

---

## Task 2: `gui_icons` — Cairo-drawn rail icons

**Files:**
- Create: `include/gui_icons.h`
- Create: `src/gui/widgets/gui_icons.c`
- Modify: `Makefile` (add `src/gui/widgets/gui_icons.c` to `SRC`)

**Interfaces:**
- Produces: `GuiIconType` enum, `gui_icon_draw(cairo_t *cr, GuiIconType type, double size, double r, double g, double b)`, `gui_icon_widget_new(GuiIconType type, GtkToggleButton *state_source)`. Task 3 (shell edit) consumes `gui_icon_widget_new`.
- Consumes: nothing beyond GTK/Cairo.

- [ ] **Step 1: Write the header**

```c
// include/gui_icons.h
#ifndef GUI_ICONS_H
#define GUI_ICONS_H

#include <gtk/gtk.h>
#include <cairo.h>

typedef enum {
    GUI_ICON_DASHBOARD,
    GUI_ICON_USB,
    GUI_ICON_PROCESSES,
    GUI_ICON_PORTS,
    GUI_ICON_LOGS
} GuiIconType;

/**
 * Dibuja el icono `type` centrado en un cuadrado de `size` x `size` puntos,
 * con el origen de `cr` ya trasladado a la esquina superior izquierda de
 * ese cuadrado (mismo estilo que guard_dial_draw: primitivas Cairo básicas,
 * sin cargar assets externos). Color uniforme (r,g,b) en [0,1].
 */
void gui_icon_draw(cairo_t *cr, GuiIconType type, double size, double r, double g, double b);

/**
 * Crea un GtkDrawingArea que dibuja `type`, conectado a la señal "toggled"
 * de `state_source` para repintarse en ámbar (#D98E3F) cuando está activo o
 * gris apagado (#7d8496) cuando no -- state_source es el botón del riel que
 * contendrá este widget.
 */
GtkWidget *gui_icon_widget_new(GuiIconType type, GtkToggleButton *state_source);

#endif // GUI_ICONS_H
```

- [ ] **Step 2: Write the implementation**

```c
// src/gui/widgets/gui_icons.c
#include "gui_icons.h"

#define GUI_ICON_AMBER_R 0.851
#define GUI_ICON_AMBER_G 0.557
#define GUI_ICON_AMBER_B 0.247
#define GUI_ICON_MUTED_R 0.490
#define GUI_ICON_MUTED_G 0.518
#define GUI_ICON_MUTED_B 0.588

static void draw_dashboard(cairo_t *cr, double size) {
    // Gráfico de barras: tres barras de distinta altura.
    double bar_w = size * 0.18;
    double gap = size * 0.12;
    double base_y = size * 0.82;
    double heights[3] = { size * 0.35, size * 0.6, size * 0.45 };
    double x = size * 0.15;
    for (int i = 0; i < 3; i++) {
        cairo_rectangle(cr, x, base_y - heights[i], bar_w, heights[i]);
        cairo_fill(cr);
        x += bar_w + gap;
    }
}

static void draw_usb(cairo_t *cr, double size) {
    // Forma de memoria USB: cuerpo rectangular + conector angosto arriba.
    double body_w = size * 0.5;
    double body_h = size * 0.45;
    double body_x = (size - body_w) / 2.0;
    double body_y = size * 0.4;
    cairo_rectangle(cr, body_x, body_y, body_w, body_h);
    cairo_fill(cr);

    double connector_w = size * 0.2;
    double connector_h = size * 0.25;
    double connector_x = (size - connector_w) / 2.0;
    double connector_y = body_y - connector_h;
    cairo_rectangle(cr, connector_x, connector_y, connector_w, connector_h);
    cairo_fill(cr);
}

static void draw_processes(cairo_t *cr, double size) {
    // Rayo (lightning bolt) con un path simple de 6 puntos.
    cairo_move_to(cr, size * 0.55, size * 0.1);
    cairo_line_to(cr, size * 0.25, size * 0.58);
    cairo_line_to(cr, size * 0.45, size * 0.58);
    cairo_line_to(cr, size * 0.4, size * 0.9);
    cairo_line_to(cr, size * 0.75, size * 0.4);
    cairo_line_to(cr, size * 0.52, size * 0.4);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_ports(cairo_t *cr, double size) {
    // Enchufe: cuerpo redondeado + dos clavijas.
    double body_r = size * 0.28;
    cairo_arc(cr, size * 0.5, size * 0.55, body_r, 0, 2 * G_PI);
    cairo_fill(cr);

    double prong_w = size * 0.08;
    double prong_h = size * 0.22;
    cairo_rectangle(cr, size * 0.35, size * 0.1, prong_w, prong_h);
    cairo_fill(cr);
    cairo_rectangle(cr, size * 0.57, size * 0.1, prong_w, prong_h);
    cairo_fill(cr);
}

static void draw_logs(cairo_t *cr, double size) {
    // Documento con líneas de texto.
    double line_h = size * 0.1;
    double x = size * 0.2;
    double w = size * 0.6;
    double y = size * 0.22;
    for (int i = 0; i < 4; i++) {
        double this_w = (i == 3) ? w * 0.6 : w;
        cairo_rectangle(cr, x, y, this_w, line_h * 0.5);
        cairo_fill(cr);
        y += line_h;
    }
}

void gui_icon_draw(cairo_t *cr, GuiIconType type, double size, double r, double g, double b) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, r, g, b);

    switch (type) {
        case GUI_ICON_DASHBOARD: draw_dashboard(cr, size); break;
        case GUI_ICON_USB: draw_usb(cr, size); break;
        case GUI_ICON_PROCESSES: draw_processes(cr, size); break;
        case GUI_ICON_PORTS: draw_ports(cr, size); break;
        case GUI_ICON_LOGS: draw_logs(cr, size); break;
    }

    cairo_restore(cr);
}

typedef struct {
    GuiIconType type;
    GtkToggleButton *state_source;
} GuiIconWidgetData;

static gboolean on_icon_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    GuiIconWidgetData *data = (GuiIconWidgetData *)user_data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    double size = (width < height) ? width : height;

    gboolean active = gtk_toggle_button_get_active(data->state_source);
    double r = active ? GUI_ICON_AMBER_R : GUI_ICON_MUTED_R;
    double g = active ? GUI_ICON_AMBER_G : GUI_ICON_MUTED_G;
    double b = active ? GUI_ICON_AMBER_B : GUI_ICON_MUTED_B;

    gui_icon_draw(cr, data->type, size, r, g, b);
    return FALSE;
}

static void on_state_source_toggled(GtkToggleButton *button, gpointer user_data) {
    (void)button;
    gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static void on_icon_widget_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    free(user_data);
}

GtkWidget *gui_icon_widget_new(GuiIconType type, GtkToggleButton *state_source) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 20, 20);

    GuiIconWidgetData *data = malloc(sizeof(GuiIconWidgetData));
    if (data) {
        data->type = type;
        data->state_source = state_source;
        g_signal_connect(area, "draw", G_CALLBACK(on_icon_draw), data);
        g_signal_connect(area, "destroy", G_CALLBACK(on_icon_widget_destroy), data);
        g_signal_connect(state_source, "toggled", G_CALLBACK(on_state_source_toggled), area);
    }

    return area;
}
```

Add `#include <stdlib.h>` to the top of the file for `malloc`/`free`.

- [ ] **Step 3: Wire into the Makefile**

Add to `SRC` (after `src/gui/widgets/guard_dial.c`):
```makefile
		src/gui/widgets/gui_icons.c \
```

- [ ] **Step 4: Build and manually verify**

Run: `make clean && make all 2>&1 | tail -30`
Expected: zero warnings. `gui_icons.c` isn't called from anywhere yet, so there is nothing new to see on screen — this step only proves it compiles clean. Visual verification of the rendered icons happens in Task 3, once they're mounted in the rail.

- [ ] **Step 5: Commit**

```bash
git add include/gui_icons.h src/gui/widgets/gui_icons.c Makefile
git commit -m "feat: add gui_icons, Cairo-drawn rail icons matching guard_dial's technique"
```

---

## Task 3: Shell icon-rail edit — swap emoji buttons for `gui_icons`

**Files:**
- Modify: `src/gui/gui_shell.c`

**Interfaces:**
- Consumes: `gui_icon_widget_new(GuiIconType, GtkToggleButton*)` from Task 2.
- Produces: nothing new — `gui_shell.h`'s public API (`gui_shell_create`, `gui_shell_get_page_container`) is unchanged.

- [ ] **Step 1: Replace `page_icons` and `create_icon_rail`**

In `src/gui/gui_shell.c`, remove the `page_icons` array (no longer needed — icons are drawn, not glyph-based) and add `#include "gui_icons.h"` at the top. Replace `create_icon_rail`:

```c
static const GuiIconType page_icon_types[GUI_SHELL_PAGE_COUNT] = {
    GUI_ICON_DASHBOARD, GUI_ICON_USB, GUI_ICON_PROCESSES, GUI_ICON_PORTS, GUI_ICON_LOGS
};

static GtkWidget *create_icon_rail(void) {
    GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(rail), "nw-rail");
    gtk_widget_set_size_request(rail, 44, -1);

    for (int i = 0; i < GUI_SHELL_PAGE_COUNT; i++) {
        GtkWidget *btn = gtk_toggle_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "nw-rail-button");
        gtk_widget_set_tooltip_text(btn, page_labels[i]);

        GtkWidget *icon = gui_icon_widget_new(page_icon_types[i], GTK_TOGGLE_BUTTON(btn));
        gtk_container_add(GTK_CONTAINER(btn), icon);

        g_signal_connect(btn, "toggled", G_CALLBACK(on_rail_button_toggled), GINT_TO_POINTER(i));
        rail_buttons[i] = btn;
        gtk_box_pack_start(GTK_BOX(rail), btn, FALSE, FALSE, 0);
    }

    return rail;
}
```

`page_labels` (used for the tooltip) and `page_names` (used for the `GtkStack` child names) are untouched.

- [ ] **Step 2: Build**

Run: `make clean && make all 2>&1 | tail -30`
Expected: zero warnings.

- [ ] **Step 3: Manual visual verification (WSLg + screenshot)**

Launch `./matcom-guard`, screenshot the window. Verify: all 5 rail icons render (bar chart, USB shape, lightning bolt, plug, document lines), the active page's icon is amber (`#D98E3F`), inactive icons are muted gray, and clicking each icon still switches the visible `GtkStack` page (Dashboard page will still show the old demo-scan widget at this point — that's expected, it isn't removed until Task 6).

- [ ] **Step 4: Commit**

```bash
git add src/gui/gui_shell.c
git commit -m "feat: replace icon-rail emoji glyphs with Cairo-drawn gui_icons"
```

---

## Task 4: Rewrite `gui_backend_adapters.c` — move to `src/gui/`, add UTF-8-safe truncation

**Files:**
- Create: `src/gui/gui_backend_adapters.c` (replaces `src/gui/integration/gui_backend_adapters.c`)
- Modify: `include/gui_backend_adapters.h` (add `utf8_safe_truncate`)
- Delete: `src/gui/integration/gui_backend_adapters.c`
- Test: `tests/unit/test_gui_backend_adapters.c`
- Modify: `Makefile` (SRC path update, new test target)

**Interfaces:**
- Produces (unchanged signatures from the old file, all still declared in `include/gui_backend_adapters.h`): `adapt_process_info_to_gui`, `adapt_device_snapshot_to_gui`, `adapt_port_info_to_gui`, `aggregate_port_statistics`, `detect_usb_changes`, `evaluate_usb_suspicion`, `init_usb_snapshot_cache`, `store_usb_snapshot`, `get_cached_usb_snapshot`, `cleanup_usb_snapshot_cache`, `format_timestamp_for_gui`, `generate_usb_status_string`, `generate_port_status_string`, `gui_export_report_to_pdf`, `filter_emoji_and_special_chars`, `wrap_text_for_pdf`, `count_wrapped_lines`. New: `void utf8_safe_truncate(const char *src, char *dest, size_t dest_size);`.
- Consumes: `ProcessInfo`/`DeviceSnapshot`/`FileInfo`/`PortInfo` (backend headers), `GUIProcess`/`GUIUSBDevice`/`GUIPort` (`gui.h`), `free_device_snapshot` (`device_monitor.h`, backend).
- Tasks 8/9/10 (USB/Process/Ports panels) consume every `adapt_*`/`detect_usb_changes`/`evaluate_usb_suspicion`/cache function here. Task 5 (coordinator) does not depend on this file.

- [ ] **Step 1: Add `utf8_safe_truncate` to the header**

Add to `include/gui_backend_adapters.h`, in the "UTILIDADES DE CONVERSIÓN" section:

```c
/**
 * @brief Copia `src` a `dest` truncando a lo sumo en `dest_size - 1` bytes,
 * sin partir una secuencia UTF-8 multibyte a la mitad. `dest` siempre queda
 * terminado en '\0'. Seguro con dest_size == 0 (no escribe nada).
 */
void utf8_safe_truncate(const char *src, char *dest, size_t dest_size);
```

- [ ] **Step 2: Write the failing test**

```c
// tests/unit/test_gui_backend_adapters.c
#include "gui_backend_adapters.h"
#include "gui.h"
#include "process_monitor.h"
#include "device_monitor.h"
#include "port_scanner.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void test_utf8_safe_truncate(void) {
    char dest[8];

    // Cadena corta: copia completa.
    utf8_safe_truncate("abc", dest, sizeof(dest));
    assert(strcmp(dest, "abc") == 0);

    // Cadena ASCII larga: trunca en dest_size - 1.
    utf8_safe_truncate("abcdefghij", dest, sizeof(dest));
    assert(strlen(dest) == 7);

    // "café" repetido para forzar el corte justo sobre un byte de continuación
    // UTF-8 (0xA9 en "é" = 0xC3 0xA9): un buffer de 4 bytes sólo tiene espacio
    // para "caf" + NUL si se corta ANTES de la secuencia de 2 bytes de la é.
    char small[4];
    utf8_safe_truncate("café", small, sizeof(small));
    assert(strlen(small) <= 3);
    // El resultado nunca termina con un byte de continuación 0x80-0xBF colgado.
    size_t len = strlen(small);
    if (len > 0) {
        unsigned char last = (unsigned char)small[len - 1];
        assert(!(last >= 0x80 && last <= 0xBF));
    }

    // dest_size == 0: no debe escribir nada (no hay espacio ni para el NUL).
    char untouched[1] = { 'X' };
    utf8_safe_truncate("abc", untouched, 0);
    assert(untouched[0] == 'X');
}

static void test_adapt_process_info_to_gui(void) {
    ProcessInfo info = {0};
    info.pid = 1234;
    strcpy(info.name, "firefox");
    info.cpu_usage = 96.0f;
    info.mem_usage = 10.0f;
    info.is_whitelisted = 1;
    info.alerta_activa = 1; // Debe ser ignorado: whitelist tiene prioridad absoluta.
    info.exceeds_thresholds = 1;

    GUIProcess gp;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.pid == 1234);
    assert(strcmp(gp.name, "firefox") == 0);
    assert(gp.is_whitelisted == TRUE);
    assert(gp.is_suspicious == FALSE); // whitelist gana sobre alerta_activa/exceeds_thresholds/CPU alta

    // No whitelisted + alerta activa -> sospechoso.
    info.is_whitelisted = 0;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.is_suspicious == TRUE);

    // No whitelisted, sin alerta ni exceeds_thresholds, pero CPU > 95 -> sospechoso.
    info.alerta_activa = 0;
    info.exceeds_thresholds = 0;
    info.cpu_usage = 96.0f;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.is_suspicious == TRUE);

    // Nada activado y bajo uso -> no sospechoso.
    info.cpu_usage = 10.0f;
    info.mem_usage = 10.0f;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.is_suspicious == FALSE);

    assert(adapt_process_info_to_gui(NULL, &gp) == -1);
    assert(adapt_process_info_to_gui(&info, NULL) == -1);
}

static void test_evaluate_usb_suspicion(void) {
    assert(evaluate_usb_suspicion(0, 0, 0, 100) == FALSE);
    assert(evaluate_usb_suspicion(0, 0, 15, 100) == TRUE);   // >10% eliminados
    assert(evaluate_usb_suspicion(0, 25, 0, 100) == TRUE);   // >20% modificados
    assert(evaluate_usb_suspicion(20, 10, 5, 100) == TRUE);  // >30% de actividad total
    assert(evaluate_usb_suspicion(15, 0, 0, 30) == TRUE);    // dispositivo pequeño + muchos nuevos
    assert(evaluate_usb_suspicion(1, 1, 1, 100) == FALSE);
}

static FileInfo *make_file(const char *path, const char *hash) {
    FileInfo *f = malloc(sizeof(FileInfo));
    f->path = strdup(path);
    f->name = strdup(path);
    f->extension = strdup("");
    f->size = 0;
    strncpy(f->sha256_hash, hash, sizeof(f->sha256_hash) - 1);
    f->sha256_hash[sizeof(f->sha256_hash) - 1] = '\0';
    f->permissions = 0644;
    f->last_modified = 0;
    f->last_accessed = 0;
    return f;
}

static DeviceSnapshot *make_snapshot(const char *device_name, int file_count) {
    DeviceSnapshot *s = malloc(sizeof(DeviceSnapshot));
    s->device_name = strdup(device_name);
    s->files = malloc(sizeof(FileInfo *) * (file_count > 0 ? file_count : 1));
    s->file_count = 0;
    s->capacity = file_count > 0 ? file_count : 1;
    s->snapshot_time = 1000;
    return s;
}

static void test_detect_usb_changes(void) {
    DeviceSnapshot *old_snap = make_snapshot("USB1", 2);
    old_snap->files[old_snap->file_count++] = make_file("/media/USB1/a.txt", "hash_a");
    old_snap->files[old_snap->file_count++] = make_file("/media/USB1/b.txt", "hash_b");

    DeviceSnapshot *new_snap = make_snapshot("USB1", 2);
    new_snap->files[new_snap->file_count++] = make_file("/media/USB1/a.txt", "hash_a_CHANGED");
    new_snap->files[new_snap->file_count++] = make_file("/media/USB1/c.txt", "hash_c");
    // b.txt ausente en new_snap -> eliminado.

    int added, modified, deleted;
    assert(detect_usb_changes(old_snap, new_snap, &added, &modified, &deleted) == 0);
    assert(added == 1);    // c.txt
    assert(modified == 1); // a.txt (hash distinto)
    assert(deleted == 1);  // b.txt

    free_device_snapshot(old_snap);
    free_device_snapshot(new_snap);
}

static void test_generate_status_strings(void) {
    char buf[64];
    assert(generate_usb_status_string(0, FALSE, TRUE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "ESCANEANDO") == 0);
    assert(generate_usb_status_string(0, TRUE, FALSE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "SOSPECHOSO") == 0);
    assert(generate_usb_status_string(3, FALSE, FALSE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "CAMBIOS DETECTADOS") == 0);
    assert(generate_usb_status_string(0, FALSE, FALSE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "LIMPIO") == 0);

    assert(generate_port_status_string(1, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "Abierto") == 0);
    assert(generate_port_status_string(0, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "Cerrado") == 0);
}

static void test_adapt_port_info_to_gui(void) {
    PortInfo port = {0};
    port.port = 443;
    port.is_open = 1;
    port.is_suspicious = 0;
    strcpy(port.service_name, "HTTPS");

    GUIPort gp;
    assert(adapt_port_info_to_gui(&port, &gp) == 0);
    assert(gp.port == 443);
    assert(strcmp(gp.service, "HTTPS") == 0);
    assert(strcmp(gp.status, "Abierto") == 0);
    assert(gp.is_suspicious == 0);
}

static void test_aggregate_port_statistics(void) {
    PortInfo ports[3] = {
        { .port = 22, .is_open = 1, .is_suspicious = 0 },
        { .port = 4444, .is_open = 1, .is_suspicious = 1 },
        { .port = 25, .is_open = 0, .is_suspicious = 0 },
    };
    int total_open, total_suspicious;
    assert(aggregate_port_statistics(ports, 3, &total_open, &total_suspicious) == 0);
    assert(total_open == 2);
    assert(total_suspicious == 1);
}

static void test_usb_snapshot_cache(void) {
    assert(init_usb_snapshot_cache() == 0);

    DeviceSnapshot *snap = make_snapshot("USB_TEST", 1);
    snap->files[snap->file_count++] = make_file("/media/USB_TEST/f.txt", "hash1");

    assert(store_usb_snapshot("USB_TEST", snap) == 0);
    DeviceSnapshot *cached = get_cached_usb_snapshot("USB_TEST");
    assert(cached == snap);
    assert(get_cached_usb_snapshot("NONEXISTENT") == NULL);

    cleanup_usb_snapshot_cache(); // Libera `snap` internamente vía free_device_snapshot.
}

static void test_format_timestamp_for_gui(void) {
    char buf[64];
    assert(format_timestamp_for_gui(0, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "Nunca") == 0);

    time_t now = time(NULL);
    assert(format_timestamp_for_gui(now - 30, buf, sizeof(buf)) == 0);
    assert(strstr(buf, "seg") != NULL);
}

static void test_filter_and_wrap(void) {
    char out[64];
    filter_emoji_and_special_chars("Hola \xF0\x9F\x91\x8B mundo", out, sizeof(out));
    assert(strstr(out, "Hola") != NULL);
    assert(strstr(out, "mundo") != NULL);

    char wrapped[128];
    wrap_text_for_pdf("una linea muy larga que debe partirse en pedazos mas chicos", wrapped, sizeof(wrapped), 10);
    assert(strchr(wrapped, '\n') != NULL);

    int lines = count_wrapped_lines("corta\nuna linea muy muy muy larga que no cabe", 10);
    assert(lines >= 3);
}

int main(void) {
    test_utf8_safe_truncate();
    test_adapt_process_info_to_gui();
    test_evaluate_usb_suspicion();
    test_detect_usb_changes();
    test_generate_status_strings();
    test_adapt_port_info_to_gui();
    test_aggregate_port_statistics();
    test_usb_snapshot_cache();
    test_format_timestamp_for_gui();
    test_filter_and_wrap();
    printf("test_gui_backend_adapters: OK\n");
    return 0;
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -g -Iinclude -o tests/unit/test_gui_backend_adapters tests/unit/test_gui_backend_adapters.c src/gui/gui_backend_adapters.c src/device_monitor.c src/threadpool.c \`pkg-config --cflags --libs gtk+-3.0\` -lcrypto -lpthread`
Expected: FAIL — `src/gui/gui_backend_adapters.c` does not exist at the new path yet.

- [ ] **Step 4: Write the implementation**

```c
// src/gui/gui_backend_adapters.c
#define _GNU_SOURCE
#include "gui_backend_adapters.h"
#include "gui_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <cairo.h>
#include <cairo-pdf.h>

// ============================================================================
// UTILIDADES DE CONVERSIÓN
// ============================================================================

void utf8_safe_truncate(const char *src, char *dest, size_t dest_size) {
    if (!src || !dest || dest_size == 0) {
        return;
    }

    size_t src_len = strlen(src);
    size_t copy_len = (src_len < dest_size - 1) ? src_len : dest_size - 1;

    // Retroceder mientras el byte en el límite de corte sea un byte de
    // continuación UTF-8 (10xxxxxx, 0x80-0xBF) -- cortar ahí partiría un
    // codepoint multibyte a la mitad.
    while (copy_len > 0) {
        unsigned char c = (unsigned char)src[copy_len];
        if (copy_len == src_len || (c & 0xC0) != 0x80) {
            break;
        }
        copy_len--;
    }

    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

// ============================================================================
// CACHE DE SNAPSHOTS USB
// ============================================================================

typedef struct USBSnapshotCache {
    char device_name[256];
    DeviceSnapshot *snapshot;
    struct USBSnapshotCache *next;
} USBSnapshotCache;

static USBSnapshotCache *cache_head = NULL;
// Protege la lista enlazada: el escaneo USB corre en su propio hilo mientras
// otro código puede consultarla desde el hilo principal.
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================================
// ADAPTADORES DE ESTRUCTURAS DE DATOS
// ============================================================================

int adapt_process_info_to_gui(const ProcessInfo *backend_info, GUIProcess *gui_process) {
    if (!backend_info || !gui_process) {
        return -1;
    }

    gui_process->pid = backend_info->pid;
    utf8_safe_truncate(backend_info->name, gui_process->name, sizeof(gui_process->name));
    gui_process->cpu_usage = backend_info->cpu_usage;
    gui_process->mem_usage = backend_info->mem_usage;
    gui_process->is_whitelisted = backend_info->is_whitelisted;

    // Un proceso whitelisted nunca se marca sospechoso, sin importar alertas
    // activas o uso de recursos -- evita falsos positivos en procesos de confianza.
    if (backend_info->is_whitelisted) {
        gui_process->is_suspicious = FALSE;
    } else if (backend_info->alerta_activa || backend_info->exceeds_thresholds ||
               backend_info->cpu_usage > 95.0 || backend_info->mem_usage > 90.0) {
        gui_process->is_suspicious = TRUE;
    } else {
        gui_process->is_suspicious = FALSE;
    }

    return 0;
}

int adapt_device_snapshot_to_gui(const DeviceSnapshot *snapshot,
                                 const DeviceSnapshot *previous_snapshot,
                                 GUIUSBDevice *gui_device) {
    if (!snapshot || !gui_device || !snapshot->device_name) {
        return -1;
    }

    memset(gui_device, 0, sizeof(GUIUSBDevice));

    utf8_safe_truncate(snapshot->device_name, gui_device->device_name, sizeof(gui_device->device_name));
    snprintf(gui_device->mount_point, sizeof(gui_device->mount_point), "/media/%s", snapshot->device_name);
    gui_device->total_files = snapshot->file_count;
    gui_device->last_scan = snapshot->snapshot_time;

    int files_added = 0, files_modified = 0, files_deleted = 0;
    if (previous_snapshot) {
        detect_usb_changes(previous_snapshot, snapshot, &files_added, &files_modified, &files_deleted);
        gui_device->files_changed = files_added + files_modified + files_deleted;
    } else {
        gui_device->files_changed = 0;
    }

    gui_device->is_suspicious = evaluate_usb_suspicion(files_added, files_modified,
                                                        files_deleted, snapshot->file_count);
    generate_usb_status_string(gui_device->files_changed, gui_device->is_suspicious,
                                FALSE, gui_device->status, sizeof(gui_device->status));

    return 0;
}

int adapt_port_info_to_gui(const PortInfo *backend_port, GUIPort *gui_port) {
    if (!backend_port || !gui_port) {
        return -1;
    }

    gui_port->port = backend_port->port;
    gui_port->is_suspicious = backend_port->is_suspicious;
    utf8_safe_truncate(backend_port->service_name, gui_port->service, sizeof(gui_port->service));
    generate_port_status_string(backend_port->is_open, gui_port->status, sizeof(gui_port->status));

    return 0;
}

int aggregate_port_statistics(const PortInfo *ports, int port_count,
                             int *total_open, int *total_suspicious) {
    if (!ports || !total_open || !total_suspicious) {
        return -1;
    }

    *total_open = 0;
    *total_suspicious = 0;
    for (int i = 0; i < port_count; i++) {
        if (ports[i].is_open) (*total_open)++;
        if (ports[i].is_suspicious) (*total_suspicious)++;
    }

    return 0;
}

// ============================================================================
// FUNCIONES DE DETECCIÓN DE CAMBIOS
// ============================================================================

int detect_usb_changes(const DeviceSnapshot *old_snapshot,
                      const DeviceSnapshot *new_snapshot,
                      int *files_added, int *files_modified, int *files_deleted) {
    if (!old_snapshot || !new_snapshot || !files_added || !files_modified || !files_deleted) {
        return -1;
    }

    *files_added = 0;
    *files_modified = 0;
    *files_deleted = 0;

    for (int i = 0; i < new_snapshot->file_count; i++) {
        FileInfo *new_file = new_snapshot->files[i];
        int found = 0;
        for (int j = 0; j < old_snapshot->file_count; j++) {
            FileInfo *old_file = old_snapshot->files[j];
            if (strcmp(new_file->path, old_file->path) == 0) {
                found = 1;
                if (strcmp(new_file->sha256_hash, old_file->sha256_hash) != 0) {
                    (*files_modified)++;
                }
                break;
            }
        }
        if (!found) {
            (*files_added)++;
        }
    }

    for (int i = 0; i < old_snapshot->file_count; i++) {
        FileInfo *old_file = old_snapshot->files[i];
        int found = 0;
        for (int j = 0; j < new_snapshot->file_count; j++) {
            if (strcmp(old_file->path, new_snapshot->files[j]->path) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            (*files_deleted)++;
        }
    }

    return 0;
}

gboolean evaluate_usb_suspicion(int files_added, int files_modified,
                                int files_deleted, int total_files) {
    if (files_deleted > (total_files * 0.1)) return TRUE;        // >10% eliminados
    if (files_modified > (total_files * 0.2)) return TRUE;       // >20% modificados
    int total_changes = files_added + files_modified + files_deleted;
    if (total_changes > (total_files * 0.3)) return TRUE;        // >30% de actividad
    if (total_files < 50 && files_added > 10) return TRUE;       // inyección en dispositivo pequeño
    return FALSE;
}

// ============================================================================
// GESTIÓN DE ESTADO Y CACHE
// ============================================================================

int init_usb_snapshot_cache(void) {
    return 0; // cache_head arranca en NULL estáticamente.
}

int store_usb_snapshot(const char *device_name, const DeviceSnapshot *snapshot) {
    if (!device_name || !snapshot) {
        return -1;
    }

    pthread_mutex_lock(&cache_mutex);

    USBSnapshotCache *current = cache_head;
    while (current) {
        if (strcmp(current->device_name, device_name) == 0) {
            if (current->snapshot) {
                free_device_snapshot(current->snapshot);
            }
            current->snapshot = (DeviceSnapshot *)snapshot;
            pthread_mutex_unlock(&cache_mutex);
            return 0;
        }
        current = current->next;
    }

    USBSnapshotCache *new_entry = malloc(sizeof(USBSnapshotCache));
    if (!new_entry) {
        pthread_mutex_unlock(&cache_mutex);
        return -1;
    }

    utf8_safe_truncate(device_name, new_entry->device_name, sizeof(new_entry->device_name));
    new_entry->snapshot = (DeviceSnapshot *)snapshot;
    new_entry->next = cache_head;
    cache_head = new_entry;

    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

DeviceSnapshot *get_cached_usb_snapshot(const char *device_name) {
    if (!device_name) {
        return NULL;
    }

    pthread_mutex_lock(&cache_mutex);
    USBSnapshotCache *current = cache_head;
    while (current) {
        if (strcmp(current->device_name, device_name) == 0) {
            DeviceSnapshot *snapshot = current->snapshot;
            pthread_mutex_unlock(&cache_mutex);
            return snapshot;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&cache_mutex);
    return NULL;
}

void cleanup_usb_snapshot_cache(void) {
    pthread_mutex_lock(&cache_mutex);
    USBSnapshotCache *current = cache_head;
    while (current) {
        USBSnapshotCache *next = current->next;
        if (current->snapshot) {
            free_device_snapshot(current->snapshot);
        }
        free(current);
        current = next;
    }
    cache_head = NULL;
    pthread_mutex_unlock(&cache_mutex);
}

// ============================================================================
// UTILIDADES DE FORMATEO
// ============================================================================

int format_timestamp_for_gui(time_t timestamp, char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return -1;
    }

    if (timestamp == 0) {
        utf8_safe_truncate("Nunca", buffer, buffer_size);
        return 0;
    }

    time_t now = time(NULL);
    int diff = (int)(now - timestamp);

    if (diff < 60) {
        snprintf(buffer, buffer_size, "Hace %d seg", diff);
    } else if (diff < 3600) {
        snprintf(buffer, buffer_size, "Hace %d min", diff / 60);
    } else if (diff < 86400) {
        snprintf(buffer, buffer_size, "Hace %d horas", diff / 3600);
    } else {
        snprintf(buffer, buffer_size, "Hace %d dias", diff / 86400);
    }

    return 0;
}

int generate_usb_status_string(int files_changed, gboolean is_suspicious,
                              gboolean is_scanning, char *status_buffer,
                              size_t buffer_size) {
    if (!status_buffer || buffer_size == 0) {
        return -1;
    }

    const char *text;
    if (is_scanning) text = "ESCANEANDO";
    else if (is_suspicious) text = "SOSPECHOSO";
    else if (files_changed > 0) text = "CAMBIOS DETECTADOS";
    else text = "LIMPIO";

    utf8_safe_truncate(text, status_buffer, buffer_size);
    return 0;
}

int generate_port_status_string(int is_open, char *status_buffer, size_t buffer_size) {
    if (!status_buffer || buffer_size == 0) {
        return -1;
    }
    utf8_safe_truncate(is_open ? "Abierto" : "Cerrado", status_buffer, buffer_size);
    return 0;
}

// ============================================================================
// EXPORTACIÓN DE LOGS Y REPORTES
// ============================================================================

static char *get_log_content(void) {
    extern GtkTextBuffer *log_buffer;
    if (!log_buffer) {
        return NULL;
    }
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(log_buffer, &start);
    gtk_text_buffer_get_end_iter(log_buffer, &end);
    return gtk_text_buffer_get_text(log_buffer, &start, &end, FALSE);
}

static void generate_system_stats(char *stats_buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    extern GtkWidget *stats_usb_count;
    extern GtkWidget *stats_process_count;
    extern GtkWidget *stats_ports_open;
    extern GtkWidget *stats_system_status;

    const char *usb_count = stats_usb_count ? gtk_label_get_text(GTK_LABEL(stats_usb_count)) : "N/A";
    const char *process_count = stats_process_count ? gtk_label_get_text(GTK_LABEL(stats_process_count)) : "N/A";
    const char *ports_count = stats_ports_open ? gtk_label_get_text(GTK_LABEL(stats_ports_open)) : "N/A";
    const char *system_status = stats_system_status ? gtk_label_get_text(GTK_LABEL(stats_system_status)) : "N/A";

    snprintf(stats_buffer, buffer_size,
        "=== REPORTE DE SEGURIDAD MATCOM GUARD ===\n"
        "Fecha y Hora: %s\n"
        "Estado del Sistema: %s\n"
        "Dispositivos USB Conectados: %s\n"
        "Procesos Monitoreados: %s\n"
        "Puertos Abiertos: %s\n"
        "Generado por: MatCom Guard v1.0\n\n",
        timestamp, system_status, usb_count, process_count, ports_count);
}

void gui_export_report_to_pdf(const char *filename) {
    if (!filename) {
        return;
    }

    const char *ext = strrchr(filename, '.');
    if (!ext || (strcmp(ext, ".pdf") != 0 && strcmp(ext, ".txt") != 0)) {
        gui_add_log_entry("EXPORT", "ERROR", "Formato de archivo no soportado. Use .pdf o .txt");
        return;
    }

    char *log_content = get_log_content();
    if (!log_content) {
        gui_add_log_entry("EXPORT", "ERROR", "No se pudo obtener el contenido del log");
        return;
    }

    char stats_buffer[2048];
    generate_system_stats(stats_buffer, sizeof(stats_buffer));

    if (strcmp(ext, ".txt") == 0) {
        FILE *file = fopen(filename, "w");
        if (!file) {
            gui_add_log_entry("EXPORT", "ERROR", "No se pudo crear el archivo de reporte");
            g_free(log_content);
            return;
        }

        char filtered_stats[4096];
        char filtered_log[32768];
        filter_emoji_and_special_chars(stats_buffer, filtered_stats, sizeof(filtered_stats));
        filter_emoji_and_special_chars(log_content, filtered_log, sizeof(filtered_log));

        fprintf(file, "%s", filtered_stats);
        fprintf(file, "=== LOG DE EVENTOS ===\n\n");
        fprintf(file, "%s", filtered_log);
        fclose(file);
    } else {
        cairo_surface_t *surface = cairo_pdf_surface_create(filename, 595, 842);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);
            gui_add_log_entry("EXPORT", "ERROR", "No se pudo crear la superficie PDF");
            g_free(log_content);
            return;
        }
        cairo_t *cr = cairo_create(surface);
        if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
            gui_add_log_entry("EXPORT", "ERROR", "No se pudo crear el contexto Cairo");
            g_free(log_content);
            return;
        }

        cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_source_rgb(cr, 0, 0, 0);

        double x = 50, y = 50;
        double line_height = 12;
        double page_height = 792;
        double margin_bottom = 50;
        int max_chars_per_line = 85;

        cairo_set_font_size(cr, 16);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, "REPORTE DE SEGURIDAD MATCOM GUARD");
        y += line_height * 2;
        cairo_set_font_size(cr, 10);

        char filtered_stats[4096];
        char filtered_log[32768];
        filter_emoji_and_special_chars(stats_buffer, filtered_stats, sizeof(filtered_stats));
        filter_emoji_and_special_chars(log_content, filtered_log, sizeof(filtered_log));

        char wrapped_stats[8192];
        wrap_text_for_pdf(filtered_stats, wrapped_stats, sizeof(wrapped_stats), max_chars_per_line);

        char *stats_copy = strdup(wrapped_stats);
        if (stats_copy) {
            char *line = strtok(stats_copy, "\n");
            while (line != NULL) {
                if (y > page_height - margin_bottom) {
                    cairo_show_page(cr);
                    y = 50;
                }
                cairo_move_to(cr, x, y);
                cairo_show_text(cr, line);
                y += line_height;
                line = strtok(NULL, "\n");
            }
            free(stats_copy);
        }
        y += line_height;

        if (y > page_height - margin_bottom) {
            cairo_show_page(cr);
            y = 50;
        }
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, "=== LOG DE EVENTOS ===");
        y += line_height * 1.5;

        char wrapped_log[65536];
        wrap_text_for_pdf(filtered_log, wrapped_log, sizeof(wrapped_log), max_chars_per_line);

        char *log_copy = strdup(wrapped_log);
        if (log_copy) {
            char *line = strtok(log_copy, "\n");
            while (line != NULL) {
                if (y > page_height - margin_bottom) {
                    cairo_show_page(cr);
                    y = 50;
                }
                cairo_move_to(cr, x, y);
                cairo_show_text(cr, line);
                y += line_height;
                line = strtok(NULL, "\n");
            }
            free(log_copy);
        }

        cairo_destroy(cr);
        cairo_surface_finish(surface);
        cairo_surface_destroy(surface);
    }

    g_free(log_content);

    char export_message[512];
    snprintf(export_message, sizeof(export_message), "Reporte de seguridad exportado a: %s", filename);
    gui_add_log_entry("EXPORT", "INFO", export_message);
}

void filter_emoji_and_special_chars(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return;
    }

    size_t input_len = strlen(input);
    size_t j = 0;
    for (size_t i = 0; input[i] != '\0' && j < output_size - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
            output[j++] = input[i];
        } else if (c == 0xC3 && i + 1 < input_len) {
            unsigned char next = (unsigned char)input[i + 1];
            char replacement = '?';
            switch (next) {
                case 0xA1: replacement = 'a'; break; // á
                case 0xA9: replacement = 'e'; break; // é
                case 0xAD: replacement = 'i'; break; // í
                case 0xB3: replacement = 'o'; break; // ó
                case 0xBA: replacement = 'u'; break; // ú
                case 0xB1: replacement = 'n'; break; // ñ
                case 0x81: replacement = 'A'; break; // Á
                case 0x89: replacement = 'E'; break; // É
                case 0x8D: replacement = 'I'; break; // Í
                case 0x93: replacement = 'O'; break; // Ó
                case 0x9A: replacement = 'U'; break; // Ú
                case 0x91: replacement = 'N'; break; // Ñ
                default: break;
            }
            output[j++] = replacement;
            i++; // saltar el segundo byte de la secuencia de 2 bytes
        }
        // Cualquier otro byte (emojis, UTF-8 de 3-4 bytes) se descarta.
    }
    output[j] = '\0';
}

void wrap_text_for_pdf(const char *input, char *output, size_t output_size, int max_width) {
    if (!input || !output || output_size == 0 || max_width <= 0) {
        return;
    }

    size_t input_len = strlen(input);
    size_t output_pos = 0;
    size_t line_start = 0;

    for (size_t i = 0; i <= input_len && output_pos < output_size - 1; i++) {
        if (input[i] == '\n' || input[i] == '\0') {
            size_t line_len = i - line_start;

            if (line_len <= (size_t)max_width) {
                if (output_pos + line_len < output_size - 1) {
                    memcpy(output + output_pos, input + line_start, line_len);
                    output_pos += line_len;
                    if (input[i] == '\n' && output_pos < output_size - 1) {
                        output[output_pos++] = '\n';
                    }
                }
            } else {
                size_t pos = line_start;
                while (pos < i && output_pos < output_size - 1) {
                    size_t chunk_end = pos + max_width;
                    if (chunk_end > i) chunk_end = i;

                    if (chunk_end < i) {
                        size_t last_space = chunk_end;
                        while (last_space > pos && input[last_space] != ' ') {
                            last_space--;
                        }
                        if (last_space > pos) {
                            chunk_end = last_space;
                        }
                    }

                    size_t chunk_len = chunk_end - pos;
                    if (output_pos + chunk_len < output_size - 1) {
                        memcpy(output + output_pos, input + pos, chunk_len);
                        output_pos += chunk_len;
                        if (output_pos < output_size - 1) {
                            output[output_pos++] = '\n';
                        }
                    }

                    pos = chunk_end;
                    if (pos < i && input[pos] == ' ') pos++;
                }
            }
            line_start = i + 1;
        }
    }

    output[output_pos] = '\0';
}

int count_wrapped_lines(const char *text, int max_width) {
    if (!text || max_width <= 0) {
        return 0;
    }

    int line_count = 0;
    size_t text_len = strlen(text);
    size_t line_start = 0;

    for (size_t i = 0; i <= text_len; i++) {
        if (text[i] == '\n' || text[i] == '\0') {
            size_t line_len = i - line_start;
            if (line_len <= (size_t)max_width) {
                line_count++;
            } else {
                line_count += (line_len + max_width - 1) / max_width;
            }
            line_start = i + 1;
        }
    }

    return line_count;
}
```

- [ ] **Step 5: Delete the old file**

```bash
rm src/gui/integration/gui_backend_adapters.c
```

- [ ] **Step 6: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -g -Iinclude -o tests/unit/test_gui_backend_adapters tests/unit/test_gui_backend_adapters.c src/gui/gui_backend_adapters.c src/device_monitor.c src/threadpool.c \`pkg-config --cflags --libs gtk+-3.0\` -lcrypto -lpthread && ./tests/unit/test_gui_backend_adapters`
Expected: `test_gui_backend_adapters: OK`

Note: this compiles `gui_backend_adapters.c` against real GTK headers because `get_log_content`/`generate_system_stats` use `GtkTextBuffer`/`GtkWidget` — but none of the tested functions touch a live display, so this still runs headless in CI/WSL without an X server.

- [ ] **Step 7: Update the Makefile**

Change the `SRC` line from `src/gui/integration/gui_backend_adapters.c` to `src/gui/gui_backend_adapters.c` (keep its position in the list). Add a test target after `test-gui-thread-bridge`:

```makefile
test-gui-backend-adapters: tests/unit/test_gui_backend_adapters.c src/gui/gui_backend_adapters.c src/device_monitor.c src/threadpool.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_gui_backend_adapters tests/unit/test_gui_backend_adapters.c src/gui/gui_backend_adapters.c src/device_monitor.c src/threadpool.c `pkg-config --cflags --libs gtk+-3.0` -lcrypto -lpthread
	./tests/unit/test_gui_backend_adapters
```

Add `test-gui-backend-adapters` to `test-unit`'s dependencies, `.PHONY`, and add its binary path to `clean` and to `.gitignore`.

- [ ] **Step 8: Full build sanity check**

Run: `make clean && make all 2>&1 | tail -30`
Expected: zero warnings. (`gui_main.c` still calls `gui_export_report_to_pdf` and the rest of the old integration files still reference the old adapter functions by their unchanged names/header, so the build stays green through this task even though later tasks haven't run yet.)

- [ ] **Step 9: Commit**

```bash
git add include/gui_backend_adapters.h src/gui/gui_backend_adapters.c tests/unit/test_gui_backend_adapters.c Makefile .gitignore
git rm src/gui/integration/gui_backend_adapters.c
git commit -m "refactor: rewrite gui_backend_adapters.c at its new path with UTF-8-safe truncation"
```

---

## Task 5: Night Watch CSS extension + `gui_dashboard_panel` — replaces `gui_stats.c`, deletes `gui_demo_scan`

**Files:**
- Modify: `resources/style/night-watch.css` (add panel/stat-card/list-row/button classes reused by every later panel task)
- Create: `include/gui_dashboard_panel.h`
- Create: `src/gui/panels/gui_dashboard_panel.c`
- Delete: `src/gui/window/gui_stats.c`
- Delete: `include/gui_demo_scan.h`, `src/gui/gui_demo_scan.c`
- Modify: `src/gui/gui_main.c` (mount the dashboard instead of the demo scan widget; remove the demo's shutdown call and include)
- Modify: `Makefile` (SRC updates)

**Interfaces:**
- Produces: `GtkWidget *gui_dashboard_panel_create(void);`, and (unchanged signature, now defined here instead of in `gui_stats.c`) `void gui_update_statistics(int usb_devices, int processes, int open_ports);` (declared in `gui.h`, untouched).
- Produces CSS classes reused by Tasks 6-10: `nw-panel`, `nw-panel-title`, `nw-toolbar`, `nw-button-primary`, `nw-button-secondary`, `nw-stat-card`, `nw-stat-value` (+ `.nw-value-warning`/`.nw-value-critical` modifiers), `nw-stat-label`, `nw-list-row`, `nw-row-dot-normal`/`-warning`/`-critical`, `nw-row-primary`, `nw-row-secondary`, `nw-row-badge` (+ `-normal`/`-warning`/`-critical` modifiers).
- Consumes: nothing new — same `gui_internal.h` extern widgets (`stats_usb_count`, `stats_usb_suspicious`, `stats_process_count`, `stats_ports_open`, `stats_system_status`, `stats_last_scan`) this panel still defines.

- [ ] **Step 1: Extend the Night Watch stylesheet**

Append to `resources/style/night-watch.css`:

```css
box.nw-panel {
    background-color: #12141C;
    padding: 16px;
}

label.nw-panel-title {
    font-family: Fraunces, Georgia, serif;
    color: #E8E6DE;
    font-weight: bold;
    font-size: 16px;
}

box.nw-toolbar {
    padding: 4px 0 12px 0;
}

button.nw-button-primary {
    background-color: rgba(217, 142, 63, 0.15);
    background-image: none;
    border: 1px solid #D98E3F;
    border-radius: 6px;
    color: #D98E3F;
    padding: 6px 14px;
}

button.nw-button-primary:disabled {
    color: #7d8496;
    border-color: #2A2E3D;
    background-color: transparent;
}

button.nw-button-secondary {
    background-color: transparent;
    background-image: none;
    border: 1px solid #2A2E3D;
    border-radius: 6px;
    color: #E8E6DE;
    padding: 6px 14px;
}

button.nw-button-secondary:disabled {
    color: #7d8496;
}

box.nw-stat-card {
    background-color: #1B1E29;
    border: 1px solid #2A2E3D;
    border-radius: 8px;
    padding: 14px;
}

label.nw-stat-value {
    color: #E8E6DE;
    font-weight: bold;
    font-size: 22px;
}

label.nw-stat-value.nw-value-warning {
    color: #D98E3F;
}

label.nw-stat-value.nw-value-critical {
    color: #C0524A;
}

label.nw-stat-label {
    color: #7d8496;
    font-size: 11px;
}

box.nw-list-row {
    background-color: #1B1E29;
    border: 1px solid #2A2E3D;
    border-radius: 6px;
    padding: 8px 12px;
}

label.nw-row-dot-normal {
    color: #4FB0A5;
}

label.nw-row-dot-warning {
    color: #D98E3F;
}

label.nw-row-dot-critical {
    color: #C0524A;
}

label.nw-row-primary {
    color: #E8E6DE;
    font-size: 13px;
}

label.nw-row-secondary {
    font-family: monospace;
    color: #7d8496;
    font-size: 11px;
}

label.nw-row-badge {
    border-radius: 10px;
    padding: 2px 8px;
    font-size: 10px;
}

label.nw-row-badge-normal {
    color: #4FB0A5;
    background-color: rgba(79, 176, 165, 0.15);
    border: 1px solid #4FB0A5;
}

label.nw-row-badge-warning {
    color: #D98E3F;
    background-color: rgba(217, 142, 63, 0.15);
    border: 1px solid #D98E3F;
}

label.nw-row-badge-critical {
    color: #C0524A;
    background-color: rgba(192, 82, 74, 0.15);
    border: 1px solid #C0524A;
}
```

- [ ] **Step 2: Write the header**

```c
// include/gui_dashboard_panel.h
#ifndef GUI_DASHBOARD_PANEL_H
#define GUI_DASHBOARD_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel del Dashboard: tarjetas de estadísticas + estado general. */
GtkWidget *gui_dashboard_panel_create(void);

#endif // GUI_DASHBOARD_PANEL_H
```

- [ ] **Step 3: Write the implementation**

```c
// src/gui/panels/gui_dashboard_panel.c
#include "gui_dashboard_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include <stdio.h>
#include <time.h>

GtkWidget *stats_usb_count = NULL;
GtkWidget *stats_usb_suspicious = NULL;
GtkWidget *stats_process_count = NULL;
GtkWidget *stats_ports_open = NULL;
GtkWidget *stats_system_status = NULL;
GtkWidget *stats_last_scan = NULL;

static GtkWidget *create_stat_card(const char *label_text, GtkWidget **value_widget) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "nw-stat-card");

    GtkWidget *value = gtk_label_new("0");
    gtk_style_context_add_class(gtk_widget_get_style_context(value), "nw-stat-value");
    gtk_widget_set_halign(value, GTK_ALIGN_START);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "nw-stat-label");
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(card), value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), label, FALSE, FALSE, 0);

    *value_widget = value;
    return card;
}

GtkWidget *gui_dashboard_panel_create(void) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *title = gtk_label_new("Estado del Sistema de Proteccion");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(panel), title, FALSE, FALSE, 0);

    GtkWidget *cards_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("Dispositivos USB", &stats_usb_count), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("USB Sospechosos", &stats_usb_suspicious), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("Procesos Monitoreados", &stats_process_count), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("Puertos Abiertos", &stats_ports_open), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(panel), cards_row, FALSE, FALSE, 0);

    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    stats_system_status = gtk_label_new("Sistema Activo");
    gtk_style_context_add_class(gtk_widget_get_style_context(stats_system_status), "nw-row-primary");
    GtkWidget *status_label = gtk_label_new("Estado General");
    gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "nw-stat-label");
    gtk_box_pack_start(GTK_BOX(status_box), stats_system_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_box), status_label, FALSE, FALSE, 0);

    GtkWidget *scan_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    stats_last_scan = gtk_label_new("Nunca");
    gtk_style_context_add_class(gtk_widget_get_style_context(stats_last_scan), "nw-row-primary");
    GtkWidget *scan_label = gtk_label_new("Ultimo Escaneo");
    gtk_style_context_add_class(gtk_widget_get_style_context(scan_label), "nw-stat-label");
    gtk_box_pack_start(GTK_BOX(scan_box), stats_last_scan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(scan_box), scan_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(status_row), status_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_row), scan_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), status_row, FALSE, FALSE, 0);

    return panel;
}

static void set_value_class(GtkWidget *label, const char *value_class) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(label);
    gtk_style_context_remove_class(ctx, "nw-value-warning");
    gtk_style_context_remove_class(ctx, "nw-value-critical");
    if (value_class) {
        gtk_style_context_add_class(ctx, value_class);
    }
}

void gui_update_statistics(int usb_devices, int processes, int open_ports) {
    if (!stats_usb_count || !stats_process_count || !stats_ports_open) {
        return;
    }

    char text[32];

    snprintf(text, sizeof(text), "%d", usb_devices);
    gtk_label_set_text(GTK_LABEL(stats_usb_count), text);
    set_value_class(stats_usb_count, usb_devices > 5 ? "nw-value-warning" : NULL);

    snprintf(text, sizeof(text), "%d", processes);
    gtk_label_set_text(GTK_LABEL(stats_process_count), text);
    set_value_class(stats_process_count, processes > 50 ? "nw-value-critical" : NULL);

    snprintf(text, sizeof(text), "%d", open_ports);
    gtk_label_set_text(GTK_LABEL(stats_ports_open), text);
    const char *ports_class = NULL;
    if (open_ports > 20) ports_class = "nw-value-critical";
    else if (open_ports > 10) ports_class = "nw-value-warning";
    set_value_class(stats_ports_open, ports_class);

    // El conteo de USB sospechosos llega por gui_update_usb_device (Task 7),
    // no por este agregado -- se deja en su valor actual.

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    if (stats_last_scan) {
        gtk_label_set_text(GTK_LABEL(stats_last_scan), timestamp);
    }

    char log_message[256];
    snprintf(log_message, sizeof(log_message),
             "Estadisticas actualizadas - USB: %d, Procesos: %d, Puertos: %d",
             usb_devices, processes, open_ports);
    gui_add_log_entry("ESTADISTICAS", "INFO", log_message);
}
```

- [ ] **Step 4: Delete the old dashboard file and the demo scan**

```bash
rm src/gui/window/gui_stats.c
rm include/gui_demo_scan.h src/gui/gui_demo_scan.c
```

- [ ] **Step 5: Update `gui_main.c` to mount the dashboard instead of the demo**

In `src/gui/gui_main.c`:
- Replace `#include "gui_demo_scan.h"` with `#include "gui_dashboard_panel.h"`.
- In `on_window_destroy`, delete the `gui_demo_scan_shutdown();` call and its preceding comment block (the Dashboard has no background thread to join).
- In `init_gui`, replace:
```c
    GtkWidget *dashboard_page = gui_shell_get_page_container(0);
    if (dashboard_page != NULL) {
        gtk_box_pack_start(GTK_BOX(dashboard_page), gui_demo_scan_create_widget(), TRUE, TRUE, 0);
    }
```
  with:
```c
    GtkWidget *dashboard_page = gui_shell_get_page_container(0);
    if (dashboard_page != NULL) {
        gtk_box_pack_start(GTK_BOX(dashboard_page), gui_dashboard_panel_create(), TRUE, TRUE, 0);
    }
```

- [ ] **Step 6: Update the Makefile**

Remove `src/gui/gui_demo_scan.c` from `SRC`. Change `src/gui/window/gui_stats.c` to `src/gui/panels/gui_dashboard_panel.c`.

- [ ] **Step 7: Build**

Run: `make clean && make all 2>&1 | tail -30`
Expected: zero warnings. (The other 4 pages are still the old panels/placeholders until later tasks — only page 0 changes visibly here.)

- [ ] **Step 8: Manual visual verification (WSLg + screenshot)**

Launch `./matcom-guard`, confirm the Dashboard page shows 4 dark stat cards (USB/USB-sospechosos/Procesos/Puertos) with amber panel title, and the demo scan button/dial from Part A are gone.

- [ ] **Step 9: Commit**

```bash
git add resources/style/night-watch.css include/gui_dashboard_panel.h src/gui/panels/gui_dashboard_panel.c src/gui/gui_main.c Makefile
git rm src/gui/window/gui_stats.c include/gui_demo_scan.h src/gui/gui_demo_scan.c
git commit -m "feat: add gui_dashboard_panel, extend Night Watch CSS, delete the Part A demo scan"
```

---

## Task 6: `gui_logs_panel` — replaces `gui_logging.c`, mounted at page 4

**Files:**
- Create: `include/gui_logs_panel.h`
- Create: `src/gui/panels/gui_logs_panel.c`
- Delete: `src/gui/window/gui_logging.c`
- Modify: `src/gui/gui_main.c` (mount the logs panel into page 4; add the include)
- Modify: `Makefile` (SRC update)

**Interfaces:**
- Produces: `GtkWidget *gui_logs_panel_create(void);`, and (unchanged signature from `gui.h`, now defined here) `void gui_add_log_entry(const char *module, const char *level, const char *message);`.
- Consumes: nothing new beyond GTK. This is the thread-safety precedent (`g_idle_add`) every other panel's cross-thread callback already follows.

- [ ] **Step 1: Write the header**

```c
// include/gui_logs_panel.h
#ifndef GUI_LOGS_PANEL_H
#define GUI_LOGS_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Registros: una vista de texto de solo lectura con el log de eventos. */
GtkWidget *gui_logs_panel_create(void);

#endif // GUI_LOGS_PANEL_H
```

- [ ] **Step 2: Write the implementation**

Same thread-safety shape as the old `gui_logging.c` (`gui_add_log_entry` is called from arbitrary backend threads throughout this whole rewrite — every panel's worker thread logs directly — so it must stay `g_idle_add`-safe), restyled with Night Watch tag colors instead of the old Material-style hex palette:

```c
// src/gui/panels/gui_logs_panel.c
#include "gui_logs_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

GtkWidget *log_text_view = NULL;
GtkTextBuffer *log_buffer = NULL;
GtkTextTag *info_tag = NULL;
GtkTextTag *warning_tag = NULL;
GtkTextTag *error_tag = NULL;
GtkTextTag *alert_tag = NULL;

GtkWidget *gui_logs_panel_create(void) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *title = gtk_label_new("Registro de Eventos");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(panel), title, FALSE, FALSE, 0);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled_window, TRUE);

    log_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_text_view), GTK_WRAP_WORD);
    gtk_style_context_add_class(gtk_widget_get_style_context(log_text_view), "nw-list-row");

    log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_text_view));

    info_tag = gtk_text_buffer_create_tag(log_buffer, "info", "foreground", "#4FB0A5", NULL);
    warning_tag = gtk_text_buffer_create_tag(log_buffer, "warning", "foreground", "#D98E3F",
                                              "weight", PANGO_WEIGHT_BOLD, NULL);
    error_tag = gtk_text_buffer_create_tag(log_buffer, "error", "foreground", "#C0524A",
                                            "weight", PANGO_WEIGHT_BOLD, NULL);
    alert_tag = gtk_text_buffer_create_tag(log_buffer, "alert", "foreground", "#12141C",
                                            "background", "#C0524A", "weight", PANGO_WEIGHT_BOLD, NULL);

    gtk_container_add(GTK_CONTAINER(scrolled_window), log_text_view);
    gtk_box_pack_start(GTK_BOX(panel), scrolled_window, TRUE, TRUE, 0);

    gtk_text_buffer_set_text(log_buffer,
        "MatCom Guard - Sistema de Proteccion Iniciado\n"
        "Monitoreando dispositivos USB, procesos y puertos de red...\n\n", -1);

    return panel;
}

static char *safe_strdup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, str, len);
    return copy;
}

typedef struct {
    char *module;
    char *level;
    char *message;
} LogEntryData;

static gboolean add_log_entry_main_thread(gpointer user_data) {
    LogEntryData *data = (LogEntryData *)user_data;
    if (!log_buffer || !data) {
        if (data) {
            free(data->module);
            free(data->level);
            free(data->message);
            free(data);
        }
        return FALSE;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);

    char full_message[1024];
    snprintf(full_message, sizeof(full_message), "[%s] %s | %s: %s\n",
             timestamp, data->level, data->module, data->message);

    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(log_buffer, &end_iter);

    GtkTextTag *tag = NULL;
    if (strcmp(data->level, "INFO") == 0) tag = info_tag;
    else if (strcmp(data->level, "WARNING") == 0) tag = warning_tag;
    else if (strcmp(data->level, "ERROR") == 0) tag = error_tag;
    else if (strcmp(data->level, "ALERT") == 0) tag = alert_tag;

    if (tag) {
        gtk_text_buffer_insert_with_tags(log_buffer, &end_iter, full_message, -1, tag, NULL);
    } else {
        gtk_text_buffer_insert(log_buffer, &end_iter, full_message, -1);
    }

    gtk_text_buffer_get_end_iter(log_buffer, &end_iter);
    GtkTextMark *end_mark = gtk_text_buffer_create_mark(log_buffer, NULL, &end_iter, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_text_view), end_mark);
    gtk_text_buffer_delete_mark(log_buffer, end_mark);

    printf("%s", full_message);

    free(data->module);
    free(data->level);
    free(data->message);
    free(data);
    return FALSE;
}

void gui_add_log_entry(const char *module, const char *level, const char *message) {
    if (!log_buffer || !module || !level || !message) return;

    LogEntryData *data = malloc(sizeof(LogEntryData));
    if (!data) return;

    data->module = safe_strdup(module);
    data->level = safe_strdup(level);
    data->message = safe_strdup(message);

    if (!data->module || !data->level || !data->message) {
        free(data->module);
        free(data->level);
        free(data->message);
        free(data);
        return;
    }

    g_idle_add(add_log_entry_main_thread, data);
}
```

- [ ] **Step 3: Delete the old file**

```bash
rm src/gui/window/gui_logging.c
```

- [ ] **Step 4: Mount into the shell**

In `src/gui/gui_main.c`, add `#include "gui_logs_panel.h"` and, in `init_gui` (right after the dashboard-mounting block added in Task 5), add:

```c
    GtkWidget *logs_page = gui_shell_get_page_container(4);
    if (logs_page != NULL) {
        gtk_box_pack_start(GTK_BOX(logs_page), gui_logs_panel_create(), TRUE, TRUE, 0);
    }
```

- [ ] **Step 5: Update the Makefile**

Change `src/gui/window/gui_logging.c` to `src/gui/panels/gui_logs_panel.c` in `SRC`.

- [ ] **Step 6: Build**

Run: `make clean && make all 2>&1 | tail -30`
Expected: zero warnings.

- [ ] **Step 7: Manual visual verification**

Launch `./matcom-guard`, click the Logs rail icon, confirm the panel shows the startup message with Night Watch styling and that log entries continue to appear as the app runs (e.g. the startup `gui_add_log_entry` calls already in `gui_main.c`).

- [ ] **Step 8: Commit**

```bash
git add include/gui_logs_panel.h src/gui/panels/gui_logs_panel.c src/gui/gui_main.c Makefile
git rm src/gui/window/gui_logging.c
git commit -m "feat: add gui_logs_panel, mounted at the Logs page"
```

---

## Task 7: `gui_usb_panel` — replaces the old USB panel + USB integration, real backend, list-row pattern

**Correction versus the spec:** the spec's Architecture section doesn't mention it, but `gui_main.c`'s `initialize_complete_backend_system()`/`cleanup_complete_backend_system()` currently call `init_usb_integration()`/`start_usb_monitoring(30)`/`cleanup_usb_integration()` directly. Since the new panel now owns its own lifecycle (auto-starts its monitor on creation, matching the already-approved `gui_demo_scan` precedent of needing no separate "init" call from `gui_main.c`), those specific calls are removed from `gui_main.c` in this task — the rest of `initialize_complete_backend_system` (coordinator init/start, process/ports "on-demand" log lines) is untouched until its own task. `include/gui_usb_integration.h` is **not** deleted yet — `gui_system_coordinator.h` (not rewritten until Task 11) still `#include`s it, and this file's replacement still has to satisfy that header's declarations by symbol name at link time.

**Files:**
- Create: `include/gui_usb_panel.h`
- Create: `src/gui/panels/gui_usb_panel.c`
- Delete: `src/gui/window/gui_usb_panel.c`, `src/gui/integration/gui_usb_integration.c`
- Modify: `src/gui/gui_main.c` (swap include, trim `init`/`cleanup` calls, add shutdown call, mount page 1)
- Modify: `Makefile` (SRC updates)

**Interfaces:**
- Produces: `GtkWidget *gui_usb_panel_create(void)`, `void gui_usb_panel_shutdown(void)`, `void gui_compatible_scan_usb(void)` (all in `include/gui_usb_panel.h`).
- Produces (matching `include/gui_usb_integration.h`'s existing declarations by name/signature, still needed at link time by the not-yet-rewritten `gui_system_coordinator.c`): `int is_usb_monitoring_active(void)`, `int is_gui_usb_scan_in_progress(void)`, `int get_usb_statistics_for_gui(int*, int*, int*, int*)`.
- Consumes: `gui_periodic_worker_start`/`_stop` (Task 1), `guard_dial_new`/`guard_dial_set_progress` (Part A), `gui_thread_bridge_post`/`GuiThreadBridgeTarget` (Part A), `adapt_device_snapshot_to_gui`/`detect_usb_changes`/`evaluate_usb_suspicion`/`store_usb_snapshot`/`get_cached_usb_snapshot`/`cleanup_usb_snapshot_cache` (Task 4), `create_device_snapshot_ex`/`monitor_connected_devices`/`free_device_list`/`free_device_snapshot` (backend, unchanged), `threadpool_create`/`threadpool_destroy` (backend, unchanged).

- [ ] **Step 1: Write the header**

```c
// include/gui_usb_panel.h
#ifndef GUI_USB_PANEL_H
#define GUI_USB_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Dispositivos USB: lista de dispositivos, dial de escaneo, botones. */
GtkWidget *gui_usb_panel_create(void);

/** Detiene el monitoreo automático y cualquier escaneo en curso; hace join de los hilos. */
void gui_usb_panel_shutdown(void);

/** Callback compatible con ScanUSBCallback (gui.h) para el menu "Escanear" del header bar. */
void gui_compatible_scan_usb(void);

#endif // GUI_USB_PANEL_H
```

- [ ] **Step 2: Write the implementation**

```c
// src/gui/panels/gui_usb_panel.c
#include "gui_usb_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include "gui_backend_adapters.h"
#include "gui_periodic_worker.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "device_monitor.h"
#include "threadpool.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define USB_HASH_POOL_THREADS 4
#define USB_AUTO_MONITOR_INTERVAL_SECONDS 30

typedef enum { USB_SCAN_MODE_REFRESH, USB_SCAN_MODE_DEEP } UsbScanMode;

typedef struct {
    GtkWidget *dot;
    GtkWidget *primary;
    GtkWidget *secondary;
    GtkWidget *badge;
} UsbRowRefs;

static GtkWidget *usb_list_box = NULL;
static GtkWidget *usb_dial = NULL;
static GtkWidget *usb_detail_label = NULL;
static GtkWidget *usb_refresh_button = NULL;
static GtkWidget *usb_deep_scan_button = NULL;
static GtkWidget *usb_cancel_button = NULL;

static ThreadPool *usb_hash_pool = NULL;
static GuiPeriodicWorker *usb_auto_monitor_worker = NULL;

static pthread_t usb_scan_thread;
static volatile sig_atomic_t usb_cancel_flag = 0;
static volatile sig_atomic_t usb_scan_running = 0;
static UsbScanMode usb_current_mode = USB_SCAN_MODE_REFRESH;

static int usb_last_scan_files_changed = 0;
static int usb_last_scan_suspicious_devices = 0;

// ============================================================================
// FILA DE LISTA (patron compartido: punto + linea sans + linea mono + badge)
// ============================================================================

static void apply_status_classes(GtkWidget *dot, GtkWidget *badge, gboolean is_suspicious, const char *status) {
    GtkStyleContext *dot_ctx = gtk_widget_get_style_context(dot);
    GtkStyleContext *badge_ctx = gtk_widget_get_style_context(badge);

    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-normal");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-warning");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-critical");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-normal");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-warning");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-critical");
    gtk_style_context_add_class(badge_ctx, "nw-row-badge");

    const char *level;
    if (is_suspicious) {
        level = "critical";
    } else if (strcmp(status, "LIMPIO") == 0 || strcmp(status, "ACTUALIZADO") == 0) {
        level = "normal";
    } else {
        level = "warning";
    }

    char dot_class[32], badge_class[32];
    snprintf(dot_class, sizeof(dot_class), "nw-row-dot-%s", level);
    snprintf(badge_class, sizeof(badge_class), "nw-row-badge-%s", level);
    gtk_style_context_add_class(dot_ctx, dot_class);
    gtk_style_context_add_class(badge_ctx, badge_class);
}

static GtkWidget *find_row_by_device(const char *device_name) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(usb_list_box));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL; l = l->next) {
        const char *name = g_object_get_data(G_OBJECT(l->data), "device-name");
        if (name && strcmp(name, device_name) == 0) {
            found = GTK_WIDGET(l->data);
            break;
        }
    }
    g_list_free(children);
    return found;
}

static void on_usb_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    (void)user_data;
    if (!row) return;
    const char *name = g_object_get_data(G_OBJECT(row), "device-name");
    UsbRowRefs *refs = g_object_get_data(G_OBJECT(row), "row-refs");
    if (!name || !refs) return;

    char detail[512];
    snprintf(detail, sizeof(detail), "<b>Dispositivo:</b> %s\n<b>%s</b>\n<b>Estado:</b> %s",
             name, gtk_label_get_text(GTK_LABEL(refs->secondary)), gtk_label_get_text(GTK_LABEL(refs->badge)));
    gtk_label_set_markup(GTK_LABEL(usb_detail_label), detail);
}

// gui_update_usb_device() (gui.h) solo debe llamarse desde el hilo principal
// de GTK -- el hilo de escaneo pasa siempre por post_usb_device_update().
void gui_update_usb_device(GUIUSBDevice *device) {
    if (!usb_list_box || !device) return;

    GtkWidget *row = find_row_by_device(device->device_name);
    UsbRowRefs *refs;

    if (!row) {
        row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "nw-list-row");

        GtkWidget *dot = gtk_label_new("\xE2\x97\x8F");
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *primary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(primary), "nw-row-primary");
        gtk_widget_set_halign(primary, GTK_ALIGN_START);
        GtkWidget *secondary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(secondary), "nw-row-secondary");
        gtk_widget_set_halign(secondary, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(text_box), primary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), secondary, FALSE, FALSE, 0);
        GtkWidget *badge = gtk_label_new("");

        gtk_box_pack_start(GTK_BOX(hbox), dot, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), text_box, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), badge, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);

        refs = malloc(sizeof(UsbRowRefs));
        refs->dot = dot;
        refs->primary = primary;
        refs->secondary = secondary;
        refs->badge = badge;
        g_object_set_data_full(G_OBJECT(row), "row-refs", refs, free);
        g_object_set_data_full(G_OBJECT(row), "device-name", g_strdup(device->device_name), g_free);

        gtk_list_box_insert(GTK_LIST_BOX(usb_list_box), row, -1);
        gtk_widget_show_all(row);
    } else {
        refs = g_object_get_data(G_OBJECT(row), "row-refs");
    }

    gtk_label_set_text(GTK_LABEL(refs->primary), device->device_name);

    char secondary_text[320];
    snprintf(secondary_text, sizeof(secondary_text), "%s . %d/%d archivos cambiados",
             device->mount_point, device->files_changed, device->total_files);
    gtk_label_set_text(GTK_LABEL(refs->secondary), secondary_text);
    gtk_label_set_text(GTK_LABEL(refs->badge), device->status);
    apply_status_classes(refs->dot, refs->badge, device->is_suspicious, device->status);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Dispositivo %s: %s - %d archivos modificados",
             device->device_name, device->status, device->files_changed);
    gui_add_log_entry("USB_MONITOR", device->is_suspicious ? "WARNING" : "INFO", log_msg);
}

typedef struct { GUIUSBDevice device; } UsbDeviceUpdateMsg;

static gboolean usb_device_update_idle(gpointer data) {
    UsbDeviceUpdateMsg *msg = (UsbDeviceUpdateMsg *)data;
    gui_update_usb_device(&msg->device);
    free(msg);
    return G_SOURCE_REMOVE;
}

static void post_usb_device_update(const GUIUSBDevice *device) {
    UsbDeviceUpdateMsg *msg = malloc(sizeof(UsbDeviceUpdateMsg));
    if (!msg) return;
    msg->device = *device;
    g_idle_add(usb_device_update_idle, msg);
}

// ============================================================================
// PROGRESO DE ESCANEO (dial compartido, patron gui_demo_scan)
// ============================================================================

static void on_usb_scan_progress(const ProgressUpdate *update, void *ui_user_data) {
    guard_dial_set_progress((GtkWidget *)ui_user_data, update->current, update->total, update->phase);
}

typedef struct {
    const char *device_name;
} UsbDeviceProgressContext;

// Corre en el hilo que esta escaneando (el hilo de escaneo, o un worker del
// hash_pool) -- construye el texto combinado y lo entrega de forma
// sincronica a gui_thread_bridge_post(), que copia el string internamente
// antes de retornar, asi que `ctx` (en la pila del llamador) no necesita
// sobrevivir mas alla de esta llamada.
static void usb_device_progress_trampoline(const ProgressUpdate *update, void *user_data) {
    UsbDeviceProgressContext *ctx = (UsbDeviceProgressContext *)user_data;
    char combined_phase[128];
    snprintf(combined_phase, sizeof(combined_phase), "%s: %s", ctx->device_name, update->phase ? update->phase : "");

    ProgressUpdate combined = { .phase = combined_phase, .current = update->current, .total = update->total };
    GuiThreadBridgeTarget target = { .handler = on_usb_scan_progress, .ui_user_data = usb_dial, .watch = G_OBJECT(usb_dial) };
    gui_thread_bridge_post(&combined, &target);
}

// ============================================================================
// HILO DE ESCANEO MANUAL (Actualizar / Escaneo Profundo)
// ============================================================================

static void *usb_scan_thread_main(void *arg) {
    (void)arg;

    DeviceList *devices = monitor_connected_devices();
    int device_count = devices ? devices->count : 0;
    int devices_processed = 0;

    for (int i = 0; i < device_count && !usb_cancel_flag; i++) {
        const char *device_name = devices->devices[i];
        UsbDeviceProgressContext ctx = { .device_name = device_name };
        DeviceSnapshot *previous = get_cached_usb_snapshot(device_name);
        int was_cancelled = 0;

        DeviceSnapshot *snapshot = create_device_snapshot_ex(
            device_name, previous, usb_hash_pool,
            usb_device_progress_trampoline, &ctx,
            &usb_cancel_flag, &was_cancelled);

        if (!snapshot || was_cancelled) {
            if (snapshot) free_device_snapshot(snapshot);
            break;
        }

        GUIUSBDevice gui_device;

        if (usb_current_mode == USB_SCAN_MODE_REFRESH) {
            store_usb_snapshot(device_name, snapshot); // el cache toma posesion
            adapt_device_snapshot_to_gui(snapshot, NULL, &gui_device);
            utf8_safe_truncate("ACTUALIZADO", gui_device.status, sizeof(gui_device.status));
            gui_device.files_changed = 0;
            gui_device.is_suspicious = FALSE;
            post_usb_device_update(&gui_device);
        } else if (previous) {
            adapt_device_snapshot_to_gui(snapshot, previous, &gui_device);
            post_usb_device_update(&gui_device);

            if (gui_device.is_suspicious) usb_last_scan_suspicious_devices++;
            usb_last_scan_files_changed += gui_device.files_changed;

            free_device_snapshot(snapshot); // el escaneo profundo nunca sobrescribe la referencia
        } else {
            store_usb_snapshot(device_name, snapshot); // primera vez: se vuelve la linea base
            adapt_device_snapshot_to_gui(snapshot, NULL, &gui_device);
            utf8_safe_truncate("NUEVO", gui_device.status, sizeof(gui_device.status));
            gui_device.files_changed = 0;
            gui_device.is_suspicious = FALSE;
            post_usb_device_update(&gui_device);
        }

        devices_processed++;
    }

    if (devices) free_device_list(devices);

    char summary[128];
    if (usb_cancel_flag) {
        snprintf(summary, sizeof(summary), "Cancelado (%d dispositivos procesados)", devices_processed);
    } else {
        snprintf(summary, sizeof(summary), "Listo: %d dispositivos procesados", devices_processed);
    }
    g_idle_add(G_SOURCE_FUNC(guard_dial_set_progress), usb_dial); // placeholder removido abajo

    return NULL;
}
```

Note on the last line above: `guard_dial_set_progress` takes 4 arguments and cannot be used directly as a `GSourceFunc` — replace that line and the summary handling with the same two-callback shape `gui_demo_scan.c` already uses (`on_scan_done_idle` doing the real work, dispatched via `g_idle_add` with a heap-allocated message). Continue the implementation:

```c
typedef struct {
    int devices_processed;
    int was_cancelled;
} UsbScanDoneMessage;

static gboolean on_usb_scan_done_idle(gpointer data) {
    pthread_join(usb_scan_thread, NULL);

    UsbScanDoneMessage *msg = (UsbScanDoneMessage *)data;
    char summary[128];
    if (msg->was_cancelled) {
        snprintf(summary, sizeof(summary), "Cancelado (%d dispositivos procesados)", msg->devices_processed);
    } else {
        snprintf(summary, sizeof(summary), "Listo: %d dispositivos procesados", msg->devices_processed);
    }
    guard_dial_set_progress(usb_dial, msg->devices_processed, msg->devices_processed, summary);
    free(msg);

    gtk_widget_set_sensitive(usb_refresh_button, TRUE);
    gtk_widget_set_sensitive(usb_deep_scan_button, TRUE);
    gtk_widget_set_sensitive(usb_cancel_button, FALSE);
    usb_scan_running = 0;

    return G_SOURCE_REMOVE;
}
```

Replace the tail of `usb_scan_thread_main` (from the `char summary[128];` line to its `return NULL;`) with:

```c
    UsbScanDoneMessage *msg = malloc(sizeof(UsbScanDoneMessage));
    if (msg) {
        msg->devices_processed = devices_processed;
        msg->was_cancelled = usb_cancel_flag;
        g_idle_add(on_usb_scan_done_idle, msg);
    }

    return NULL;
}
```

And place the `UsbScanDoneMessage`/`on_usb_scan_done_idle` definitions **above** `usb_scan_thread_main` in the file (C requires them declared before use, since there's no forward prototype for either).

Continue with the button handlers, monitor-automation, lifecycle, and the four legacy-named compatibility functions:

```c
static void start_usb_scan(UsbScanMode mode) {
    if (usb_scan_running) {
        gui_add_log_entry("USB_SCANNER", "WARNING", "Escaneo USB ya en progreso");
        return;
    }

    usb_current_mode = mode;
    usb_cancel_flag = 0;
    usb_scan_running = 1;

    gtk_widget_set_sensitive(usb_refresh_button, FALSE);
    gtk_widget_set_sensitive(usb_deep_scan_button, FALSE);
    gtk_widget_set_sensitive(usb_cancel_button, TRUE);
    guard_dial_set_progress(usb_dial, 0, 0, mode == USB_SCAN_MODE_REFRESH ? "Actualizando snapshots..." : "Escaneando en profundidad...");

    gui_add_log_entry("USB_SCANNER", "INFO",
        mode == USB_SCAN_MODE_REFRESH ? "Actualizando snapshots de dispositivos USB"
                                       : "Iniciando escaneo profundo de dispositivos USB");

    if (pthread_create(&usb_scan_thread, NULL, usb_scan_thread_main, NULL) != 0) {
        gui_add_log_entry("USB_SCANNER", "ERROR", "No se pudo crear el hilo de escaneo USB");
        usb_scan_running = 0;
        gtk_widget_set_sensitive(usb_refresh_button, TRUE);
        gtk_widget_set_sensitive(usb_deep_scan_button, TRUE);
        gtk_widget_set_sensitive(usb_cancel_button, FALSE);
        guard_dial_set_progress(usb_dial, 0, 0, "No se pudo iniciar el escaneo");
    }
}

static void on_refresh_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    start_usb_scan(USB_SCAN_MODE_REFRESH);
}

static void on_deep_scan_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    start_usb_scan(USB_SCAN_MODE_DEEP);
}

static void on_cancel_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    usb_cancel_flag = 1;
}

// ============================================================================
// MONITOREO AUTOMATICO (gui_periodic_worker)
// ============================================================================

static void usb_handle_new_device(const char *device_name, volatile sig_atomic_t *cancel) {
    GUIUSBDevice initial = {0};
    utf8_safe_truncate(device_name, initial.device_name, sizeof(initial.device_name));
    snprintf(initial.mount_point, sizeof(initial.mount_point), "/media/%s", device_name);
    utf8_safe_truncate("DETECTADO", initial.status, sizeof(initial.status));
    initial.last_scan = time(NULL);
    post_usb_device_update(&initial);

    int was_cancelled = 0;
    DeviceSnapshot *snapshot = create_device_snapshot_ex(device_name, NULL, usb_hash_pool, NULL, NULL, cancel, &was_cancelled);
    if (!snapshot || was_cancelled) {
        if (snapshot) free_device_snapshot(snapshot);
        return;
    }

    store_usb_snapshot(device_name, snapshot);
    GUIUSBDevice gui_device;
    adapt_device_snapshot_to_gui(snapshot, NULL, &gui_device);
    utf8_safe_truncate("NUEVO", gui_device.status, sizeof(gui_device.status));
    gui_device.files_changed = 0;
    gui_device.is_suspicious = FALSE;
    post_usb_device_update(&gui_device);
}

static void usb_auto_monitor_work(volatile sig_atomic_t *cancel, void *user_data) {
    (void)user_data;
    // Estatico: solo el hilo del periodic worker toca `previous`, un unico
    // consumidor siempre conocido -- no necesita mutex propio.
    static DeviceList *previous = NULL;

    DeviceList *current = monitor_connected_devices();

    if (current && previous) {
        for (int i = 0; i < current->count && !*cancel; i++) {
            int found = 0;
            for (int j = 0; j < previous->count; j++) {
                if (strcmp(current->devices[i], previous->devices[j]) == 0) { found = 1; break; }
            }
            if (!found) usb_handle_new_device(current->devices[i], cancel);
        }
        for (int i = 0; i < previous->count; i++) {
            int found = 0;
            for (int j = 0; j < current->count; j++) {
                if (strcmp(previous->devices[i], current->devices[j]) == 0) { found = 1; break; }
            }
            if (!found) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Dispositivo USB desconectado: %s", previous->devices[i]);
                gui_add_log_entry("USB_MONITOR", "INFO", msg);
            }
        }
    } else if (current && !previous) {
        for (int i = 0; i < current->count && !*cancel; i++) {
            usb_handle_new_device(current->devices[i], cancel);
        }
    }

    if (previous) free_device_list(previous);
    previous = current;
}

// ============================================================================
// CICLO DE VIDA DEL PANEL
// ============================================================================

GtkWidget *gui_usb_panel_create(void) {
    init_usb_snapshot_cache();
    usb_hash_pool = threadpool_create(USB_HASH_POOL_THREADS);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new("Dispositivos USB");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new(""), TRUE, TRUE, 0);

    usb_cancel_button = gtk_button_new_with_label("Cancelar");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_cancel_button), "nw-button-secondary");
    gtk_widget_set_sensitive(usb_cancel_button, FALSE);
    g_signal_connect(usb_cancel_button, "clicked", G_CALLBACK(on_cancel_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), usb_cancel_button, FALSE, FALSE, 0);

    usb_deep_scan_button = gtk_button_new_with_label("Escaneo Profundo");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_deep_scan_button), "nw-button-secondary");
    gtk_widget_set_tooltip_text(usb_deep_scan_button, "Comparar con el snapshot de referencia sin modificarlo");
    g_signal_connect(usb_deep_scan_button, "clicked", G_CALLBACK(on_deep_scan_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), usb_deep_scan_button, FALSE, FALSE, 0);

    usb_refresh_button = gtk_button_new_with_label("Actualizar");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_refresh_button), "nw-button-primary");
    gtk_widget_set_tooltip_text(usb_refresh_button, "Tomar nuevos snapshots de referencia");
    g_signal_connect(usb_refresh_button, "clicked", G_CALLBACK(on_refresh_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), usb_refresh_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), toolbar, FALSE, FALSE, 0);

    usb_dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(panel), usb_dial, FALSE, FALSE, 0);

    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);

    usb_list_box = gtk_list_box_new();
    g_signal_connect(usb_list_box, "row-selected", G_CALLBACK(on_usb_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), usb_list_box);
    gtk_box_pack_start(GTK_BOX(content_box), scrolled, TRUE, TRUE, 0);

    usb_detail_label = gtk_label_new("Seleccione un dispositivo para ver detalles");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_detail_label), "nw-row-secondary");
    gtk_label_set_line_wrap(GTK_LABEL(usb_detail_label), TRUE);
    gtk_widget_set_size_request(usb_detail_label, 220, -1);
    gtk_widget_set_valign(usb_detail_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content_box), usb_detail_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), content_box, TRUE, TRUE, 0);

    usb_auto_monitor_worker = gui_periodic_worker_start(USB_AUTO_MONITOR_INTERVAL_SECONDS, usb_auto_monitor_work, NULL);

    return panel;
}

void gui_usb_panel_shutdown(void) {
    gui_periodic_worker_stop(usb_auto_monitor_worker);
    usb_auto_monitor_worker = NULL;

    if (usb_scan_running) {
        usb_cancel_flag = 1;
        pthread_join(usb_scan_thread, NULL);
        usb_scan_running = 0;
    }

    if (usb_hash_pool) {
        threadpool_destroy(usb_hash_pool);
        usb_hash_pool = NULL;
    }

    cleanup_usb_snapshot_cache();
}

// ============================================================================
// COMPATIBILIDAD CON EL COORDINADOR Y EL MENU "ESCANEAR" DEL HEADER BAR
// ============================================================================

int is_usb_monitoring_active(void) {
    return usb_auto_monitor_worker != NULL;
}

int is_gui_usb_scan_in_progress(void) {
    return usb_scan_running;
}

int get_usb_statistics_for_gui(int *total_devices, int *suspicious_devices,
                               int *total_files, int *files_with_changes) {
    if (!total_devices || !suspicious_devices || !total_files || !files_with_changes) {
        return -1;
    }

    *total_devices = 0;
    *total_files = 0;

    DeviceList *devices = monitor_connected_devices();
    if (devices) {
        *total_devices = devices->count;
        for (int i = 0; i < devices->count; i++) {
            DeviceSnapshot *snapshot = get_cached_usb_snapshot(devices->devices[i]);
            if (snapshot) *total_files += snapshot->file_count;
        }
        free_device_list(devices);
    }

    // Igual que el codigo anterior: suspicious/changed reflejan el ultimo
    // escaneo profundo -- un simple "Actualizar" no los toca.
    *suspicious_devices = usb_last_scan_suspicious_devices;
    *files_with_changes = usb_last_scan_files_changed;

    return 0;
}

void gui_compatible_scan_usb(void) {
    if (usb_scan_running) {
        gui_add_log_entry("USB_SCANNER", "INFO", "Escaneo USB ya en progreso");
        return;
    }
    start_usb_scan(USB_SCAN_MODE_REFRESH);
}
```

- [ ] **Step 3: Delete the old files**

```bash
rm src/gui/window/gui_usb_panel.c src/gui/integration/gui_usb_integration.c
```

- [ ] **Step 4: Update `gui_main.c`**

- Replace `#include "gui_usb_integration.h"` with `#include "gui_usb_panel.h"`.
- In `initialize_complete_backend_system()`, delete the `if (init_usb_integration() != 0) { ... }` block, the `if (start_usb_monitoring(30) != 0) { ... } else { ...; notify_module_status_change("usb", ...); }` block, and their log lines.
- In `cleanup_complete_backend_system()`, delete the `cleanup_usb_integration();` call and its surrounding log lines.
- In `on_window_destroy`, add `gui_usb_panel_shutdown();` immediately after `gui_logs_panel`... i.e. right before the `cleanup_complete_backend_system();` call (there is no demo-scan call left to anchor against after Task 5 — place it as the first line of the shutdown sequence, matching the ordering invariant in Global Constraints).
- In `init_gui`, after the logs-page mounting block from Task 6, add:
```c
    GtkWidget *usb_page = gui_shell_get_page_container(1);
    if (usb_page != NULL) {
        gtk_box_pack_start(GTK_BOX(usb_page), gui_usb_panel_create(), TRUE, TRUE, 0);
    }
```

- [ ] **Step 5: Update the Makefile**

Change `src/gui/window/gui_usb_panel.c` to `src/gui/panels/gui_usb_panel.c`. Remove `src/gui/integration/gui_usb_integration.c` from `SRC`.

- [ ] **Step 6: Build**

Run: `make clean && make all 2>&1 | tail -40`
Expected: zero warnings. (`gui_system_coordinator.c`, still unmodified, keeps linking against `is_usb_monitoring_active`/`is_gui_usb_scan_in_progress`/`get_usb_statistics_for_gui`, now defined in the new file.)

- [ ] **Step 7: Manual visual verification (WSLg + screenshot)**

Launch `./matcom-guard`. On the USB page: confirm the toolbar (Actualizar / Escaneo Profundo / Cancelar), the guard dial, and an empty list initially (unless a USB device is actually mounted under `/media` in the WSL environment — if none is, connect a virtual/loopback mount or accept an empty list as a valid state for this check). Click "Actualizar": confirm the dial animates and the button re-enables when done. If any device exists, confirm its row shows a colored dot, device name, mount point + file counts in monospace, and a status badge; click the row and confirm the detail label updates.

- [ ] **Step 8: Commit**

```bash
git add include/gui_usb_panel.h src/gui/panels/gui_usb_panel.c src/gui/gui_main.c Makefile
git rm src/gui/window/gui_usb_panel.c src/gui/integration/gui_usb_integration.c
git commit -m "feat: add gui_usb_panel wired to the real backend via guard_dial/gui_thread_bridge"
```

---

## Task 8: `gui_process_panel` — replaces the old process panel + process integration, real kill, bridged backend callbacks

**Correction versus the spec:** `process_monitor.c`'s own `monitoring_thread_function` invokes every registered `ProcessCallbacks` callback (`on_new_process`, `on_high_cpu_alert`, etc.) **directly from its own background thread** (confirmed by reading `src/process_monitor.c`) — the old `gui_process_integration.c` then called `gui_update_process()` straight from inside those callbacks, touching a `GtkListBox`/`GtkListStore` from a non-GTK thread. This is exactly the class of bug the whole rewrite exists to close (the spec calls this out explicitly for the coordinator; the same gap exists here and gets the same fix). Every one of those four callbacks now heap-copies its already-adapted `GUIProcess` and dispatches through `g_idle_add`, matching `gui_logs_panel`'s established `add_log_entry_main_thread` idiom. The backend's own `ProcessCallbacks.on_status_update` field is a `ProgressCallback` — it is wired directly to `gui_thread_bridge_post`, no wrapper needed.

**Deliberate improvement (not a spec change, a bug fix within this file's own scope):** the old "Terminar Proceso" button never actually killed anything (`// Aqui iria el codigo real para terminar el proceso / Por ahora, solo simulamos` — it just removed the row). This rewrite sends a real `kill(pid, SIGTERM)`. The confirmation dialog and row-removal-on-confirm UX are unchanged.

**Files:**
- Create: `include/gui_process_panel.h`
- Create: `src/gui/panels/gui_process_panel.c`
- Delete: `src/gui/window/gui_process_panel.c`, `src/gui/integration/gui_process_integration.c`
- Modify: `src/gui/gui_main.c` (swap include, trim init/cleanup calls, add shutdown call, mount page 2)
- Modify: `Makefile` (SRC updates)

**Interfaces:**
- Produces: `GtkWidget *gui_process_panel_create(void)`, `void gui_process_panel_shutdown(void)`, `void gui_compatible_scan_processes(void)` (`include/gui_process_panel.h`).
- Produces (matching `include/gui_process_integration.h`'s existing declarations, still needed by `gui_system_coordinator.c` and by `gui_main.c`'s `intelligent_system_sync` until their own tasks): `int is_process_monitoring_active(void)`, `int get_process_statistics_for_gui(int*, int*, int*, int*)`, `int sync_gui_with_backend_processes(void)`.
- Consumes: `load_config`, `start_monitoring`, `stop_monitoring`, `is_monitoring_active`, `set_process_callbacks`, `get_monitoring_stats`, `get_process_list_copy`, `cleanup_monitoring` (`process_monitor.h`, backend, unchanged); `adapt_process_info_to_gui` (Task 4); `guard_dial_new`/`guard_dial_set_progress`, `gui_thread_bridge_post`/`GuiThreadBridgeTarget` (Part A).

- [ ] **Step 1: Write the header**

```c
// include/gui_process_panel.h
#ifndef GUI_PROCESS_PANEL_H
#define GUI_PROCESS_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Procesos: lista de procesos, dial de monitoreo, umbrales, terminar. */
GtkWidget *gui_process_panel_create(void);

/** Detiene el monitoreo de procesos si esta activo y libera sus recursos. */
void gui_process_panel_shutdown(void);

/** Callback compatible con ScanProcessesCallback (gui.h) para el menu "Escanear". */
void gui_compatible_scan_processes(void);

#endif // GUI_PROCESS_PANEL_H
```

- [ ] **Step 2: Write the implementation**

```c
// src/gui/panels/gui_process_panel.c
#include "gui_process_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include "gui_backend_adapters.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "process_monitor.h"
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *dot;
    GtkWidget *primary;
    GtkWidget *secondary;
    GtkWidget *badge;
} ProcessRowRefs;

static GtkWidget *process_list_box = NULL;
static GtkWidget *process_dial = NULL;
static GtkWidget *process_kill_button = NULL;
static GtkWidget *process_cpu_threshold_spin = NULL;
static GtkWidget *process_mem_threshold_spin = NULL;

static ProcessCallbacks process_backend_callbacks;
static GuiThreadBridgeTarget process_progress_target;

// ============================================================================
// FILA DE LISTA
// ============================================================================

static void apply_process_status_classes(GtkWidget *dot, GtkWidget *badge, const char *level) {
    GtkStyleContext *dot_ctx = gtk_widget_get_style_context(dot);
    GtkStyleContext *badge_ctx = gtk_widget_get_style_context(badge);

    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-normal");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-warning");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-critical");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-normal");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-warning");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-critical");
    gtk_style_context_add_class(badge_ctx, "nw-row-badge");

    char dot_class[32], badge_class[32];
    snprintf(dot_class, sizeof(dot_class), "nw-row-dot-%s", level);
    snprintf(badge_class, sizeof(badge_class), "nw-row-badge-%s", level);
    gtk_style_context_add_class(dot_ctx, dot_class);
    gtk_style_context_add_class(badge_ctx, badge_class);
}

static GtkWidget *find_row_by_pid(pid_t pid) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(process_list_box));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL; l = l->next) {
        pid_t row_pid = (pid_t)(intptr_t)g_object_get_data(G_OBJECT(l->data), "pid");
        if (row_pid == pid) { found = GTK_WIDGET(l->data); break; }
    }
    g_list_free(children);
    return found;
}

static void on_process_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)data;
    gtk_widget_set_sensitive(process_kill_button, row != NULL);
}

// gui_update_process() (gui.h) solo debe llamarse desde el hilo principal --
// las cuatro callbacks de ProcessCallbacks pasan por post_process_update().
void gui_update_process(GUIProcess *process) {
    if (!process_list_box || !process) return;

    gdouble cpu_threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(process_cpu_threshold_spin));
    gdouble mem_threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(process_mem_threshold_spin));

    const char *status;
    const char *level;
    if (process->is_whitelisted) {
        status = "Whitelisted"; level = "normal";
    } else if (process->is_suspicious) {
        status = "SOSPECHOSO"; level = "critical";
    } else if (process->cpu_usage > cpu_threshold && process->mem_usage > mem_threshold) {
        status = "Alto CPU+RAM"; level = "critical";
    } else if (process->cpu_usage > cpu_threshold) {
        status = "Alto CPU"; level = "warning";
    } else if (process->mem_usage > mem_threshold) {
        status = "Alta Memoria"; level = "warning";
    } else {
        status = "Normal"; level = "normal";
    }

    GtkWidget *row = find_row_by_pid(process->pid);
    ProcessRowRefs *refs;

    if (!row) {
        row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "nw-list-row");

        GtkWidget *dot = gtk_label_new("\xE2\x97\x8F");
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *primary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(primary), "nw-row-primary");
        gtk_widget_set_halign(primary, GTK_ALIGN_START);
        GtkWidget *secondary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(secondary), "nw-row-secondary");
        gtk_widget_set_halign(secondary, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(text_box), primary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), secondary, FALSE, FALSE, 0);
        GtkWidget *badge = gtk_label_new("");

        gtk_box_pack_start(GTK_BOX(hbox), dot, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), text_box, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), badge, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);

        refs = malloc(sizeof(ProcessRowRefs));
        refs->dot = dot;
        refs->primary = primary;
        refs->secondary = secondary;
        refs->badge = badge;
        g_object_set_data(G_OBJECT(row), "pid", GINT_TO_POINTER(process->pid));
        g_object_set_data_full(G_OBJECT(row), "row-refs", refs, free);

        gtk_list_box_insert(GTK_LIST_BOX(process_list_box), row, -1);
        gtk_widget_show_all(row);
    } else {
        refs = g_object_get_data(G_OBJECT(row), "row-refs");
    }

    gtk_label_set_text(GTK_LABEL(refs->primary), process->name);
    char secondary_text[128];
    snprintf(secondary_text, sizeof(secondary_text), "PID %d . CPU %.1f%% . MEM %.1f%%",
             process->pid, process->cpu_usage, process->mem_usage);
    gtk_label_set_text(GTK_LABEL(refs->secondary), secondary_text);
    gtk_label_set_text(GTK_LABEL(refs->badge), status);
    apply_process_status_classes(refs->dot, refs->badge, level);

    gboolean exceeds = process->cpu_usage > cpu_threshold || process->mem_usage > mem_threshold;
    if ((process->is_suspicious || exceeds) && !process->is_whitelisted) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Proceso '%s' (PID: %d) - CPU: %.1f%%, RAM: %.1f%% - Estado: %s",
                 process->name, process->pid, process->cpu_usage, process->mem_usage, status);
        gui_add_log_entry("PROCESS_MONITOR", process->is_suspicious ? "ALERT" : "WARNING", log_msg);
    } else if ((process->is_suspicious || exceeds) && process->is_whitelisted) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Proceso whitelisted '%s' (PID: %d) - CPU: %.1f%%, RAM: %.1f%% - Estado: %s",
                 process->name, process->pid, process->cpu_usage, process->mem_usage, status);
        gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
    }
}

typedef struct { GUIProcess process; } ProcessUpdateMsg;

static gboolean process_update_idle(gpointer data) {
    ProcessUpdateMsg *msg = (ProcessUpdateMsg *)data;
    gui_update_process(&msg->process);
    free(msg);
    return G_SOURCE_REMOVE;
}

static void post_process_update(const GUIProcess *process) {
    ProcessUpdateMsg *msg = malloc(sizeof(ProcessUpdateMsg));
    if (!msg) return;
    msg->process = *process;
    g_idle_add(process_update_idle, msg);
}

// ============================================================================
// CALLBACKS DEL BACKEND (corren en el hilo de monitoreo de process_monitor.c)
// ============================================================================

static void on_backend_new_process(ProcessInfo *info) {
    if (!info) return;
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Nuevo proceso detectado: %s (PID: %d) - CPU: %.1f%%, MEM: %.1f%%",
             info->name, info->pid, info->cpu_usage, info->mem_usage);
    gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
}

static void on_backend_process_terminated(pid_t pid, const char *name) {
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Proceso terminado: %s (PID: %d)", name ? name : "desconocido", pid);
    gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
}

static void on_backend_high_cpu_alert(ProcessInfo *info) {
    if (!info) return;
    if (info->is_whitelisted) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Intento de alerta CPU para proceso whitelisted '%s' (PID: %d)", info->name, info->pid);
        gui_add_log_entry("PROCESS_MONITOR", "WARNING", msg);
        return;
    }
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "ALERTA CPU: Proceso '%s' (PID: %d) usando %.1f%% de CPU",
             info->name, info->pid, info->cpu_usage);
    gui_add_log_entry("PROCESS_MONITOR", "ALERT", log_msg);
}

static void on_backend_high_memory_alert(ProcessInfo *info) {
    if (!info) return;
    if (info->is_whitelisted) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Intento de alerta RAM para proceso whitelisted '%s' (PID: %d)", info->name, info->pid);
        gui_add_log_entry("PROCESS_MONITOR", "WARNING", msg);
        return;
    }
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "ALERTA MEMORIA: Proceso '%s' (PID: %d) usando %.1f%% de RAM",
             info->name, info->pid, info->mem_usage);
    gui_add_log_entry("PROCESS_MONITOR", "ALERT", log_msg);
}

static void on_backend_alert_cleared(ProcessInfo *info) {
    if (!info) return;
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Alerta despejada: Proceso '%s' (PID: %d) volvio a valores normales",
             info->name, info->pid);
    gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
}

static void on_process_status_update(const ProgressUpdate *update, void *ui_user_data) {
    guard_dial_set_progress((GtkWidget *)ui_user_data, update->current, update->total, update->phase);
}

// ============================================================================
// BOTONES
// ============================================================================

static int process_sync_from_backend(void) {
    int count = 0;
    ProcessInfo *list = get_process_list_copy(&count);
    if (!list) {
        gui_add_log_entry("PROCESS_INTEGRATION", "INFO", "No hay procesos monitoreados para sincronizar");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        GUIProcess gp;
        if (adapt_process_info_to_gui(&list[i], &gp) == 0) {
            gui_update_process(&gp); // ya estamos en el hilo principal (boton o intelligent_system_sync)
        }
    }
    free(list);
    return count;
}

static void on_scan_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    if (!is_monitoring_active()) {
        gui_add_log_entry("PROCESS_SCANNER", "INFO", "Iniciando monitoreo de procesos del sistema");
        if (start_monitoring() != 0) {
            gui_add_log_entry("PROCESS_SCANNER", "ERROR", "No se pudo iniciar el monitoreo de procesos");
            return;
        }
        guard_dial_set_progress(process_dial, 0, 0, "Monitoreo activo");
    } else {
        gui_add_log_entry("PROCESS_SCANNER", "INFO", "Actualizando vista de procesos...");
        process_sync_from_backend();
    }
}

static void on_kill_process_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(process_list_box));
    if (!row) return;

    pid_t pid = (pid_t)(intptr_t)g_object_get_data(G_OBJECT(row), "pid");
    ProcessRowRefs *refs = g_object_get_data(G_OBJECT(row), "row-refs");
    char name[256];
    utf8_safe_truncate(gtk_label_get_text(GTK_LABEL(refs->primary)), name, sizeof(name));

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
        "Esta seguro de que desea terminar el proceso '%s' (PID: %d)?", name, pid);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_YES) return;

    char log_msg[256];
    if (kill(pid, SIGTERM) == 0) {
        snprintf(log_msg, sizeof(log_msg), "Senal SIGTERM enviada a '%s' (PID: %d)", name, pid);
        gui_add_log_entry("PROCESS_MANAGER", "WARNING", log_msg);
    } else {
        snprintf(log_msg, sizeof(log_msg), "No se pudo terminar '%s' (PID: %d): %s", name, pid, strerror(errno));
        gui_add_log_entry("PROCESS_MANAGER", "ERROR", log_msg);
    }

    gtk_widget_destroy(GTK_WIDGET(row));
    gtk_widget_set_sensitive(process_kill_button, FALSE);
}

static void on_threshold_changed(GtkSpinButton *spin, gpointer data) {
    const char *type = (const char *)data;
    gdouble value = gtk_spin_button_get_value(spin);
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Umbral de %s (panel) cambiado a %.0f%%", type, value);
    gui_add_log_entry("CONFIG", "INFO", log_msg);
}

// ============================================================================
// CICLO DE VIDA DEL PANEL
// ============================================================================

GtkWidget *gui_process_panel_create(void) {
    load_config();

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new("Procesos del Sistema");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new(""), TRUE, TRUE, 0);

    process_kill_button = gtk_button_new_with_label("Terminar Proceso");
    gtk_style_context_add_class(gtk_widget_get_style_context(process_kill_button), "nw-button-secondary");
    gtk_widget_set_sensitive(process_kill_button, FALSE);
    g_signal_connect(process_kill_button, "clicked", G_CALLBACK(on_kill_process_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), process_kill_button, FALSE, FALSE, 0);

    GtkWidget *scan_button = gtk_button_new_with_label("Escanear Procesos");
    gtk_style_context_add_class(gtk_widget_get_style_context(scan_button), "nw-button-primary");
    g_signal_connect(scan_button, "clicked", G_CALLBACK(on_scan_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), scan_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), toolbar, FALSE, FALSE, 0);

    process_dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(panel), process_dial, FALSE, FALSE, 0);
    process_progress_target.handler = on_process_status_update;
    process_progress_target.ui_user_data = process_dial;
    process_progress_target.watch = G_OBJECT(process_dial);

    GtkWidget *threshold_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(threshold_row), gtk_label_new("Umbral CPU (%):"), FALSE, FALSE, 0);
    process_cpu_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(process_cpu_threshold_spin), 70.0);
    g_signal_connect(process_cpu_threshold_spin, "value-changed", G_CALLBACK(on_threshold_changed), "CPU");
    gtk_box_pack_start(GTK_BOX(threshold_row), process_cpu_threshold_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(threshold_row), gtk_label_new("Umbral Memoria (%):"), FALSE, FALSE, 0);
    process_mem_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(process_mem_threshold_spin), 50.0);
    g_signal_connect(process_mem_threshold_spin, "value-changed", G_CALLBACK(on_threshold_changed), "Memoria");
    gtk_box_pack_start(GTK_BOX(threshold_row), process_mem_threshold_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), threshold_row, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    process_list_box = gtk_list_box_new();
    g_signal_connect(process_list_box, "row-selected", G_CALLBACK(on_process_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), process_list_box);
    gtk_box_pack_start(GTK_BOX(panel), scrolled, TRUE, TRUE, 0);

    memset(&process_backend_callbacks, 0, sizeof(process_backend_callbacks));
    process_backend_callbacks.on_new_process = on_backend_new_process;
    process_backend_callbacks.on_process_terminated = on_backend_process_terminated;
    process_backend_callbacks.on_high_cpu_alert = on_backend_high_cpu_alert;
    process_backend_callbacks.on_high_memory_alert = on_backend_high_memory_alert;
    process_backend_callbacks.on_alert_cleared = on_backend_alert_cleared;
    process_backend_callbacks.on_status_update = gui_thread_bridge_post;
    process_backend_callbacks.status_user_data = &process_progress_target;
    set_process_callbacks(&process_backend_callbacks);

    return panel;
}

void gui_process_panel_shutdown(void) {
    if (is_monitoring_active()) {
        stop_monitoring();
    }
    cleanup_monitoring();
}

// ============================================================================
// COMPATIBILIDAD CON EL COORDINADOR, intelligent_system_sync Y EL MENU "ESCANEAR"
// ============================================================================

int is_process_monitoring_active(void) {
    return is_monitoring_active();
}

int get_process_statistics_for_gui(int *total_processes, int *high_cpu_count,
                                   int *high_memory_count, int *suspicious_count) {
    if (!total_processes || !high_cpu_count || !high_memory_count || !suspicious_count) {
        return -1;
    }
    MonitoringStats stats = get_monitoring_stats();
    *total_processes = stats.total_processes;
    *high_cpu_count = stats.high_cpu_count;
    *high_memory_count = stats.high_memory_count;
    *suspicious_count = stats.active_alerts;
    return 0;
}

int sync_gui_with_backend_processes(void) {
    return process_sync_from_backend();
}

void gui_compatible_scan_processes(void) {
    if (!is_monitoring_active()) {
        if (start_monitoring() != 0) {
            gui_add_log_entry("PROCESS_INTEGRATION", "ERROR", "Error al iniciar monitoreo de procesos");
            return;
        }
        guard_dial_set_progress(process_dial, 0, 0, "Monitoreo activo");
    } else {
        process_sync_from_backend();
    }
}
```

- [ ] **Step 3: Delete the old files**

```bash
rm src/gui/window/gui_process_panel.c src/gui/integration/gui_process_integration.c
```

- [ ] **Step 4: Update `gui_main.c`**

- Replace `#include "gui_process_integration.h"` with `#include "gui_process_panel.h"`.
- In `initialize_complete_backend_system()`, delete the `if (init_process_integration() != 0) { ... }` block and its success log line (keep the `notify_module_status_change("process", MODULE_STATUS_INACTIVE, "Listo para iniciar bajo demanda");` line — it's still accurate, processes still start on demand).
- In `cleanup_complete_backend_system()`, delete the `cleanup_process_integration();` call and its surrounding log lines.
- In `on_window_destroy`, add `gui_process_panel_shutdown();` alongside the `gui_usb_panel_shutdown();` call added in Task 7.
- In `init_gui`, after the USB-page mounting block from Task 7, add:
```c
    GtkWidget *process_page = gui_shell_get_page_container(2);
    if (process_page != NULL) {
        gtk_box_pack_start(GTK_BOX(process_page), gui_process_panel_create(), TRUE, TRUE, 0);
    }
```

- [ ] **Step 5: Update the Makefile**

Change `src/gui/window/gui_process_panel.c` to `src/gui/panels/gui_process_panel.c`. Remove `src/gui/integration/gui_process_integration.c` from `SRC`.

- [ ] **Step 6: Build**

Run: `make clean && make all 2>&1 | tail -40`
Expected: zero warnings.

- [ ] **Step 7: Manual visual verification (WSLg + screenshot)**

Launch `./matcom-guard`. On the Processes page: click "Escanear Procesos" — confirm the dial shows "Monitoreo activo" and rows start appearing (real processes from the host). Select a row, confirm "Terminar Proceso" enables; pick a harmless test process (e.g. spawn `sleep 300 &` in the WSL shell first) and confirm the confirmation dialog appears, and that after confirming, the process is actually gone from `ps` output (not just the list). Adjust the CPU/Memory threshold spinbuttons and confirm row badges/colors update accordingly on the next refresh.

- [ ] **Step 8: Commit**

```bash
git add include/gui_process_panel.h src/gui/panels/gui_process_panel.c src/gui/gui_main.c Makefile
git rm src/gui/window/gui_process_panel.c src/gui/integration/gui_process_integration.c
git commit -m "feat: add gui_process_panel with bridged backend callbacks and a real process kill"
```

---

## Task 9: `gui_ports_panel` — replaces the old ports panel + ports integration, switches to the real `scan_ports_range`

**Correction versus the spec (architectural finding from grounding, not just a style rewrite):** the old `gui_ports_integration.c` does **not** use the already-merged backend's `scan_ports_range`/`get_port_service_name`/`is_port_suspicious` at all. It has its own hand-rolled, single-port-at-a-time `port_scanning_thread_function` with a **duplicate, hardcoded service-classification table** baked into the loop (ports 21/22/23/80/443/3389/5900/8080/31337/4444/6667 mapped inline). This predates the backend rewrite and was simply never switched over. This task replaces that entire hand-rolled thread with one call to `scan_ports_range` — the exact same real, multi-threaded, cancellable, `ProgressCallback`-driven backend function `gui_demo_scan.c` already proved out in Part A — eliminating the duplicate classification logic entirely (the real classification comes from `port_scanner.h`'s `get_port_service_name`/`is_port_suspicious`, invoked internally by `scan_ports_range` itself). A small local table of human-readable descriptions is kept **only** as detail-panel flavor text (never for suspicion classification).

**Files:**
- Create: `include/gui_ports_panel.h`
- Create: `src/gui/panels/gui_ports_panel.c`
- Delete: `src/gui/window/gui_ports_panel.c`, `src/gui/integration/gui_ports_integration.c`
- Modify: `src/gui/gui_main.c` (swap include, trim init/cleanup calls, add shutdown call, mount page 3)
- Modify: `Makefile` (SRC updates)

**Interfaces:**
- Produces: `GtkWidget *gui_ports_panel_create(void)`, `void gui_ports_panel_shutdown(void)`, `void gui_compatible_scan_ports(void)` (`include/gui_ports_panel.h`).
- Produces (matching `include/gui_ports_integration.h`'s and `gui_internal.h`'s existing declarations, still needed by `gui_system_coordinator.c` until Task 11): `int is_port_scan_active(void)`, `int get_port_statistics_for_gui(int*, int*, time_t*)`, `void on_scan_ports_clicked(GtkButton*, gpointer)` (the last one is declared in the unchanged `gui_internal.h`, reused internally the same way the old file did, for the Quick/Full preset buttons to invoke the range-scan path).
- Consumes: `scan_ports_range` (`port_scanner.h`, backend, unchanged — the actual new dependency this task adds), `adapt_port_info_to_gui`/`aggregate_port_statistics` (Task 4), `guard_dial_new`/`guard_dial_set_progress`, `gui_thread_bridge_post`/`GuiThreadBridgeTarget` (Part A).
- **Not reimplemented (confirmed dead code — no caller exists anywhere in the codebase):** `get_last_scan_results`, `generate_port_scan_report`, the `PortScanConfig`/`PortScanType` types, `perform_quick_port_scan`/`perform_full_port_scan`/`perform_custom_port_scan`, `cancel_port_scan`, `get_port_scan_progress`. These stay declared in the not-yet-deleted `include/gui_ports_integration.h` (harmless, matching the same header's harmless declared-but-newly-unimplemented functions from the USB/Process panels) and disappear along with that header in Task 11.

- [ ] **Step 1: Write the header**

```c
// include/gui_ports_panel.h
#ifndef GUI_PORTS_PANEL_H
#define GUI_PORTS_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Puertos: rango configurable, presets, dial, resultados. */
GtkWidget *gui_ports_panel_create(void);

/** Cancela cualquier escaneo en curso y hace join del hilo. */
void gui_ports_panel_shutdown(void);

/** Callback compatible con ScanPortsCallback (gui.h) para el menu "Escanear". */
void gui_compatible_scan_ports(void);

#endif // GUI_PORTS_PANEL_H
```

- [ ] **Step 2: Write the implementation**

```c
// src/gui/panels/gui_ports_panel.c
#include "gui_ports_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include "gui_backend_adapters.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "port_scanner.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PORT_SCAN_THREADS 50

typedef struct {
    GtkWidget *dot;
    GtkWidget *primary;
    GtkWidget *secondary;
    GtkWidget *badge;
} PortRowRefs;

typedef struct {
    int port;
    const char *description;
} PortDescription;

// Solo texto informativo para el panel de detalle -- la clasificacion real
// de sospecha viene de is_port_suspicious()/get_port_service_name() dentro
// de scan_ports_range(), no de esta tabla.
static const PortDescription port_descriptions[] = {
    {20, "FTP Data Transfer"}, {21, "File Transfer Protocol"}, {22, "Secure Shell"},
    {23, "Telnet (inseguro)"}, {25, "Simple Mail Transfer"}, {53, "Domain Name System"},
    {80, "Web Server"}, {110, "Post Office Protocol"}, {143, "Internet Message Access"},
    {443, "Secure Web Server"}, {445, "Server Message Block"}, {3306, "MySQL Database"},
    {5432, "PostgreSQL Database"}, {8080, "Alternative HTTP"}, {8443, "Alternative HTTPS"},
    {3389, "Remote Desktop"}, {5900, "Virtual Network Computing"}, {6379, "Redis Database"},
    {27017, "MongoDB Database"}, {31337, "Elite - Backdoor comun"}, {4444, "Metasploit default"},
    {6666, "IRC - a menudo usado por botnets"}, {6667, "IRC - a menudo usado por botnets"},
    {12345, "NetBus backdoor"},
    {0, NULL}
};

static const char *get_port_description(int port) {
    for (int i = 0; port_descriptions[i].description != NULL; i++) {
        if (port_descriptions[i].port == port) return port_descriptions[i].description;
    }
    return "Sin descripcion disponible";
}

static GtkWidget *ports_list_box = NULL;
static GtkWidget *ports_dial = NULL;
static GtkWidget *ports_detail_label = NULL;
static GtkWidget *port_start_spin = NULL;
static GtkWidget *port_end_spin = NULL;
static GtkWidget *ports_scan_button = NULL;
static GtkWidget *ports_quick_button = NULL;
static GtkWidget *ports_full_button = NULL;
static GtkWidget *ports_cancel_button = NULL;

static pthread_t port_scan_thread;
static volatile sig_atomic_t port_cancel_flag = 0;
static volatile sig_atomic_t port_scan_running = 0;
static int port_scan_start_value = 1;
static int port_scan_end_value = 1024;

static int port_last_total_open = 0;
static int port_last_total_suspicious = 0;
static time_t port_last_scan_time = 0;

// ============================================================================
// FILA DE LISTA
// ============================================================================

static void apply_port_status_classes(GtkWidget *dot, GtkWidget *badge, const char *level) {
    GtkStyleContext *dot_ctx = gtk_widget_get_style_context(dot);
    GtkStyleContext *badge_ctx = gtk_widget_get_style_context(badge);

    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-normal");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-warning");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-critical");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-normal");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-warning");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-critical");
    gtk_style_context_add_class(badge_ctx, "nw-row-badge");

    char dot_class[32], badge_class[32];
    snprintf(dot_class, sizeof(dot_class), "nw-row-dot-%s", level);
    snprintf(badge_class, sizeof(badge_class), "nw-row-badge-%s", level);
    gtk_style_context_add_class(dot_ctx, dot_class);
    gtk_style_context_add_class(badge_ctx, badge_class);
}

static GtkWidget *find_row_by_port(int port) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(ports_list_box));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL; l = l->next) {
        int row_port = (int)(intptr_t)g_object_get_data(G_OBJECT(l->data), "port");
        if (row_port == port) { found = GTK_WIDGET(l->data); break; }
    }
    g_list_free(children);
    return found;
}

static void on_port_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)data;
    if (!row) return;
    int port = (int)(intptr_t)g_object_get_data(G_OBJECT(row), "port");
    PortRowRefs *refs = g_object_get_data(G_OBJECT(row), "row-refs");
    if (!refs) return;

    char detail[512];
    snprintf(detail, sizeof(detail), "<b>%s</b>\n%s\n\n<i>%s</i>",
             gtk_label_get_text(GTK_LABEL(refs->primary)),
             gtk_label_get_text(GTK_LABEL(refs->secondary)),
             get_port_description(port));
    gtk_label_set_markup(GTK_LABEL(ports_detail_label), detail);
}

// Llamada solo desde el hilo principal: on_port_scan_done_idle (idle
// callback de GLib) ya corre ahi, asi que no hace falta bridging adicional.
void gui_update_port(GUIPort *port) {
    if (!ports_list_box || !port) return;

    GtkWidget *row = find_row_by_port(port->port);
    PortRowRefs *refs;

    if (!row) {
        row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "nw-list-row");

        GtkWidget *dot = gtk_label_new("\xE2\x97\x8F");
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *primary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(primary), "nw-row-primary");
        gtk_widget_set_halign(primary, GTK_ALIGN_START);
        GtkWidget *secondary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(secondary), "nw-row-secondary");
        gtk_widget_set_halign(secondary, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(text_box), primary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), secondary, FALSE, FALSE, 0);
        GtkWidget *badge = gtk_label_new("");

        gtk_box_pack_start(GTK_BOX(hbox), dot, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), text_box, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), badge, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);

        refs = malloc(sizeof(PortRowRefs));
        refs->dot = dot;
        refs->primary = primary;
        refs->secondary = secondary;
        refs->badge = badge;
        g_object_set_data(G_OBJECT(row), "port", GINT_TO_POINTER(port->port));
        g_object_set_data_full(G_OBJECT(row), "row-refs", refs, free);

        gtk_list_box_insert(GTK_LIST_BOX(ports_list_box), row, -1);
        gtk_widget_show_all(row);
    } else {
        refs = g_object_get_data(G_OBJECT(row), "row-refs");
    }

    char primary_text[192];
    snprintf(primary_text, sizeof(primary_text), "Puerto %d - %s", port->port, port->service);
    gtk_label_set_text(GTK_LABEL(refs->primary), primary_text);

    char secondary_text[64];
    snprintf(secondary_text, sizeof(secondary_text), "puerto %d/tcp", port->port);
    gtk_label_set_text(GTK_LABEL(refs->secondary), secondary_text);

    gtk_label_set_text(GTK_LABEL(refs->badge), port->is_suspicious ? "SOSPECHOSO" : "Abierto");
    apply_port_status_classes(refs->dot, refs->badge, port->is_suspicious ? "critical" : "normal");

    if (port->is_suspicious) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "Puerto sospechoso: %d/tcp (%s)", port->port, port->service);
        gui_add_log_entry("PORT_SCANNER", "WARNING", log_msg);
    }
}

// ============================================================================
// ESCANEO (hilo real via scan_ports_range, mismo patron que gui_demo_scan)
// ============================================================================

static void on_port_scan_progress(const ProgressUpdate *update, void *ui_user_data) {
    guard_dial_set_progress((GtkWidget *)ui_user_data, update->current, update->total, update->phase);
}

typedef struct { ScanResult result; } PortScanDoneMessage;

static gboolean on_port_scan_done_idle(gpointer data) {
    pthread_join(port_scan_thread, NULL);
    PortScanDoneMessage *msg = (PortScanDoneMessage *)data;

    if (msg->result.ports) {
        for (int i = 0; i < msg->result.total_ports; i++) {
            if (msg->result.ports[i].is_open) {
                GUIPort gp;
                adapt_port_info_to_gui(&msg->result.ports[i], &gp);
                gui_update_port(&gp);
            }
        }
        aggregate_port_statistics(msg->result.ports, msg->result.total_ports,
                                   &port_last_total_open, &port_last_total_suspicious);
        free(msg->result.ports);
    }
    port_last_scan_time = time(NULL);

    char summary[128];
    if (port_cancel_flag) {
        snprintf(summary, sizeof(summary), "Cancelado (%d abiertos de %d)", msg->result.open_ports, msg->result.total_ports);
    } else {
        snprintf(summary, sizeof(summary), "Listo: %d abiertos de %d", msg->result.open_ports, msg->result.total_ports);
    }
    guard_dial_set_progress(ports_dial, msg->result.total_ports, msg->result.total_ports, summary);
    free(msg);

    gtk_widget_set_sensitive(ports_scan_button, TRUE);
    gtk_widget_set_sensitive(ports_quick_button, TRUE);
    gtk_widget_set_sensitive(ports_full_button, TRUE);
    gtk_widget_set_sensitive(ports_cancel_button, FALSE);
    port_scan_running = 0;

    return G_SOURCE_REMOVE;
}

static gboolean on_port_scan_start_failed_idle(gpointer data) {
    (void)data;
    pthread_join(port_scan_thread, NULL);
    gtk_widget_set_sensitive(ports_scan_button, TRUE);
    gtk_widget_set_sensitive(ports_quick_button, TRUE);
    gtk_widget_set_sensitive(ports_full_button, TRUE);
    gtk_widget_set_sensitive(ports_cancel_button, FALSE);
    guard_dial_set_progress(ports_dial, 0, 0, "No se pudo iniciar el escaneo");
    port_scan_running = 0;
    return G_SOURCE_REMOVE;
}

static void *port_scan_thread_main(void *arg) {
    (void)arg;
    GuiThreadBridgeTarget target = { .handler = on_port_scan_progress, .ui_user_data = ports_dial, .watch = G_OBJECT(ports_dial) };

    PortScanDoneMessage *msg = malloc(sizeof(PortScanDoneMessage));
    if (!msg) {
        g_idle_add(on_port_scan_start_failed_idle, NULL);
        return NULL;
    }

    int rc = scan_ports_range(port_scan_start_value, port_scan_end_value, PORT_SCAN_THREADS,
                               gui_thread_bridge_post, &target, &port_cancel_flag, &msg->result);
    if (rc != 0) {
        msg->result.ports = NULL;
        msg->result.total_ports = 0;
        msg->result.open_ports = 0;
        msg->result.suspicious_ports = 0;
    }

    g_idle_add(on_port_scan_done_idle, msg);
    return NULL;
}

static void clear_ports_list(void) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(ports_list_box));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

static void start_port_scan_with_range(int start, int end) {
    if (port_scan_running) {
        gui_add_log_entry("PORT_SCANNER", "WARNING", "Escaneo de puertos ya en progreso");
        return;
    }

    port_scan_start_value = start;
    port_scan_end_value = end;
    port_cancel_flag = 0;
    port_scan_running = 1;

    gtk_widget_set_sensitive(ports_scan_button, FALSE);
    gtk_widget_set_sensitive(ports_quick_button, FALSE);
    gtk_widget_set_sensitive(ports_full_button, FALSE);
    gtk_widget_set_sensitive(ports_cancel_button, TRUE);
    guard_dial_set_progress(ports_dial, 0, end - start + 1, "Iniciando escaneo de puertos...");
    clear_ports_list();

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Escaneando puertos %d-%d", start, end);
    gui_add_log_entry("PORT_SCANNER", "INFO", log_msg);

    if (pthread_create(&port_scan_thread, NULL, port_scan_thread_main, NULL) != 0) {
        gui_add_log_entry("PORT_SCANNER", "ERROR", "No se pudo crear el hilo de escaneo de puertos");
        port_scan_running = 0;
        gtk_widget_set_sensitive(ports_scan_button, TRUE);
        gtk_widget_set_sensitive(ports_quick_button, TRUE);
        gtk_widget_set_sensitive(ports_full_button, TRUE);
        gtk_widget_set_sensitive(ports_cancel_button, FALSE);
        guard_dial_set_progress(ports_dial, 0, 0, "No se pudo iniciar el escaneo");
    }
}

void on_scan_ports_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    int start = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(port_start_spin));
    int end = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(port_end_spin));
    start_port_scan_with_range(start, end);
}

static void on_quick_scan_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
    start_port_scan_with_range(1, 1024);
}

static void on_full_scan_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
        "El escaneo completo de 65535 puertos puede tomar mucho tiempo.\nEsta seguro de que desea continuar?");
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_YES) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 65535.0);
        start_port_scan_with_range(1, 65535);
    }
}

static void on_cancel_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    port_cancel_flag = 1;
}

// ============================================================================
// CICLO DE VIDA DEL PANEL
// ============================================================================

GtkWidget *gui_ports_panel_create(void) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new("Escaner de Puertos de Red");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new(""), TRUE, TRUE, 0);

    ports_cancel_button = gtk_button_new_with_label("Cancelar");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_cancel_button), "nw-button-secondary");
    gtk_widget_set_sensitive(ports_cancel_button, FALSE);
    g_signal_connect(ports_cancel_button, "clicked", G_CALLBACK(on_cancel_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), ports_cancel_button, FALSE, FALSE, 0);

    ports_scan_button = gtk_button_new_with_label("Escanear Rango");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_scan_button), "nw-button-primary");
    g_signal_connect(ports_scan_button, "clicked", G_CALLBACK(on_scan_ports_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), ports_scan_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), toolbar, FALSE, FALSE, 0);

    ports_dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(panel), ports_dial, FALSE, FALSE, 0);

    GtkWidget *range_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(range_row), gtk_label_new("Rango de puertos:"), FALSE, FALSE, 0);

    port_start_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
    gtk_box_pack_start(GTK_BOX(range_row), port_start_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(range_row), gtk_label_new("-"), FALSE, FALSE, 0);

    port_end_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
    gtk_box_pack_start(GTK_BOX(range_row), port_end_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(range_row), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);

    ports_quick_button = gtk_button_new_with_label("Escaneo Rapido (1-1024)");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_quick_button), "nw-button-secondary");
    g_signal_connect(ports_quick_button, "clicked", G_CALLBACK(on_quick_scan_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(range_row), ports_quick_button, FALSE, FALSE, 0);

    ports_full_button = gtk_button_new_with_label("Escaneo Completo (1-65535)");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_full_button), "nw-button-secondary");
    g_signal_connect(ports_full_button, "clicked", G_CALLBACK(on_full_scan_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(range_row), ports_full_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), range_row, FALSE, FALSE, 0);

    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    ports_list_box = gtk_list_box_new();
    g_signal_connect(ports_list_box, "row-selected", G_CALLBACK(on_port_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), ports_list_box);
    gtk_box_pack_start(GTK_BOX(content_box), scrolled, TRUE, TRUE, 0);

    ports_detail_label = gtk_label_new("Seleccione un puerto para ver detalles");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_detail_label), "nw-row-secondary");
    gtk_label_set_line_wrap(GTK_LABEL(ports_detail_label), TRUE);
    gtk_widget_set_size_request(ports_detail_label, 240, -1);
    gtk_widget_set_valign(ports_detail_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content_box), ports_detail_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), content_box, TRUE, TRUE, 0);

    return panel;
}

void gui_ports_panel_shutdown(void) {
    if (port_scan_running) {
        port_cancel_flag = 1;
        pthread_join(port_scan_thread, NULL);
        port_scan_running = 0;
    }
}

// ============================================================================
// COMPATIBILIDAD CON EL COORDINADOR Y EL MENU "ESCANEAR"
// ============================================================================

int is_port_scan_active(void) {
    return port_scan_running;
}

int get_port_statistics_for_gui(int *total_open, int *total_suspicious, time_t *last_scan_time) {
    if (!total_open || !total_suspicious || !last_scan_time) {
        return -1;
    }
    *total_open = port_last_total_open;
    *total_suspicious = port_last_total_suspicious;
    *last_scan_time = port_last_scan_time;
    return 0;
}

void gui_compatible_scan_ports(void) {
    if (port_scan_running) {
        gui_add_log_entry("PORT_SCANNER", "INFO", "Escaneo de puertos en progreso");
        return;
    }
    gui_add_log_entry("PORT_INTEGRATION", "INFO", "Iniciando escaneo rapido de puertos solicitado por usuario");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
    start_port_scan_with_range(1, 1024);
}
```

- [ ] **Step 3: Delete the old files**

```bash
rm src/gui/window/gui_ports_panel.c src/gui/integration/gui_ports_integration.c
```

- [ ] **Step 4: Update `gui_main.c`**

- Replace `#include "gui_ports_integration.h"` with `#include "gui_ports_panel.h"`.
- In `initialize_complete_backend_system()`, delete the `if (init_ports_integration() != 0) { ... }` block and its success log line (keep the `notify_module_status_change("ports", MODULE_STATUS_INACTIVE, "Listo para iniciar bajo demanda");` line).
- In `cleanup_complete_backend_system()`, delete the `cleanup_ports_integration();` call and its surrounding log lines.
- In `on_window_destroy`, add `gui_ports_panel_shutdown();` alongside the other two panel shutdown calls.
- In `init_gui`, after the process-page mounting block from Task 8, add:
```c
    GtkWidget *ports_page = gui_shell_get_page_container(3);
    if (ports_page != NULL) {
        gtk_box_pack_start(GTK_BOX(ports_page), gui_ports_panel_create(), TRUE, TRUE, 0);
    }
```

- [ ] **Step 5: Update the Makefile**

Change `src/gui/window/gui_ports_panel.c` to `src/gui/panels/gui_ports_panel.c`. Remove `src/gui/integration/gui_ports_integration.c` from `SRC`.

- [ ] **Step 6: Build**

Run: `make clean && make all 2>&1 | tail -40`
Expected: zero warnings.

- [ ] **Step 7: Manual visual verification (WSLg + screenshot)**

Launch `./matcom-guard`. On the Ports page: click "Escaneo Rapido" — confirm the dial animates through 1-1024 far faster than the old sequential scanner (real multi-threaded backend), and that open ports on `localhost`/the WSL VM (commonly 22, maybe others) appear as rows with correct service names. Click a row and confirm the detail panel shows its description. Try "Escaneo Completo" — confirm the warning dialog appears, and that "Cancelar" actually stops it partway through (dial shows "Cancelado").

- [ ] **Step 8: Commit**

```bash
git add include/gui_ports_panel.h src/gui/panels/gui_ports_panel.c src/gui/gui_main.c Makefile
git rm src/gui/window/gui_ports_panel.c src/gui/integration/gui_ports_integration.c
git commit -m "feat: add gui_ports_panel, switching to the real multi-threaded scan_ports_range"
```

---

## Task 10: Rewrite `gui_config_dialog.c` — move to `src/gui/`, restyle, fix a real memory leak

**Files:**
- Create: `include/gui_config_dialog.h`
- Create: `src/gui/gui_config_dialog.c` (replaces `src/gui/window/gui_config_dialog.c`)
- Delete: `src/gui/window/gui_config_dialog.c`
- Modify: `Makefile` (SRC path update)

**Interfaces:**
- Produces (unchanged names/signatures, already declared in the unmodified `gui_internal.h`, which is why `gui_main.c` needs no changes for this task): `void show_config_dialog(GtkWindow *parent)`, `gdouble get_cpu_threshold(void)`, `gdouble get_mem_threshold(void)`, `gint get_usb_scan_interval(void)`, `gint get_process_scan_interval(void)`, `gint get_port_scan_interval(void)`, `gboolean is_auto_scan_usb_enabled(void)`, `gboolean is_auto_scan_processes_enabled(void)`, `gboolean is_auto_scan_ports_enabled(void)`, `gboolean is_sound_alerts_enabled(void)`, `gboolean is_notifications_enabled(void)`.
- Consumes: nothing new. No threading — this is a modal form, matching the spec's own scoping ("no new threading concerns").
- **Scope note:** the old Apply handler never actually pushed the CPU/memory/interval values into the running USB/process backends (only saved them to `~/.config/matcom-guard/config.ini` and logged that it "notified" modules — it didn't). That gap is preserved as-is here; wiring live config pushes into the panels isn't required by the spec and isn't a correctness bug the way the cross-thread GTK issues were, so it's left alone to avoid opening new cross-task coupling with Tasks 7-9's already-finished headers.

- [ ] **Step 1: Write the header**

```c
// include/gui_config_dialog.h
#ifndef GUI_CONFIG_DIALOG_H
#define GUI_CONFIG_DIALOG_H

#include <gtk/gtk.h>

void show_config_dialog(GtkWindow *parent);

gdouble get_cpu_threshold(void);
gdouble get_mem_threshold(void);
gint get_usb_scan_interval(void);
gint get_process_scan_interval(void);
gint get_port_scan_interval(void);
gboolean is_auto_scan_usb_enabled(void);
gboolean is_auto_scan_processes_enabled(void);
gboolean is_auto_scan_ports_enabled(void);
gboolean is_sound_alerts_enabled(void);
gboolean is_notifications_enabled(void);

#endif // GUI_CONFIG_DIALOG_H
```

- [ ] **Step 2: Write the implementation**

Same 4-tab structure and defaults as the old file (Umbrales/Intervalos/Alertas/Lista Blanca, `GKeyFile`-backed persistence at `~/.config/matcom-guard/config.ini`), restyled with `nw-panel`/`nw-panel-title`, and fixing a real leak: `save_config_to_file`/`load_config_from_file` built `config_path` with `g_build_filename` (heap-allocated) into a `const char *` and never freed it.

```c
// src/gui/gui_config_dialog.c
#include "gui_config_dialog.h"
#include "gui_internal.h"
#include "gui.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    gdouble cpu_threshold;
    gdouble mem_threshold;
    gint usb_scan_interval;
    gint process_scan_interval;
    gint port_scan_interval;
    gboolean auto_scan_usb;
    gboolean auto_scan_processes;
    gboolean auto_scan_ports;
    gboolean enable_sound_alerts;
    gboolean enable_notifications;
    gboolean log_to_file;
    gint port_scan_start;
    gint port_scan_end;
    gchar *whitelist_processes;
} AppConfig;

static AppConfig config = {
    .cpu_threshold = 70.0,
    .mem_threshold = 50.0,
    .usb_scan_interval = 30,
    .process_scan_interval = 5,
    .port_scan_interval = 300,
    .auto_scan_usb = TRUE,
    .auto_scan_processes = TRUE,
    .auto_scan_ports = FALSE,
    .enable_sound_alerts = TRUE,
    .enable_notifications = TRUE,
    .log_to_file = TRUE,
    .port_scan_start = 1,
    .port_scan_end = 1024,
    .whitelist_processes = "firefox,chrome,systemd,gnome-shell"
};

static GtkWidget *config_dialog = NULL;
static GtkWidget *cpu_threshold_spin = NULL;
static GtkWidget *mem_threshold_spin = NULL;
static GtkWidget *usb_interval_spin = NULL;
static GtkWidget *process_interval_spin = NULL;
static GtkWidget *port_interval_spin = NULL;
static GtkWidget *auto_usb_check = NULL;
static GtkWidget *auto_process_check = NULL;
static GtkWidget *auto_port_check = NULL;
static GtkWidget *sound_alerts_check = NULL;
static GtkWidget *notifications_check = NULL;
static GtkWidget *log_file_check = NULL;
static GtkWidget *port_start_spin = NULL;
static GtkWidget *port_end_spin = NULL;
static GtkWidget *whitelist_entry = NULL;

static void save_config_to_file(void) {
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "matcom-guard", "config.ini", NULL);
    gchar *config_dir = g_path_get_dirname(config_path);
    g_mkdir_with_parents(config_dir, 0755);
    g_free(config_dir);

    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_double(keyfile, "Thresholds", "cpu_threshold", config.cpu_threshold);
    g_key_file_set_double(keyfile, "Thresholds", "mem_threshold", config.mem_threshold);
    g_key_file_set_integer(keyfile, "Intervals", "usb_scan_interval", config.usb_scan_interval);
    g_key_file_set_integer(keyfile, "Intervals", "process_scan_interval", config.process_scan_interval);
    g_key_file_set_integer(keyfile, "Intervals", "port_scan_interval", config.port_scan_interval);
    g_key_file_set_boolean(keyfile, "AutoScan", "usb", config.auto_scan_usb);
    g_key_file_set_boolean(keyfile, "AutoScan", "processes", config.auto_scan_processes);
    g_key_file_set_boolean(keyfile, "AutoScan", "ports", config.auto_scan_ports);
    g_key_file_set_boolean(keyfile, "Alerts", "sound", config.enable_sound_alerts);
    g_key_file_set_boolean(keyfile, "Alerts", "notifications", config.enable_notifications);
    g_key_file_set_boolean(keyfile, "Alerts", "log_to_file", config.log_to_file);
    g_key_file_set_integer(keyfile, "Ports", "scan_start", config.port_scan_start);
    g_key_file_set_integer(keyfile, "Ports", "scan_end", config.port_scan_end);
    g_key_file_set_string(keyfile, "Whitelist", "processes", config.whitelist_processes);

    GError *error = NULL;
    if (!g_key_file_save_to_file(keyfile, config_path, &error)) {
        gui_add_log_entry("CONFIG", "ERROR", error->message);
        g_error_free(error);
    } else {
        gui_add_log_entry("CONFIG", "INFO", "Configuracion guardada exitosamente");
    }

    g_key_file_free(keyfile);
    g_free(config_path);
}

static void load_config_from_file(void) {
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "matcom-guard", "config.ini", NULL);
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        g_error_free(error);
        g_key_file_free(keyfile);
        g_free(config_path);
        return; // Sin archivo previo: se mantienen los valores por defecto.
    }

    config.cpu_threshold = g_key_file_get_double(keyfile, "Thresholds", "cpu_threshold", NULL);
    config.mem_threshold = g_key_file_get_double(keyfile, "Thresholds", "mem_threshold", NULL);
    config.usb_scan_interval = g_key_file_get_integer(keyfile, "Intervals", "usb_scan_interval", NULL);
    config.process_scan_interval = g_key_file_get_integer(keyfile, "Intervals", "process_scan_interval", NULL);
    config.port_scan_interval = g_key_file_get_integer(keyfile, "Intervals", "port_scan_interval", NULL);
    config.auto_scan_usb = g_key_file_get_boolean(keyfile, "AutoScan", "usb", NULL);
    config.auto_scan_processes = g_key_file_get_boolean(keyfile, "AutoScan", "processes", NULL);
    config.auto_scan_ports = g_key_file_get_boolean(keyfile, "AutoScan", "ports", NULL);
    config.enable_sound_alerts = g_key_file_get_boolean(keyfile, "Alerts", "sound", NULL);
    config.enable_notifications = g_key_file_get_boolean(keyfile, "Alerts", "notifications", NULL);
    config.log_to_file = g_key_file_get_boolean(keyfile, "Alerts", "log_to_file", NULL);
    config.port_scan_start = g_key_file_get_integer(keyfile, "Ports", "scan_start", NULL);
    config.port_scan_end = g_key_file_get_integer(keyfile, "Ports", "scan_end", NULL);

    gchar *whitelist = g_key_file_get_string(keyfile, "Whitelist", "processes", NULL);
    if (whitelist) {
        g_free(config.whitelist_processes);
        config.whitelist_processes = whitelist;
    }

    g_key_file_free(keyfile);
    g_free(config_path);
    gui_add_log_entry("CONFIG", "INFO", "Configuracion cargada desde archivo");
}

static void on_config_apply_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;

    config.cpu_threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(cpu_threshold_spin));
    config.mem_threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(mem_threshold_spin));
    config.usb_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(usb_interval_spin));
    config.process_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(process_interval_spin));
    config.port_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_interval_spin));
    config.auto_scan_usb = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_usb_check));
    config.auto_scan_processes = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_process_check));
    config.auto_scan_ports = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_port_check));
    config.enable_sound_alerts = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sound_alerts_check));
    config.enable_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications_check));
    config.log_to_file = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(log_file_check));
    config.port_scan_start = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_start_spin));
    config.port_scan_end = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_end_spin));

    const gchar *whitelist = gtk_entry_get_text(GTK_ENTRY(whitelist_entry));
    g_free(config.whitelist_processes);
    config.whitelist_processes = g_strdup(whitelist);

    save_config_to_file();
    gui_add_log_entry("CONFIG", "INFO", "Configuracion aplicada exitosamente");
}

static void on_config_defaults_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(config_dialog), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Esta seguro de que desea restaurar los valores por defecto?");

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(cpu_threshold_spin), 70.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(mem_threshold_spin), 50.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(usb_interval_spin), 30.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(process_interval_spin), 5.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_interval_spin), 300.0);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_usb_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_process_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_port_check), FALSE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound_alerts_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(log_file_check), TRUE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
        gtk_entry_set_text(GTK_ENTRY(whitelist_entry), "firefox,chrome,systemd,gnome-shell");
        gui_add_log_entry("CONFIG", "INFO", "Valores por defecto restaurados");
    }

    gtk_widget_destroy(dialog);
}

static GtkWidget *page_title(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "nw-panel-title");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

static GtkWidget *create_thresholds_page(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "nw-panel");
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    gtk_grid_attach(GTK_GRID(grid), page_title("Umbrales de Alerta"), 0, 0, 2, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Umbral de CPU (%):"), 0, 1, 1, 1);
    cpu_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cpu_threshold_spin), config.cpu_threshold);
    gtk_grid_attach(GTK_GRID(grid), cpu_threshold_spin, 1, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Umbral de Memoria (%):"), 0, 2, 1, 1);
    mem_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mem_threshold_spin), config.mem_threshold);
    gtk_grid_attach(GTK_GRID(grid), mem_threshold_spin, 1, 2, 1, 1);

    return grid;
}

static GtkWidget *create_intervals_page(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "nw-panel");
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    gtk_grid_attach(GTK_GRID(grid), page_title("Intervalos de Escaneo Automatico"), 0, 0, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Modulo"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Intervalo (seg)"), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Activar"), 2, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Dispositivos USB:"), 0, 2, 1, 1);
    usb_interval_spin = gtk_spin_button_new_with_range(5.0, 3600.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(usb_interval_spin), config.usb_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), usb_interval_spin, 1, 2, 1, 1);
    auto_usb_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_usb_check), config.auto_scan_usb);
    gtk_grid_attach(GTK_GRID(grid), auto_usb_check, 2, 2, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Procesos:"), 0, 3, 1, 1);
    process_interval_spin = gtk_spin_button_new_with_range(1.0, 300.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(process_interval_spin), config.process_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), process_interval_spin, 1, 3, 1, 1);
    auto_process_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_process_check), config.auto_scan_processes);
    gtk_grid_attach(GTK_GRID(grid), auto_process_check, 2, 3, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Puertos de Red:"), 0, 4, 1, 1);
    port_interval_spin = gtk_spin_button_new_with_range(60.0, 7200.0, 60.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_interval_spin), config.port_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), port_interval_spin, 1, 4, 1, 1);
    auto_port_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_port_check), config.auto_scan_ports);
    gtk_grid_attach(GTK_GRID(grid), auto_port_check, 2, 4, 1, 1);

    return grid;
}

static GtkWidget *create_alerts_page(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(vbox), "nw-panel");

    gtk_box_pack_start(GTK_BOX(vbox), page_title("Configuracion de Alertas"), FALSE, FALSE, 0);

    sound_alerts_check = gtk_check_button_new_with_label("Activar alertas sonoras");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound_alerts_check), config.enable_sound_alerts);
    gtk_box_pack_start(GTK_BOX(vbox), sound_alerts_check, FALSE, FALSE, 0);

    notifications_check = gtk_check_button_new_with_label("Mostrar notificaciones del sistema");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check), config.enable_notifications);
    gtk_box_pack_start(GTK_BOX(vbox), notifications_check, FALSE, FALSE, 0);

    log_file_check = gtk_check_button_new_with_label("Guardar logs en archivo");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(log_file_check), config.log_to_file);
    gtk_box_pack_start(GTK_BOX(vbox), log_file_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(vbox), page_title("Rango de Escaneo de Puertos"), FALSE, FALSE, 0);

    GtkWidget *ports_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(ports_box), gtk_label_new("Desde:"), FALSE, FALSE, 0);
    port_start_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), config.port_scan_start);
    gtk_box_pack_start(GTK_BOX(ports_box), port_start_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ports_box), gtk_label_new("Hasta:"), FALSE, FALSE, 0);
    port_end_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), config.port_scan_end);
    gtk_box_pack_start(GTK_BOX(ports_box), port_end_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), ports_box, FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *create_whitelist_page(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(vbox), "nw-panel");

    gtk_box_pack_start(GTK_BOX(vbox), page_title("Lista Blanca de Procesos"), FALSE, FALSE, 0);

    GtkWidget *desc = gtk_label_new(
        "Los procesos en esta lista no generaran alertas aunque excedan los umbrales.\n"
        "Separe los nombres con comas (ejemplo: firefox,chrome,code)");
    gtk_label_set_line_wrap(GTK_LABEL(desc), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), desc, FALSE, FALSE, 0);

    whitelist_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(whitelist_entry), config.whitelist_processes);
    gtk_box_pack_start(GTK_BOX(vbox), whitelist_entry, FALSE, FALSE, 0);

    GtkWidget *common_list = gtk_label_new(
        "Procesos comunes que puede agregar:\n"
        "firefox, chrome, chromium - Navegadores web\n"
        "code, sublime_text, atom - Editores de codigo\n"
        "spotify, vlc, mpv - Reproductores multimedia\n"
        "discord, slack, teams - Aplicaciones de comunicacion\n"
        "gnome-shell, plasmashell - Entornos de escritorio\n"
        "systemd, init - Procesos del sistema");
    gtk_label_set_xalign(GTK_LABEL(common_list), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), common_list, FALSE, FALSE, 0);

    return vbox;
}

void show_config_dialog(GtkWindow *parent) {
    load_config_from_file();

    config_dialog = gtk_dialog_new_with_buttons("Configuracion de MatCom Guard", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancelar", GTK_RESPONSE_CANCEL,
        "_Valores por Defecto", GTK_RESPONSE_REJECT,
        "_Aplicar", GTK_RESPONSE_APPLY,
        "_Aceptar", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(config_dialog), 600, 500);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_thresholds_page(), gtk_label_new("Umbrales"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_intervals_page(), gtk_label_new("Intervalos"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_alerts_page(), gtk_label_new("Alertas"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_whitelist_page(), gtk_label_new("Lista Blanca"));

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(config_dialog));
    gtk_box_pack_start(GTK_BOX(content_area), notebook, TRUE, TRUE, 0);

    g_signal_connect(gtk_dialog_get_widget_for_response(GTK_DIALOG(config_dialog), GTK_RESPONSE_REJECT),
                     "clicked", G_CALLBACK(on_config_defaults_clicked), NULL);

    gtk_widget_show_all(config_dialog);

    gint response;
    do {
        response = gtk_dialog_run(GTK_DIALOG(config_dialog));
        if (response == GTK_RESPONSE_APPLY || response == GTK_RESPONSE_ACCEPT) {
            on_config_apply_clicked(NULL, NULL);
        }
    } while (response == GTK_RESPONSE_APPLY);

    gtk_widget_destroy(config_dialog);
    config_dialog = NULL;
}

gdouble get_cpu_threshold(void) { return config.cpu_threshold; }
gdouble get_mem_threshold(void) { return config.mem_threshold; }
gint get_usb_scan_interval(void) { return config.usb_scan_interval; }
gint get_process_scan_interval(void) { return config.process_scan_interval; }
gint get_port_scan_interval(void) { return config.port_scan_interval; }
gboolean is_auto_scan_usb_enabled(void) { return config.auto_scan_usb; }
gboolean is_auto_scan_processes_enabled(void) { return config.auto_scan_processes; }
gboolean is_auto_scan_ports_enabled(void) { return config.auto_scan_ports; }
gboolean is_sound_alerts_enabled(void) { return config.enable_sound_alerts; }
gboolean is_notifications_enabled(void) { return config.enable_notifications; }
```

- [ ] **Step 3: Delete the old file**

```bash
rm src/gui/window/gui_config_dialog.c
```

- [ ] **Step 4: Update the Makefile**

Change `src/gui/window/gui_config_dialog.c` to `src/gui/gui_config_dialog.c` in `SRC`.

- [ ] **Step 5: Build**

Run: `make clean && make all 2>&1 | tail -30`
Expected: zero warnings.

- [ ] **Step 6: Manual visual verification**

Launch `./matcom-guard`, click "Configuracion" in the header bar. Confirm all 4 tabs render with dark Night Watch backgrounds, "Valores por Defecto" prompts and resets correctly, "Aplicar" saves without closing the dialog, and "Aceptar" saves and closes. Inspect `~/.config/matcom-guard/config.ini` afterward to confirm it was written.

- [ ] **Step 7: Commit**

```bash
git add include/gui_config_dialog.h src/gui/gui_config_dialog.c Makefile
git rm src/gui/window/gui_config_dialog.c
git commit -m "refactor: rewrite gui_config_dialog.c at its new path, restyled, fix a config_path leak"
```

---

## Task 11: Rewrite `gui_system_coordinator.c` on `gui_periodic_worker`, delete the 3 old integration headers

**Correction versus the spec:** the *current* `gui_system_coordinator.c`'s own `coordinator_thread_function` calls `update_gui_with_global_state()` (which calls `gui_update_statistics`/`gui_update_system_status`/`gui_set_scanning_status`) **directly from its own background thread** — exactly the bug class the spec calls out. The rewrite moves all of that to the main thread: the periodic work function only touches mutex-protected state and calls `gui_add_log_entry` (already thread-safe), then posts a `ProgressUpdate` (phase = the security description, current/total = 0/0) through `gui_thread_bridge_post`; the bridge's handler runs on the main thread, re-reads the fresh state via the already mutex-protected `get_global_state_copy()`, and only then touches GTK widgets.

**Files:**
- Create: `src/gui/gui_system_coordinator.c` (replaces `src/gui/integration/gui_system_coordinator.c`)
- Modify: `include/gui_system_coordinator.h` (drop the two now-unnecessary threading fields, swap the 3 old integration-header includes for the 3 new panel headers, drop `update_gui_with_global_state`'s public declaration — nothing outside this file ever called it)
- Modify: `include/gui_usb_panel.h`, `include/gui_process_panel.h`, `include/gui_ports_panel.h` (add the "legacy-compat" declarations their `.c` files already define, so this file has prototypes to compile against)
- Delete: `src/gui/integration/gui_system_coordinator.c`, `include/gui_usb_integration.h`, `include/gui_process_integration.h`, `include/gui_ports_integration.h`
- Modify: `Makefile` (SRC path update)

**Interfaces:**
- Produces (unchanged names/signatures — `gui_main.c`'s calls to these need no changes): `init_system_coordinator`, `start_system_coordinator(int)`, `stop_system_coordinator(void)`, `cleanup_system_coordinator(void)`, `evaluate_system_security_level`, `update_aggregate_statistics`, `detect_cross_module_correlations`, `get_global_state_copy`, `get_current_security_level`, `get_consolidated_statistics`, `notify_security_event`, `notify_module_status_change`, `update_coordinator_configuration`, `request_immediate_system_evaluation`.
- Consumes: `gui_periodic_worker_start`/`_stop` (Task 1), `gui_thread_bridge_post`/`GuiThreadBridgeTarget` (Part A), and the compat functions this task adds to the 3 panel headers (`is_usb_monitoring_active`, `is_gui_usb_scan_in_progress`, `get_usb_statistics_for_gui`, `is_process_monitoring_active`, `get_process_statistics_for_gui`, `is_port_scan_active`, `get_port_statistics_for_gui` — all already *defined*, in Tasks 7-9, just not yet *declared* anywhere this file can see).

- [ ] **Step 1: Add the compat declarations to the 3 panel headers**

Add to `include/gui_usb_panel.h` (before the `#endif`):
```c
int is_usb_monitoring_active(void);
int is_gui_usb_scan_in_progress(void);
int get_usb_statistics_for_gui(int *total_devices, int *suspicious_devices, int *total_files, int *files_with_changes);
```

Add to `include/gui_process_panel.h` (before the `#endif`):
```c
int is_process_monitoring_active(void);
int get_process_statistics_for_gui(int *total_processes, int *high_cpu_count, int *high_memory_count, int *suspicious_count);
int sync_gui_with_backend_processes(void);
```

Add `#include <time.h>` to the top of `include/gui_ports_panel.h` (for `time_t`), then add before its `#endif`:
```c
int is_port_scan_active(void);
int get_port_statistics_for_gui(int *total_open, int *total_suspicious, time_t *last_scan_time);
```

- [ ] **Step 2: Rewrite the header**

```c
// include/gui_system_coordinator.h
#ifndef GUI_SYSTEM_COORDINATOR_H
#define GUI_SYSTEM_COORDINATOR_H

#include "gui.h"
#include "gui_usb_panel.h"
#include "gui_process_panel.h"
#include "gui_ports_panel.h"
#include <pthread.h>
#include <time.h>

typedef enum {
    SECURITY_LEVEL_SAFE,
    SECURITY_LEVEL_MONITORING,
    SECURITY_LEVEL_WARNING,
    SECURITY_LEVEL_CRITICAL,
    SECURITY_LEVEL_UNKNOWN
} SystemSecurityLevel;

typedef enum {
    MODULE_STATUS_INACTIVE,
    MODULE_STATUS_ACTIVE,
    MODULE_STATUS_SCANNING,
    MODULE_STATUS_ERROR,
    MODULE_STATUS_MAINTENANCE
} ModuleStatus;

typedef struct {
    SystemSecurityLevel security_level;
    time_t last_security_evaluation;
    char security_description[512];

    ModuleStatus process_module_status;
    ModuleStatus usb_module_status;
    ModuleStatus ports_module_status;

    struct {
        int total_processes_monitored;
        int suspicious_processes;
        int processes_exceeding_cpu_threshold;
        int processes_exceeding_memory_threshold;
        time_t last_process_scan;

        int total_usb_devices;
        int suspicious_usb_devices;
        int total_files_monitored;
        int files_with_changes;
        time_t last_usb_scan;

        int total_ports_scanned;
        int open_ports_found;
        int suspicious_ports;
        time_t last_port_scan;

        time_t statistics_last_updated;
    } aggregate_stats;

    struct {
        time_t system_start_time;
        int coordination_cycles_completed;
        int gui_updates_sent;
        float average_coordination_time_ms;
    } performance_metrics;

    pthread_mutex_t state_mutex;
} SystemGlobalState;

int init_system_coordinator(void);
int start_system_coordinator(int update_interval_seconds);
int stop_system_coordinator(void);
void cleanup_system_coordinator(void);

SystemSecurityLevel evaluate_system_security_level(void);
int update_aggregate_statistics(void);
int detect_cross_module_correlations(void);

int get_global_state_copy(SystemGlobalState *state_copy);
SystemSecurityLevel get_current_security_level(void);
int get_consolidated_statistics(int *total_devices, int *total_processes,
                               int *total_open_ports, int *security_alerts);

int notify_security_event(const char *source_module, int event_severity,
                         const char *event_description);
int notify_module_status_change(const char *module_name, ModuleStatus new_status,
                               const char *status_description);

int update_coordinator_configuration(int update_interval, int security_evaluation_sensitivity);
int request_immediate_system_evaluation(void);

#endif // GUI_SYSTEM_COORDINATOR_H
```

- [ ] **Step 3: Write the implementation**

```c
// src/gui/gui_system_coordinator.c
#include "gui_system_coordinator.h"
#include "gui_internal.h"
#include "gui_periodic_worker.h"
#include "gui_thread_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static SystemGlobalState global_state;
static GuiPeriodicWorker *coordinator_worker = NULL;
static GuiThreadBridgeTarget coordinator_bridge_target;
static int security_evaluation_sensitivity = 5;

// ============================================================================
// EVALUACION Y AGREGACION (pura, sin GTK -- se ejecuta en el hilo del worker)
// ============================================================================

SystemSecurityLevel evaluate_system_security_level(void) {
    int security_score = 0;
    int total_threats = 0;

    if (global_state.aggregate_stats.suspicious_processes > 0) {
        security_score += global_state.aggregate_stats.suspicious_processes * 15;
        total_threats += global_state.aggregate_stats.suspicious_processes;
    }
    if (global_state.aggregate_stats.suspicious_usb_devices > 0) {
        security_score += global_state.aggregate_stats.suspicious_usb_devices * 25;
        total_threats += global_state.aggregate_stats.suspicious_usb_devices;
    }
    if (global_state.aggregate_stats.suspicious_ports > 0) {
        security_score += global_state.aggregate_stats.suspicious_ports * 20;
        total_threats += global_state.aggregate_stats.suspicious_ports;
    }
    security_score += (global_state.aggregate_stats.processes_exceeding_cpu_threshold * 2);
    security_score += (global_state.aggregate_stats.processes_exceeding_memory_threshold * 2);

    if (global_state.process_module_status == MODULE_STATUS_ERROR) security_score += 10;
    if (global_state.usb_module_status == MODULE_STATUS_ERROR) security_score += 10;
    if (global_state.ports_module_status == MODULE_STATUS_ERROR) security_score += 10;

    security_score = (security_score * security_evaluation_sensitivity) / 5;

    SystemSecurityLevel level;
    char description[512];

    if (security_score == 0) {
        level = SECURITY_LEVEL_SAFE;
        snprintf(description, sizeof(description), "Sistema seguro - No se detectaron amenazas");
    } else if (security_score <= 10) {
        level = SECURITY_LEVEL_MONITORING;
        snprintf(description, sizeof(description), "Monitoreo activo - Actividad menor detectada (puntuacion: %d)", security_score);
    } else if (security_score <= 30) {
        level = SECURITY_LEVEL_WARNING;
        snprintf(description, sizeof(description), "Advertencia - %d amenaza(s) detectada(s) (puntuacion: %d)", total_threats, security_score);
    } else {
        level = SECURITY_LEVEL_CRITICAL;
        snprintf(description, sizeof(description), "CRITICO - Multiples amenazas activas detectadas (puntuacion: %d)", security_score);
    }

    strncpy(global_state.security_description, description, sizeof(global_state.security_description) - 1);
    global_state.security_description[sizeof(global_state.security_description) - 1] = '\0';

    return level;
}

int update_aggregate_statistics(void) {
    int success_count = 0;
    const int total_modules = 3;

    int processes_monitored = 0, high_cpu = 0, high_mem = 0, suspicious_processes = 0;
    if (get_process_statistics_for_gui(&processes_monitored, &high_cpu, &high_mem, &suspicious_processes) == 0) {
        global_state.aggregate_stats.total_processes_monitored = processes_monitored;
        global_state.aggregate_stats.processes_exceeding_cpu_threshold = high_cpu;
        global_state.aggregate_stats.processes_exceeding_memory_threshold = high_mem;
        global_state.aggregate_stats.suspicious_processes = suspicious_processes;
        global_state.aggregate_stats.last_process_scan = time(NULL);
        global_state.process_module_status = is_process_monitoring_active() ? MODULE_STATUS_ACTIVE : MODULE_STATUS_INACTIVE;
        success_count++;
    } else {
        global_state.process_module_status = MODULE_STATUS_ERROR;
    }

    int usb_devices = 0, suspicious_usb = 0, total_files = 0, files_with_changes = 0;
    if (get_usb_statistics_for_gui(&usb_devices, &suspicious_usb, &total_files, &files_with_changes) == 0) {
        global_state.aggregate_stats.total_usb_devices = usb_devices;
        global_state.aggregate_stats.suspicious_usb_devices = suspicious_usb;
        global_state.aggregate_stats.total_files_monitored = total_files;
        global_state.aggregate_stats.files_with_changes = files_with_changes;
        global_state.aggregate_stats.last_usb_scan = time(NULL);
        if (is_usb_monitoring_active()) {
            global_state.usb_module_status = is_gui_usb_scan_in_progress() ? MODULE_STATUS_SCANNING : MODULE_STATUS_ACTIVE;
        } else {
            global_state.usb_module_status = MODULE_STATUS_INACTIVE;
        }
        success_count++;
    } else {
        global_state.usb_module_status = MODULE_STATUS_ERROR;
    }

    int open_ports = 0, suspicious_ports = 0;
    time_t last_port_scan;
    if (get_port_statistics_for_gui(&open_ports, &suspicious_ports, &last_port_scan) == 0) {
        global_state.aggregate_stats.open_ports_found = open_ports;
        global_state.aggregate_stats.suspicious_ports = suspicious_ports;
        global_state.aggregate_stats.last_port_scan = last_port_scan;
        global_state.ports_module_status = is_port_scan_active() ? MODULE_STATUS_SCANNING : MODULE_STATUS_ACTIVE;
        success_count++;
    } else {
        global_state.ports_module_status = MODULE_STATUS_ERROR;
    }

    global_state.aggregate_stats.statistics_last_updated = time(NULL);

    if (success_count == total_modules) return 0;
    if (success_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Solo %d de %d modulos respondieron correctamente", success_count, total_modules);
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", msg);
        return 0;
    }
    gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Ningun modulo respondio - posible fallo del sistema");
    return -1;
}

int detect_cross_module_correlations(void) {
    int correlations_detected = 0;

    if (global_state.aggregate_stats.suspicious_processes > 0 &&
        global_state.aggregate_stats.suspicious_usb_devices > 0) {
        time_t diff = labs((long)(global_state.aggregate_stats.last_process_scan - global_state.aggregate_stats.last_usb_scan));
        if (diff <= 300) {
            gui_add_log_entry("CORRELATION_ANALYSIS", "ALERT", "Correlacion detectada: actividad sospechosa simultanea en procesos y USB");
            correlations_detected++;
        }
    }

    if (global_state.aggregate_stats.open_ports_found > 20 &&
        global_state.aggregate_stats.processes_exceeding_cpu_threshold > 5) {
        gui_add_log_entry("CORRELATION_ANALYSIS", "WARNING", "Correlacion detectada: alta actividad de red con uso intensivo de CPU");
        correlations_detected++;
    }

    if (global_state.aggregate_stats.suspicious_ports > 0 &&
        global_state.aggregate_stats.files_with_changes > 10) {
        gui_add_log_entry("CORRELATION_ANALYSIS", "ALERT", "Correlacion detectada: puertos sospechosos con modificacion masiva de archivos");
        correlations_detected++;
    }

    int modules_with_errors = 0;
    if (global_state.process_module_status == MODULE_STATUS_ERROR) modules_with_errors++;
    if (global_state.usb_module_status == MODULE_STATUS_ERROR) modules_with_errors++;
    if (global_state.ports_module_status == MODULE_STATUS_ERROR) modules_with_errors++;
    if (modules_with_errors >= 2) {
        gui_add_log_entry("CORRELATION_ANALYSIS", "CRITICAL", "Correlacion detectada: fallo multiple de modulos - posible ataque al sistema");
        correlations_detected++;
    }

    return correlations_detected;
}

// ============================================================================
// PUENTE HACIA LA GUI (el handler corre en el hilo principal de GTK)
// ============================================================================

static void on_coordinator_status_update(const ProgressUpdate *update, void *ui_user_data) {
    (void)ui_user_data;

    SystemGlobalState local_state;
    if (get_global_state_copy(&local_state) != 0) return;

    gui_update_statistics(local_state.aggregate_stats.total_usb_devices,
                           local_state.aggregate_stats.total_processes_monitored,
                           local_state.aggregate_stats.open_ports_found);

    gboolean system_healthy = (local_state.security_level <= SECURITY_LEVEL_MONITORING);
    gui_update_system_status(update->phase, system_healthy);

    if (local_state.security_level >= SECURITY_LEVEL_WARNING) {
        gboolean any_scanning = (local_state.process_module_status == MODULE_STATUS_SCANNING ||
                                  local_state.usb_module_status == MODULE_STATUS_SCANNING ||
                                  local_state.ports_module_status == MODULE_STATUS_SCANNING);
        gui_set_scanning_status(any_scanning);
    }

    pthread_mutex_lock(&global_state.state_mutex);
    global_state.performance_metrics.gui_updates_sent++;
    pthread_mutex_unlock(&global_state.state_mutex);
}

static void coordinator_work(volatile sig_atomic_t *cancel, void *user_data) {
    (void)cancel;
    (void)user_data;

    pthread_mutex_lock(&global_state.state_mutex);

    if (update_aggregate_statistics() != 0) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", "Error al actualizar estadisticas agregadas");
    }

    SystemSecurityLevel previous_level = global_state.security_level;
    global_state.security_level = evaluate_system_security_level();
    global_state.last_security_evaluation = time(NULL);

    if (previous_level != global_state.security_level) {
        const char *level_names[] = {"SAFE", "MONITORING", "WARNING", "CRITICAL", "UNKNOWN"};
        char msg[512];
        snprintf(msg, sizeof(msg), "Nivel de seguridad cambio de %s a %s",
                 level_names[previous_level], level_names[global_state.security_level]);
        gui_add_log_entry("SECURITY_EVALUATION",
                           global_state.security_level >= SECURITY_LEVEL_WARNING ? "ALERT" : "INFO", msg);
    }

    char description[512];
    strncpy(description, global_state.security_description, sizeof(description) - 1);
    description[sizeof(description) - 1] = '\0';

    global_state.performance_metrics.coordination_cycles_completed++;

    pthread_mutex_unlock(&global_state.state_mutex);

    int correlations = detect_cross_module_correlations();
    if (correlations > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Detectadas %d correlaciones de seguridad entre modulos", correlations);
        gui_add_log_entry("CORRELATION_ANALYSIS", "WARNING", msg);
    }

    ProgressUpdate update = { .phase = description, .current = 0, .total = 0 };
    gui_thread_bridge_post(&update, &coordinator_bridge_target);
}

// ============================================================================
// CICLO DE VIDA
// ============================================================================

int init_system_coordinator(void) {
    memset(&global_state, 0, sizeof(SystemGlobalState));
    global_state.security_level = SECURITY_LEVEL_UNKNOWN;
    strncpy(global_state.security_description, "Sistema iniciando - evaluacion pendiente",
            sizeof(global_state.security_description) - 1);
    global_state.process_module_status = MODULE_STATUS_INACTIVE;
    global_state.usb_module_status = MODULE_STATUS_INACTIVE;
    global_state.ports_module_status = MODULE_STATUS_INACTIVE;
    global_state.performance_metrics.system_start_time = time(NULL);

    if (pthread_mutex_init(&global_state.state_mutex, NULL) != 0) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Error al inicializar mutex del estado global");
        return -1;
    }

    coordinator_bridge_target.handler = on_coordinator_status_update;
    coordinator_bridge_target.ui_user_data = NULL;
    coordinator_bridge_target.watch = NULL; // no hay un widget unico que observar; el handler relee el estado global

    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Sistema coordinador inicializado exitosamente");
    return 0;
}

int start_system_coordinator(int update_interval_seconds_param) {
    if (coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", "Coordinador ya esta activo");
        return 0;
    }

    int interval = (update_interval_seconds_param >= 1 && update_interval_seconds_param <= 300)
                   ? update_interval_seconds_param : 5;

    pthread_mutex_lock(&global_state.state_mutex);
    update_aggregate_statistics();
    global_state.security_level = evaluate_system_security_level();
    global_state.last_security_evaluation = time(NULL);
    pthread_mutex_unlock(&global_state.state_mutex);

    coordinator_worker = gui_periodic_worker_start(interval, coordinator_work, NULL);
    if (!coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Error al iniciar el coordinador del sistema");
        return -1;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Coordinador del sistema iniciado (intervalo: %d segundos)", interval);
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", msg);
    return 0;
}

int stop_system_coordinator(void) {
    if (!coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Coordinador no esta activo");
        return 0;
    }
    gui_periodic_worker_stop(coordinator_worker); // señala + join, casi instantaneo
    coordinator_worker = NULL;
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Coordinador detenido exitosamente");
    return 0;
}

void cleanup_system_coordinator(void) {
    if (coordinator_worker) {
        stop_system_coordinator();
    }
    pthread_mutex_destroy(&global_state.state_mutex);
    memset(&global_state, 0, sizeof(SystemGlobalState));
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Recursos del coordinador liberados correctamente");
}

// ============================================================================
// ACCESO THREAD-SAFE AL ESTADO GLOBAL
// ============================================================================

int get_global_state_copy(SystemGlobalState *state_copy) {
    if (!state_copy) return -1;
    pthread_mutex_lock(&global_state.state_mutex);
    *state_copy = global_state;
    pthread_mutex_unlock(&global_state.state_mutex);
    return 0;
}

SystemSecurityLevel get_current_security_level(void) {
    pthread_mutex_lock(&global_state.state_mutex);
    SystemSecurityLevel level = global_state.security_level;
    pthread_mutex_unlock(&global_state.state_mutex);
    return level;
}

int get_consolidated_statistics(int *total_devices, int *total_processes,
                               int *total_open_ports, int *security_alerts) {
    if (!total_devices || !total_processes || !total_open_ports || !security_alerts) return -1;

    pthread_mutex_lock(&global_state.state_mutex);
    *total_devices = global_state.aggregate_stats.total_usb_devices;
    *total_processes = global_state.aggregate_stats.total_processes_monitored;
    *total_open_ports = global_state.aggregate_stats.open_ports_found;
    *security_alerts = global_state.aggregate_stats.suspicious_processes +
                      global_state.aggregate_stats.suspicious_usb_devices +
                      global_state.aggregate_stats.suspicious_ports;
    pthread_mutex_unlock(&global_state.state_mutex);
    return 0;
}

// ============================================================================
// NOTIFICACIONES ENTRE MODULOS
// ============================================================================

int notify_security_event(const char *source_module, int event_severity, const char *event_description) {
    if (!source_module || !event_description) return -1;
    if (event_severity < 1 || event_severity > 10) event_severity = 5;

    char msg[1024];
    snprintf(msg, sizeof(msg), "Evento de seguridad reportado por %s (severidad %d/10): %s",
             source_module, event_severity, event_description);
    gui_add_log_entry("SECURITY_EVENTS", event_severity >= 8 ? "ALERT" : event_severity >= 6 ? "WARNING" : "INFO", msg);

    if (event_severity >= 7) {
        request_immediate_system_evaluation();
    }
    return 0;
}

int notify_module_status_change(const char *module_name, ModuleStatus new_status, const char *status_description) {
    if (!module_name || !status_description) return -1;

    pthread_mutex_lock(&global_state.state_mutex);
    if (strcmp(module_name, "process") == 0) global_state.process_module_status = new_status;
    else if (strcmp(module_name, "usb") == 0) global_state.usb_module_status = new_status;
    else if (strcmp(module_name, "ports") == 0) global_state.ports_module_status = new_status;
    pthread_mutex_unlock(&global_state.state_mutex);

    const char *status_names[] = {"INACTIVE", "ACTIVE", "SCANNING", "ERROR", "MAINTENANCE"};
    char msg[512];
    snprintf(msg, sizeof(msg), "Modulo %s cambio estado a %s: %s", module_name, status_names[new_status], status_description);
    gui_add_log_entry("MODULE_STATUS", new_status == MODULE_STATUS_ERROR ? "ERROR" : "INFO", msg);
    return 0;
}

int update_coordinator_configuration(int update_interval, int security_evaluation_sensitivity_param) {
    if (update_interval < 1 || update_interval > 300) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Intervalo de actualizacion invalido (1-300 segundos)");
        return -1;
    }
    if (security_evaluation_sensitivity_param < 1 || security_evaluation_sensitivity_param > 10) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Sensibilidad de evaluacion invalida (1-10)");
        return -1;
    }

    security_evaluation_sensitivity = security_evaluation_sensitivity_param;

    if (coordinator_worker) {
        // Reiniciar con el nuevo intervalo: el periodic worker no soporta
        // cambiar su propio intervalo en caliente.
        gui_periodic_worker_stop(coordinator_worker);
        coordinator_worker = gui_periodic_worker_start(update_interval, coordinator_work, NULL);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Configuracion coordinador actualizada: intervalo=%ds, sensibilidad=%d/10",
             update_interval, security_evaluation_sensitivity_param);
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", msg);
    return 0;
}

int request_immediate_system_evaluation(void) {
    if (!coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", "No se puede evaluar: coordinador no esta activo");
        return -1;
    }
    // gui_periodic_worker no expone una senal de "despertar ya" -- una
    // evaluacion inmediata simplemente corre el mismo trabajo aca mismo,
    // en el hilo del llamador; coordinator_work() solo toca estado protegido
    // por mutex y hace un post seguro por el bridge, asi que es seguro
    // llamarla desde cualquier hilo, incluido el principal.
    coordinator_work(NULL, NULL);
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Evaluacion inmediata del sistema completada");
    return 0;
}
```

- [ ] **Step 4: Delete the old files**

```bash
rm src/gui/integration/gui_system_coordinator.c
rm include/gui_usb_integration.h include/gui_process_integration.h include/gui_ports_integration.h
```

- [ ] **Step 5: Update the Makefile**

Change `src/gui/integration/gui_system_coordinator.c` to `src/gui/gui_system_coordinator.c` in `SRC`.

- [ ] **Step 6: Build**

Run: `make clean && make all 2>&1 | tail -40`
Expected: zero warnings. This is the first point where the entire `src/gui/integration/` directory is empty — confirm with `ls src/gui/integration/` that nothing is left in it (an empty directory doesn't need to be removed from git, since git doesn't track empty directories).

- [ ] **Step 7: Manual visual verification (WSLg + screenshot)**

Launch `./matcom-guard`, wait ~10 seconds, confirm the Dashboard's "Estado General" label and stat cards update periodically without needing any manual scan (the coordinator's background evaluation), and that no GTK-thread-safety criticals appear in the terminal output (`Gtk-CRITICAL **: ... called from outside the GTK main thread` would indicate the bridge isn't being used correctly somewhere).

- [ ] **Step 8: Commit**

```bash
git add include/gui_system_coordinator.h include/gui_usb_panel.h include/gui_process_panel.h include/gui_ports_panel.h src/gui/gui_system_coordinator.c Makefile
git rm src/gui/integration/gui_system_coordinator.c include/gui_usb_integration.h include/gui_process_integration.h include/gui_ports_integration.h
git commit -m "refactor: rewrite gui_system_coordinator.c on gui_periodic_worker, close its cross-thread GTK gap"
```

---

## Task 12: Final `gui_main.c` cleanup, Makefile canonicalization, whole-app verification

**Correction versus the spec:** this closes out the finding surfaced during grounding — the spec's Architecture section describes `gui_main.c` as effectively untouched by Part B, but by this point it has been edited once per panel task (Tasks 5, 7, 8, 9 each trimmed one module's init/cleanup calls and added one mount + one shutdown call). This task shows the two backend-lifecycle functions and the header-bar menu handlers in their final, complete form so there's no ambiguity left from 4 tasks' worth of incremental diffs, and removes one more piece of dead code found during grounding: `on_scan_usb_menu_clicked`/`on_scan_processes_menu_clicked`/`on_scan_ports_menu_clicked`/`on_scan_all_clicked` each guard a `gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), N)` call with `if (notebook != NULL)` — but `notebook` (declared in the unchanged `gui_internal.h`) has been assigned nowhere since Part A replaced the tab-based layout with `gui_shell`'s `GtkStack`, so this branch has already been dead, unreachable code since Part A merged. It is not re-wired to `gui_shell` (out of scope — the spec authorizes exactly one `gui_shell.c` edit, the icon-rendering change in Task 3) — it is simply removed, since it never did anything.

**Files:**
- Modify: `src/gui/gui_main.c` (finalize `initialize_complete_backend_system`, `cleanup_complete_backend_system`, `intelligent_system_sync`; remove dead notebook-navigation code)
- Modify: `Makefile` (replace `SRC` wholesale with its final, canonical form)

**Interfaces:** none new — this task only cleans up call sites against APIs every earlier task already finished.

- [ ] **Step 1: Replace the two backend-lifecycle functions in `gui_main.c`**

Replace the full bodies of `initialize_complete_backend_system` and `cleanup_complete_backend_system` with:

```c
static int initialize_complete_backend_system(void) {
    gui_add_log_entry("STARTUP", "INFO", "=== INICIANDO SISTEMA BACKEND COMPLETO ===");

    if (init_system_coordinator() != 0) {
        gui_add_log_entry("STARTUP", "CRITICAL", "FALLO CRITICO: No se pudo inicializar coordinador del sistema");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "Coordinador del sistema inicializado");

    // USB, Procesos y Puertos ya se inicializaron cuando gui_usb_panel_create()/
    // gui_process_panel_create()/gui_ports_panel_create() se llamaron mas
    // arriba en init_gui() -- cada panel administra su propio ciclo de vida,
    // no hay un paso de "init" separado por modulo como antes.
    notify_module_status_change("process", MODULE_STATUS_INACTIVE, "Listo para iniciar bajo demanda");
    notify_module_status_change("ports", MODULE_STATUS_INACTIVE, "Listo para iniciar bajo demanda");

    if (start_system_coordinator(5) != 0) {
        gui_add_log_entry("STARTUP", "CRITICAL", "FALLO CRITICO: No se pudo iniciar coordinador del sistema");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "Coordinador del sistema iniciado");

    gui_add_log_entry("STARTUP", "INFO", "Realizando sincronizacion inicial del sistema...");
    sleep(2);
    request_immediate_system_evaluation();

    gui_add_log_entry("STARTUP", "INFO", "=== SISTEMA BACKEND COMPLETAMENTE OPERATIVO ===");
    return 0;
}

static void cleanup_complete_backend_system(void) {
    gui_add_log_entry("SHUTDOWN", "INFO", "=== INICIANDO LIMPIEZA COMPLETA DEL SISTEMA ===");
    gui_add_log_entry("SHUTDOWN", "INFO", "Deteniendo coordinador del sistema...");
    cleanup_system_coordinator();
    gui_add_log_entry("SHUTDOWN", "INFO", "Coordinador del sistema finalizado");
    gui_add_log_entry("SHUTDOWN", "INFO", "=== LIMPIEZA COMPLETA FINALIZADA ===");
}
```

- [ ] **Step 2: Trim `intelligent_system_sync`**

Replace its body with:

```c
static void intelligent_system_sync(void) {
    gui_add_log_entry("SYNC", "INFO", "Iniciando sincronizacion inteligente del sistema...");

    int total_devices, total_processes, total_ports, security_alerts;
    if (get_consolidated_statistics(&total_devices, &total_processes, &total_ports, &security_alerts) == 0) {
        gui_update_statistics(total_devices, total_processes, total_ports);

        char stats_msg[512];
        snprintf(stats_msg, sizeof(stats_msg),
                 "Estado sincronizado: %d dispositivos USB, %d procesos, %d puertos abiertos, %d alertas",
                 total_devices, total_processes, total_ports, security_alerts);
        gui_add_log_entry("SYNC", "INFO", stats_msg);

        if (security_alerts > 0) {
            SystemSecurityLevel level = get_current_security_level();
            if (level >= SECURITY_LEVEL_WARNING) {
                gui_update_system_status("Alertas de Seguridad Activas", FALSE);
            }
        }
    }

    // USB ya se mantiene sincronizado por su propio gui_periodic_worker
    // (con una primera invocacion inmediata al crear el panel en init_gui) --
    // no hace falta un empujon manual aca como en el codigo anterior.
    if (is_process_monitoring_active()) {
        sync_gui_with_backend_processes();
    }

    gui_add_log_entry("SYNC", "INFO", "Sincronizacion inteligente completada");
}
```

- [ ] **Step 3: Remove dead notebook-navigation code from the header-bar menu handlers**

In each of `on_scan_all_clicked`, `on_scan_usb_menu_clicked`, `on_scan_processes_menu_clicked`, `on_scan_ports_menu_clicked`, delete the `if (notebook != NULL) { gtk_notebook_set_current_page(...); }` block (and its preceding comment, where present). Each handler keeps only its log entry and its callback invocation. Example (`on_scan_usb_menu_clicked`, others follow the same shape):

```c
static void on_scan_usb_menu_clicked(GtkMenuItem *item __attribute__((unused)), gpointer data __attribute__((unused))) {
    if (usb_callback) {
        gui_add_log_entry("USB_SCANNER", "INFO", "Escaneo manual de USB iniciado desde menu");
        usb_callback();
    }
}
```

- [ ] **Step 4: Replace the Makefile's `SRC` block wholesale**

Across Tasks 1-11, `SRC` accumulated one small edit per task. Replace the entire block with its final, canonical form so there is no ambiguity left from the incremental diffs:

```makefile
SRC = src/main.c \
		src/port_scanner.c \
		src/threadpool.c \
		src/process_monitor.c \
		src/device_monitor.c \
		src/gui/widgets/guard_dial.c \
		src/gui/widgets/gui_icons.c \
		src/gui/gui_thread_bridge.c \
		src/gui/gui_periodic_worker.c \
		src/gui/gui_shell.c \
		src/gui/gui_main.c \
		src/gui/gui_backend_adapters.c \
		src/gui/gui_system_coordinator.c \
		src/gui/gui_config_dialog.c \
		src/gui/panels/gui_dashboard_panel.c \
		src/gui/panels/gui_logs_panel.c \
		src/gui/panels/gui_usb_panel.c \
		src/gui/panels/gui_process_panel.c \
		src/gui/panels/gui_ports_panel.c \
		src/gui/window/gui_status.c
```

`src/gui/window/gui_status.c` is the only survivor from the old `window/` directory — it was never in this spec's file-replacement table (the bottom status bar is untouched, out of scope for both Part A and Part B). Confirm `src/gui/window/` now contains only `gui_status.c` and `src/gui/integration/` is empty.

- [ ] **Step 5: Full clean build**

Run: `make clean && make all 2>&1 | tail -50`
Expected: zero warnings, zero errors.

- [ ] **Step 6: Full unit-test suite**

Run: `make test-unit 2>&1 | tail -60`
Expected: all 10 unit tests pass (`test_progress`, `test_threadpool`, `test_port_classification`, `test_port_scan_range`, `test_hash_skip`, `test_process_shutdown`, `test_guard_dial_math`, `test_gui_thread_bridge`, `test_gui_periodic_worker`, `test_gui_backend_adapters`).

- [ ] **Step 7: Whole-app manual verification (WSLg + screenshot)**

Launch `./matcom-guard` and walk through, in order:
1. Dashboard: stat cards populate within ~5-10s without any manual action (coordinator running).
2. USB page: "Actualizar" and "Escaneo Profundo" both work; rows show correctly.
3. Processes page: "Escanear Procesos" starts monitoring; kill a real test process and confirm it's gone from `ps`.
4. Ports page: quick scan and a cancelled full scan both work.
5. Logs page: entries from all of the above appear, correctly colored.
6. Header bar: "Configuracion" opens the 4-tab dialog, saves correctly; "Exportar" produces a valid `.txt` and `.pdf`; the "Escanear" dropdown menu's 4 items each still trigger the right panel's scan (even though they no longer switch pages, since that never actually worked before this rewrite either); the pause/resume toggle still updates the status bar.
7. Close the window: confirm the shutdown log sequence completes and the process exits cleanly (no hang, no crash) — this exercises the full shutdown order from Global Constraints (3 panel shutdowns, then coordinator cleanup).

- [ ] **Step 8: Commit**

```bash
git add src/gui/gui_main.c Makefile
git commit -m "chore: finalize gui_main.c backend lifecycle and Makefile SRC for the Part B rewrite"
```

---

## Self-Review

**Spec coverage:**
- Replace all 5 panels + paired integration GUI/threading logic → Tasks 5 (Dashboard), 6 (Logs), 7 (USB), 8 (Processes), 9 (Ports).
- Rewrite `gui_backend_adapters.c` → Task 4.
- Redesign `gui_system_coordinator.c` on a shared `gui_periodic_worker` → Tasks 1, 11.
- Rewrite `gui_config_dialog.c` → Task 10.
- Replace the 5 rail icons with Cairo-drawn icons → Tasks 2, 3.
- Delete `gui_demo_scan.*` → Task 5.
- Zero-warning, launchable at every commit → every task's own build step.
- Shared list-row pattern (dot/primary/secondary/badge) → Task 5 (CSS), used by Tasks 7/8/9.
- Scan-driven panels via `guard_dial`/`gui_thread_bridge_post`/real backend calls → Tasks 7, 8, 9.
- USB automatic monitoring preserved via `gui_periodic_worker` → Task 7.
- UTF-8-safe truncation → Task 4 (`utf8_safe_truncate`), used in Tasks 7/8.
- Per-panel `_shutdown()` + ordering invariant → Global Constraints, Tasks 7/8/9/11, wired together in Task 12.
- TDD unit-testable surface (`gui_backend_adapters.c`, `gui_periodic_worker` timing) → Tasks 1, 4. Manual-only surface (panel layouts, icons, config dialog, scan/cancel cycles) → Tasks 2, 3, 5-10, 12.
- 100% functional parity requirement (process kill+confirm, adjustable port ranges with presets, known-services descriptions, 4-tab persisted config, USB refresh-vs-deep-scan distinction) → preserved in Tasks 7, 8, 9, 10, with two deliberate, narrowly-scoped improvements called out explicitly where the old behavior was a genuine bug rather than a feature (Task 8's real `kill()`, Tasks 8/9/11's cross-thread GTK fixes) and one duplicate-logic elimination called out explicitly (Task 9's switch to the real `scan_ports_range`).

**Placeholder scan:** no "TBD"/"TODO" markers; every step carries literal code or an exact shell command; no step says "similar to Task N" without the code.

**Type consistency:** `GuiPeriodicWorkFn`/`GuiPeriodicWorker` (Task 1) used identically in Tasks 7 and 11. `GuiIconType`/`gui_icon_widget_new` (Task 2) used identically in Task 3. `utf8_safe_truncate` (Task 4) used identically in Tasks 7 and 8. The four legacy-named compat functions per panel (Tasks 7/8/9) match the exact signatures `gui_system_coordinator.c` (Task 11) calls them with. `GuiThreadBridgeTarget`/`gui_thread_bridge_post` (Part A) used with matching field names across Tasks 7, 8, 9, 11.

**Pre-flight conflict scan:**

| File | Touched by | Notes |
|---|---|---|
| `src/gui/gui_main.c` | Tasks 5, 7, 8, 9, 12 | Each task's edit is additive/localized (one include swap, one init/cleanup trim, one mount, one shutdown call); Task 12 shows the final state of the two lifecycle functions to remove ambiguity. |
| `Makefile` | Tasks 1, 2, 3(none), 4, 5, 6, 7, 8, 9, 10, 11, 12 | Task 12 replaces `SRC` wholesale as the final canonicalization step. |
| `resources/style/night-watch.css` | Task 5 only | One append; no later task edits it. |
| `include/gui_usb_panel.h` | Tasks 7, 11 | Task 11 only adds declarations, doesn't touch Task 7's. |
| `include/gui_process_panel.h` | Tasks 8, 11 | Same shape as above. |
| `include/gui_ports_panel.h` | Tasks 9, 11 | Same shape as above. |

No two tasks edit the same lines of the same file without one being a later, self-contained continuation already called out above — safe to execute in the written order.

---

**Plan complete and saved to `docs/superpowers/plans/2026-09-13-frontend-panels.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
