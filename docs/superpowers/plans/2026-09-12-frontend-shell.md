# Frontend Shell Implementation Plan (Part A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the new app shell (icon-rail navigation, status/badge bar, Cairo guard-dial widget, GLib thread-safety bridge) around the existing GTK+3 app, with the 5 content pages as placeholders except a real end-to-end demo (a live `127.0.0.1:1-100` port scan with progress and cancellation) on the Dashboard page.

**Architecture:** Four new self-contained GUI-side modules (`guard_dial`, `gui_thread_bridge`, `gui_shell`, `gui_demo_scan`) plus one new CSS stylesheet, wired into the existing `gui_main.c` entry point in place of the current `GtkNotebook` tabs. No backend file changes — the demo calls the already-merged `scan_ports_range` unmodified. The 10 old panel/integration files stay compiled but unmounted until a future Part B replaces them one at a time.

**Tech Stack:** C99, GTK+3, Cairo, Pango (via GTK's own pkg-config flags), GLib (`g_idle_add`, `g_main_context_iteration`, `GObject` weak pointers), pthreads.

**Spec:** `docs/superpowers/specs/2026-09-12-frontend-shell-design.md`

**Correction versus the spec:** the spec's Architecture section shows `gui_shell_create(GtkApplication *app)`. The existing codebase does not use `GtkApplication` — `gui_main.c` builds a plain `GtkWindow` and runs `gtk_main()`/`gtk_main_quit()` directly. Introducing `GtkApplication` would be a bigger change than this plan's scope, so `gui_shell_create(void)` instead returns a `GtkWidget*` (a container) that `gui_main.c` packs into its existing `main_container`, leaving the window/event-loop bootstrap untouched. Everything else in the spec (the icon-rail, the stack, `gui_shell_get_page_container`, the dial, the bridge, the demo) is unchanged.

## Global Constraints

- C99, built with `-Wall -Wextra -std=c99 -g -Iinclude -DDEBUG` — every new/changed file must compile with **zero warnings** under this exact flag set.
- The only pre-existing files this plan may modify are `src/gui/gui_main.c` and `Makefile`. No file among the 5 old panels (`gui_usb_panel.c`, `gui_process_panel.c`, `gui_ports_panel.c`, `gui_config_dialog.c`, `gui_stats.c`, `gui_status.c`, `gui_logging.c`) or 5 old integrations (`gui_usb_integration.c`, `gui_process_integration.c`, `gui_ports_integration.c`, `gui_backend_adapters.c`, `gui_system_coordinator.c`) may be touched.
- No backend file may be modified: nothing under `src/*.c` outside `src/gui/`, and no header outside `include/gui*.h`/the 4 new headers this plan creates. This plan only *calls* one existing, unmodified backend function (`scan_ports_range`, declared in `include/port_scanner.h`).
- Every `malloc`/`realloc` return value is checked before use.
- A GTK widget is only ever touched from the main thread. Any backend worker thread that needs to update one does so through `gui_thread_bridge_post`, never directly.
- All work happens on branch `rewrite/frontend`, created from the current tip of `main` — never commit this work directly to `main`.

---

## Setup (do this once, before Task 1)

```bash
cd /path/to/Matcom-Guard
git checkout main
git pull
git checkout -b rewrite/frontend
mkdir -p src/gui/widgets resources/style
```

All tasks below assume you're on `rewrite/frontend`. All manual verification (building, running the app or a throwaway preview program) happens under WSL/Linux — from a Windows shell that's `cd /mnt/c/.../Matcom-Guard && make ...` inside `wsl -d <distro> -- bash -c "..."`, or directly if you're already on Linux. Building and running the real GUI needs a display; under WSL that means WSLg is available and, for screenshot automation, `GDK_BACKEND=x11 ./matcom-guard`.

---

### Task 1: Guard dial — pure progress math

**Files:**
- Create: `include/guard_dial.h`
- Create: `src/gui/widgets/guard_dial.c`
- Test: `tests/unit/test_guard_dial_math.c`

**Interfaces:**
- Produces: `double guard_dial_compute_arc_fraction(int current, int total);` — Task 2 (the same file) uses this to compute the drawn arc's length; nothing outside this file needs it yet.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_guard_dial_math.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "guard_dial.h"

int main(void) {
    // total == 0 significa progreso indeterminado: sin fracción que mostrar.
    assert(guard_dial_compute_arc_fraction(5, 0) == 0.0);
    assert(guard_dial_compute_arc_fraction(0, 0) == 0.0);

    // Casos normales.
    assert(guard_dial_compute_arc_fraction(0, 100) == 0.0);
    assert(guard_dial_compute_arc_fraction(50, 100) == 0.5);
    assert(guard_dial_compute_arc_fraction(100, 100) == 1.0);

    // Saturación: nunca se devuelve una fracción fuera de [0,1].
    assert(guard_dial_compute_arc_fraction(150, 100) == 1.0);
    assert(guard_dial_compute_arc_fraction(-10, 100) == 0.0);

    printf("test_guard_dial_math: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_guard_dial_math tests/unit/test_guard_dial_math.c` (no `.c` implementation exists yet, so this must fail to even compile/link — that's the expected RED state).
Expected: FAIL — `fatal error: guard_dial.h: No such file or directory` (or, once the header exists but not the .c, an undefined-reference linker error).

- [ ] **Step 3: Write minimal implementation**

Create `include/guard_dial.h`:

```c
#ifndef GUARD_DIAL_H
#define GUARD_DIAL_H

/**
 * Fracción de arco (0.0 a 1.0) que representa el progreso current/total,
 * usada para dibujar el dial de guardia.
 *
 * `total <= 0` (progreso indeterminado o inválido) devuelve 0.0.
 * `current` se satura al rango [0, total] antes de calcular la fracción,
 * así que el valor devuelto siempre está en [0.0, 1.0].
 */
double guard_dial_compute_arc_fraction(int current, int total);

#endif // GUARD_DIAL_H
```

Create `src/gui/widgets/guard_dial.c`:

```c
#include "guard_dial.h"

double guard_dial_compute_arc_fraction(int current, int total) {
    if (total <= 0) {
        return 0.0;
    }
    if (current <= 0) {
        return 0.0;
    }
    if (current >= total) {
        return 1.0;
    }
    return (double)current / (double)total;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_guard_dial_math tests/unit/test_guard_dial_math.c src/gui/widgets/guard_dial.c && ./tests/unit/test_guard_dial_math`
Expected: PASS — `test_guard_dial_math: OK`, zero compiler warnings.

- [ ] **Step 5: Commit**

```bash
git add include/guard_dial.h src/gui/widgets/guard_dial.c tests/unit/test_guard_dial_math.c
git commit -m "feat: add guard dial progress-to-arc-fraction math"
```

---

### Task 2: Guard dial — the GTK widget

**Files:**
- Modify: `include/guard_dial.h` (extend)
- Modify: `src/gui/widgets/guard_dial.c` (extend)

**Interfaces:**
- Consumes: `guard_dial_compute_arc_fraction` (Task 1, same file)
- Produces: `GtkWidget *guard_dial_new(void);` and `void guard_dial_set_progress(GtkWidget *dial, int current, int total, const char *phase);` — Task 5 (the demo) and Task 6 (wiring) use both.

- [ ] **Step 1: Extend the header**

In `include/guard_dial.h`, add `#include <gtk/gtk.h>` right after the include guard, and add these two declarations before the closing `#endif`:

```c
/** Crea un dial de guardia nuevo, listo para agregar a un contenedor. */
GtkWidget *guard_dial_new(void);

/**
 * Actualiza el progreso mostrado y fuerza un redibujado. `phase` se copia
 * internamente a un buffer propio — el puntero que se pasa no necesita
 * seguir siendo válido después de que esta función retorna. `phase` puede
 * ser NULL (se trata como cadena vacía).
 */
void guard_dial_set_progress(GtkWidget *dial, int current, int total, const char *phase);
```

The full file should now read:

```c
#ifndef GUARD_DIAL_H
#define GUARD_DIAL_H

#include <gtk/gtk.h>

/**
 * Fracción de arco (0.0 a 1.0) que representa el progreso current/total,
 * usada para dibujar el dial de guardia.
 *
 * `total <= 0` (progreso indeterminado o inválido) devuelve 0.0.
 * `current` se satura al rango [0, total] antes de calcular la fracción,
 * así que el valor devuelto siempre está en [0.0, 1.0].
 */
double guard_dial_compute_arc_fraction(int current, int total);

/** Crea un dial de guardia nuevo, listo para agregar a un contenedor. */
GtkWidget *guard_dial_new(void);

/**
 * Actualiza el progreso mostrado y fuerza un redibujado. `phase` se copia
 * internamente a un buffer propio — el puntero que se pasa no necesita
 * seguir siendo válido después de que esta función retorna. `phase` puede
 * ser NULL (se trata como cadena vacía).
 */
void guard_dial_set_progress(GtkWidget *dial, int current, int total, const char *phase);

#endif // GUARD_DIAL_H
```

- [ ] **Step 2: Extend the implementation**

Append to `src/gui/widgets/guard_dial.c` (keep `guard_dial_compute_arc_fraction` from Task 1 as-is):

```c
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_DIAL_PHASE_BUFFER 128
#define GUARD_DIAL_STATE_KEY "guard-dial-state"

typedef struct {
    int current;
    int total;
    char phase[GUARD_DIAL_PHASE_BUFFER];
} GuardDialState;

static void guard_dial_state_free(gpointer data) {
    free(data);
}

static gboolean guard_dial_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)user_data;
    GuardDialState *state = g_object_get_data(G_OBJECT(widget), GUARD_DIAL_STATE_KEY);

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    double cx = allocation.width / 2.0;
    double cy = allocation.height / 2.0;
    double smaller_side = allocation.width < allocation.height ? allocation.width : allocation.height;
    double radius = smaller_side / 2.0 - 12.0;
    if (radius < 1.0) {
        radius = 1.0;
    }

    // Anillo de fondo.
    cairo_set_source_rgb(cr, 0x2A / 255.0, 0x2E / 255.0, 0x3D / 255.0);
    cairo_set_line_width(cr, 10);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_stroke(cr);

    if (state != NULL) {
        double fraction = guard_dial_compute_arc_fraction(state->current, state->total);
        double start_angle = -G_PI / 2.0;
        double end_angle = start_angle + fraction * 2 * G_PI;

        if (fraction > 0.0) {
            cairo_set_source_rgb(cr, 0xD9 / 255.0, 0x8E / 255.0, 0x3F / 255.0);
            cairo_set_line_width(cr, 10);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_arc(cr, cx, cy, radius, start_angle, end_angle);
            cairo_stroke(cr);
        }
    }

    // Anillo interior punteado, puramente decorativo (aspecto de instrumento).
    cairo_set_source_rgb(cr, 0x23 / 255.0, 0x28 / 255.0, 0x38 / 255.0);
    cairo_set_line_width(cr, 1);
    double dashes[] = {2.0, 4.0};
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_arc(cr, cx, cy, radius - 16 > 1.0 ? radius - 16 : 1.0, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);

    if (state != NULL) {
        char percent_text[16];
        int percent = (state->total > 0)
            ? (int)(guard_dial_compute_arc_fraction(state->current, state->total) * 100)
            : 0;
        snprintf(percent_text, sizeof(percent_text), "%d%%", percent);

        int text_w, text_h;

        PangoLayout *percent_layout = pango_cairo_create_layout(cr);
        PangoFontDescription *percent_font = pango_font_description_from_string("Georgia Bold 16");
        pango_layout_set_font_description(percent_layout, percent_font);
        pango_layout_set_text(percent_layout, percent_text, -1);
        pango_layout_get_pixel_size(percent_layout, &text_w, &text_h);
        cairo_set_source_rgb(cr, 0xE8 / 255.0, 0xE6 / 255.0, 0xDE / 255.0);
        cairo_move_to(cr, cx - text_w / 2.0, cy - text_h - 2);
        pango_cairo_show_layout(cr, percent_layout);
        pango_font_description_free(percent_font);
        g_object_unref(percent_layout);

        PangoLayout *phase_layout = pango_cairo_create_layout(cr);
        PangoFontDescription *phase_font = pango_font_description_from_string("Monospace 8");
        pango_layout_set_font_description(phase_layout, phase_font);
        pango_layout_set_text(phase_layout, state->phase, -1);
        pango_layout_get_pixel_size(phase_layout, &text_w, &text_h);
        cairo_set_source_rgb(cr, 0x9a / 255.0, 0xa1 / 255.0, 0xad / 255.0);
        cairo_move_to(cr, cx - text_w / 2.0, cy + 4);
        pango_cairo_show_layout(cr, phase_layout);
        pango_font_description_free(phase_font);
        g_object_unref(phase_layout);
    }

    return FALSE;
}

GtkWidget *guard_dial_new(void) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 140, 140);

    GuardDialState *state = malloc(sizeof(GuardDialState));
    if (state == NULL) {
        fprintf(stderr, "[guard_dial] No se pudo asignar memoria para el estado del dial\n");
    } else {
        state->current = 0;
        state->total = 0;
        state->phase[0] = '\0';
        g_object_set_data_full(G_OBJECT(area), GUARD_DIAL_STATE_KEY, state, guard_dial_state_free);
    }

    g_signal_connect(area, "draw", G_CALLBACK(guard_dial_draw), NULL);
    return area;
}

void guard_dial_set_progress(GtkWidget *dial, int current, int total, const char *phase) {
    GuardDialState *state = g_object_get_data(G_OBJECT(dial), GUARD_DIAL_STATE_KEY);
    if (state == NULL) {
        return;
    }

    state->current = current;
    state->total = total;
    if (phase != NULL) {
        strncpy(state->phase, phase, sizeof(state->phase) - 1);
        state->phase[sizeof(state->phase) - 1] = '\0';
    } else {
        state->phase[0] = '\0';
    }

    gtk_widget_queue_draw(dial);
}
```

- [ ] **Step 3: Confirm the math test still passes and the file compiles/links clean**

`guard_dial.c` now calls real GTK/Cairo/Pango functions inside `guard_dial_new`/`guard_dial_draw`, so — even though the test itself never calls those two functions — linking `guard_dial.c`'s object code into the test binary requires the GTK pkg-config flags from this point on (GTK's own pkg-config data pulls in Cairo and Pango transitively, the same way the project's main `Makefile` only lists `GTK_FLAGS` explicitly and still gets Pango for free):

Run: `gcc -Wall -Wextra -std=c99 -Iinclude `pkg-config --cflags --libs gtk+-3.0` -o tests/unit/test_guard_dial_math tests/unit/test_guard_dial_math.c src/gui/widgets/guard_dial.c && ./tests/unit/test_guard_dial_math`
Expected: PASS, `test_guard_dial_math: OK`, zero compiler warnings.

- [ ] **Step 4: Manual visual verification (not committed)**

GTK widget rendering has no automated test story without a display (see the spec's Testing Strategy). Write a throwaway preview program, run it once under WSLg, confirm it looks right, then discard it — do not commit this file.

Save as `/tmp/guard_dial_preview.c`:

```c
#include <gtk/gtk.h>
#include "guard_dial.h"

static gboolean animate(gpointer data) {
    static int current = 0;
    GtkWidget *dial = GTK_WIDGET(data);
    current = (current + 5) % 105;
    guard_dial_set_progress(dial, current, 100, "Vista previa del dial");
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 200, 200);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *dial = guard_dial_new();
    gtk_container_add(GTK_CONTAINER(window), dial);
    g_timeout_add(200, animate, dial);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
```

Run (from WSL, with a display available):
```bash
gcc -Wall -Wextra -std=c99 -Iinclude `pkg-config --cflags --libs gtk+-3.0` -o /tmp/guard_dial_preview /tmp/guard_dial_preview.c src/gui/widgets/guard_dial.c
GDK_BACKEND=x11 /tmp/guard_dial_preview
```
Expected: a small window shows the dial, background ring, dashed inner ring, and an amber progress arc that grows and wraps around every ~4 seconds, with a percentage and "Vista previa del dial" text in the center. Close the window to exit. Delete `/tmp/guard_dial_preview.c` and `/tmp/guard_dial_preview` when done.

- [ ] **Step 5: Commit**

```bash
git add include/guard_dial.h src/gui/widgets/guard_dial.c
git commit -m "feat: add guard dial GTK widget (Cairo/Pango rendering)"
```

---

### Task 3: Thread-safety bridge

**Files:**
- Create: `include/gui_thread_bridge.h`
- Create: `src/gui/gui_thread_bridge.c`
- Test: `tests/unit/test_gui_thread_bridge.c`

**Interfaces:**
- Consumes: `ProgressUpdate` (`include/progress.h`, already in the merged backend)
- Produces: `GuiThreadBridgeTarget` struct, `GuiUiProgressHandler` typedef, and `void gui_thread_bridge_post(const ProgressUpdate *update, void *user_data);` — matches `ProgressCallback`'s exact signature. Task 5 (the demo) passes this function directly as `cb` to `scan_ports_range`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_gui_thread_bridge.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include "gui_thread_bridge.h"

static int handler_calls = 0;
static char last_phase[128];
static int last_current = -1;
static int last_total = -1;

static void record_handler(const ProgressUpdate *update, void *ui_user_data) {
    int *counter = (int *)ui_user_data;
    (*counter)++;
    handler_calls++;
    strncpy(last_phase, update->phase, sizeof(last_phase) - 1);
    last_phase[sizeof(last_phase) - 1] = '\0';
    last_current = update->current;
    last_total = update->total;
}

static void drain_idle_queue(void) {
    while (g_main_context_iteration(NULL, FALSE)) {
        // sigue procesando mientras haya trabajo pendiente en el contexto por defecto
    }
}

static void test_delivers_a_copy_on_the_main_context(void) {
    handler_calls = 0;
    int ui_counter = 0;
    GuiThreadBridgeTarget target = { .handler = record_handler, .ui_user_data = &ui_counter, .watch = NULL };

    char phase_buf[64];
    strcpy(phase_buf, "Escaneando puertos");
    ProgressUpdate update = { .phase = phase_buf, .current = 42, .total = 100 };

    gui_thread_bridge_post(&update, &target);

    // gui_thread_bridge_post solo encola el trabajo -- todavía no llamó al handler.
    assert(handler_calls == 0);

    // Sobrescribir el buffer original ANTES de procesar la cola, para probar
    // que el puente ya copió el string (no solo guardó el puntero).
    strcpy(phase_buf, "BASURA");

    drain_idle_queue();

    assert(handler_calls == 1);
    assert(ui_counter == 1);
    assert(strcmp(last_phase, "Escaneando puertos") == 0);
    assert(last_current == 42);
    assert(last_total == 100);

    printf("test_gui_thread_bridge: entrega con copia -> OK\n");
}

static void test_skips_handler_if_watch_was_destroyed(void) {
    handler_calls = 0;
    int ui_counter = 0;
    GObject *watched = g_object_new(G_TYPE_OBJECT, NULL);
    GuiThreadBridgeTarget target = { .handler = record_handler, .ui_user_data = &ui_counter, .watch = watched };

    ProgressUpdate update = { .phase = "no debería llegar", .current = 1, .total = 2 };
    gui_thread_bridge_post(&update, &target);

    // Finalizar el objeto observado ANTES de procesar la cola.
    g_object_unref(watched);

    drain_idle_queue();

    assert(handler_calls == 0);
    printf("test_gui_thread_bridge: se salta el handler si watch fue destruido -> OK\n");
}

static void test_delivers_normally_when_watch_survives(void) {
    handler_calls = 0;
    int ui_counter = 0;
    GObject *watched = g_object_new(G_TYPE_OBJECT, NULL);
    GuiThreadBridgeTarget target = { .handler = record_handler, .ui_user_data = &ui_counter, .watch = watched };

    ProgressUpdate update = { .phase = "todo bien", .current = 3, .total = 4 };
    gui_thread_bridge_post(&update, &target);

    drain_idle_queue();

    assert(handler_calls == 1);
    g_object_unref(watched);
    printf("test_gui_thread_bridge: entrega normal con watch vivo -> OK\n");
}

int main(void) {
    test_delivers_a_copy_on_the_main_context();
    test_skips_handler_if_watch_was_destroyed();
    test_delivers_normally_when_watch_survives();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude `pkg-config --cflags --libs gobject-2.0` -o tests/unit/test_gui_thread_bridge tests/unit/test_gui_thread_bridge.c`
Expected: FAIL — `fatal error: gui_thread_bridge.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `include/gui_thread_bridge.h`:

```c
#ifndef GUI_THREAD_BRIDGE_H
#define GUI_THREAD_BRIDGE_H

#include <glib-object.h>
#include "progress.h"

/** Firma del handler que corre en el hilo principal de GTK. */
typedef void (*GuiUiProgressHandler)(const ProgressUpdate *update, void *ui_user_data);

/**
 * `watch` es opcional (puede ser NULL). Si no es NULL, se rastrea con un
 * weak pointer de GObject: si el objeto es destruido antes de que la
 * actualización encolada llegue al hilo principal, el handler se salta en
 * vez de tocar un widget que ya no existe.
 */
typedef struct {
    GuiUiProgressHandler handler;
    void *ui_user_data;
    GObject *watch;
} GuiThreadBridgeTarget;

/**
 * Coincide exactamente con la firma de ProgressCallback (include/progress.h)
 * -- se puede pasar directamente como `cb` a cualquier función del backend
 * que acepte un ProgressCallback, con `user_data` apuntando a un
 * GuiThreadBridgeTarget que el llamador posee durante toda la operación.
 *
 * Se puede llamar desde cualquier hilo (incluyendo varios hilos worker de
 * un mismo pool concurrentemente). Copia `update` y entrega la copia a
 * `target->handler`, en el hilo principal de GTK, mediante g_idle_add().
 */
void gui_thread_bridge_post(const ProgressUpdate *update, void *user_data);

#endif // GUI_THREAD_BRIDGE_H
```

Create `src/gui/gui_thread_bridge.c`:

```c
#include "gui_thread_bridge.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRIDGE_PHASE_BUFFER 128

typedef struct {
    GuiThreadBridgeTarget target;
    char phase[BRIDGE_PHASE_BUFFER];
    int current;
    int total;
    // Copia mutable de target.watch: GLib la pone en NULL automáticamente
    // si el objeto observado se destruye antes de que este mensaje se
    // procese. target.watch en sí queda sin tocar, para poder distinguir
    // "nunca hubo watch" (target.watch == NULL) de "había watch pero ya
    // murió" (target.watch != NULL pero watch_slot == NULL).
    GObject *watch_slot;
} BridgeMessage;

static gboolean bridge_dispatch(gpointer data) {
    BridgeMessage *msg = (BridgeMessage *)data;

    gboolean watch_still_alive = (msg->target.watch == NULL) || (msg->watch_slot != NULL);
    if (watch_still_alive) {
        if (msg->target.watch != NULL) {
            g_object_remove_weak_pointer(msg->target.watch, (gpointer *)&msg->watch_slot);
        }
        ProgressUpdate copy = { .phase = msg->phase, .current = msg->current, .total = msg->total };
        msg->target.handler(&copy, msg->target.ui_user_data);
    }
    // Si watch_still_alive es falso, el objeto observado ya fue finalizado:
    // GLib ya limpió su propio registro de weak pointer, no hay nada que
    // remover acá.

    free(msg);
    return G_SOURCE_REMOVE;
}

void gui_thread_bridge_post(const ProgressUpdate *update, void *user_data) {
    GuiThreadBridgeTarget *target = (GuiThreadBridgeTarget *)user_data;
    if (target == NULL || target->handler == NULL || update == NULL) {
        return;
    }

    BridgeMessage *msg = malloc(sizeof(BridgeMessage));
    if (msg == NULL) {
        fprintf(stderr, "[gui_thread_bridge] No se pudo asignar memoria, se descarta una actualización de progreso\n");
        return;
    }

    msg->target = *target;
    msg->current = update->current;
    msg->total = update->total;
    if (update->phase != NULL) {
        strncpy(msg->phase, update->phase, sizeof(msg->phase) - 1);
        msg->phase[sizeof(msg->phase) - 1] = '\0';
    } else {
        msg->phase[0] = '\0';
    }

    msg->watch_slot = target->watch;
    if (target->watch != NULL) {
        g_object_add_weak_pointer(target->watch, (gpointer *)&msg->watch_slot);
    }

    g_idle_add(bridge_dispatch, msg);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude `pkg-config --cflags --libs gobject-2.0` -o tests/unit/test_gui_thread_bridge tests/unit/test_gui_thread_bridge.c src/gui/gui_thread_bridge.c && ./tests/unit/test_gui_thread_bridge`
Expected: PASS — all three `-> OK` lines print, zero compiler warnings.

- [ ] **Step 5: Commit**

```bash
git add include/gui_thread_bridge.h src/gui/gui_thread_bridge.c tests/unit/test_gui_thread_bridge.c
git commit -m "feat: add GLib thread-safety bridge for backend progress callbacks"
```

---

### Task 4: App shell — icon rail, status bar, content stack, Night Watch CSS

**Files:**
- Create: `include/gui_shell.h`
- Create: `src/gui/gui_shell.c`
- Create: `resources/style/night-watch.css`

**Interfaces:**
- Produces: `GtkWidget *gui_shell_create(void);` and `GtkWidget *gui_shell_get_page_container(int page_index);` (valid `page_index` is `0`..`4`, returns `NULL` outside that range). Task 6 uses both to wire the shell into `gui_main.c` and to embed the Task 5 demo into page `0`.

- [ ] **Step 1: Write the CSS stylesheet**

Create `resources/style/night-watch.css`:

```css
box.nw-shell {
    background-color: #12141C;
}

box.nw-status-bar {
    background-color: #161927;
    border-bottom: 1px solid #2A2E3D;
    padding: 10px 14px;
}

label.nw-title {
    color: #E8E6DE;
    font-weight: bold;
    font-size: 14px;
}

label.nw-badge {
    color: #4FB0A5;
    background-color: rgba(79, 176, 165, 0.15);
    border: 1px solid #4FB0A5;
    border-radius: 10px;
    padding: 3px 8px;
    font-size: 10px;
}

box.nw-rail {
    background-color: #12141C;
    border-right: 1px solid #2A2E3D;
    padding: 8px 0;
}

button.nw-rail-button {
    background-color: transparent;
    background-image: none;
    border: 1px solid transparent;
    color: #7d8496;
    font-size: 16px;
    padding: 6px;
}

button.nw-rail-button:checked {
    background-color: rgba(217, 142, 63, 0.15);
    border: 1px solid #D98E3F;
    border-radius: 6px;
    color: #D98E3F;
}

label.nw-placeholder {
    color: #7d8496;
    font-size: 14px;
}
```

This is not a test-driven step (it's a stylesheet, not logic) — the file's correctness is verified visually in Step 4 below.

- [ ] **Step 2: Write the header**

Create `include/gui_shell.h`:

```c
#ifndef GUI_SHELL_H
#define GUI_SHELL_H

#include <gtk/gtk.h>

/**
 * Construye el shell nuevo: barra de estado/insignia arriba, riel de
 * iconos angosto a la izquierda, y un GtkStack de 5 páginas a la derecha
 * (4 son placeholders "próximamente"; la página 0, Dashboard, se entrega
 * vacía para que el llamador la llene). Devuelve un contenedor listo para
 * empacar en cualquier GtkBox/ventana existente -- no crea su propia
 * ventana. Carga y aplica el stylesheet Night Watch la primera vez que se
 * llama.
 */
GtkWidget *gui_shell_create(void);

/**
 * Devuelve el contenedor de la página `page_index` (0=Dashboard,
 * 1=USB, 2=Procesos, 3=Puertos, 4=Registros) para que el llamador le
 * agregue contenido real. NULL si `page_index` está fuera de [0, 4] o si
 * gui_shell_create() todavía no fue llamado.
 */
GtkWidget *gui_shell_get_page_container(int page_index);

#endif // GUI_SHELL_H
```

- [ ] **Step 3: Write the implementation**

Create `src/gui/gui_shell.c`:

```c
#include "gui_shell.h"
#include <stdio.h>

#define GUI_SHELL_PAGE_COUNT 5
#define GUI_SHELL_CSS_PATH "resources/style/night-watch.css"

static GtkWidget *rail_buttons[GUI_SHELL_PAGE_COUNT];
static GtkWidget *page_containers[GUI_SHELL_PAGE_COUNT];
static GtkWidget *content_stack = NULL;

static const char *page_names[GUI_SHELL_PAGE_COUNT] = {
    "page0", "page1", "page2", "page3", "page4"
};
static const char *page_icons[GUI_SHELL_PAGE_COUNT] = {
    "\xF0\x9F\x93\x8A", "\xF0\x9F\x92\xBE", "\xE2\x9A\xA1", "\xF0\x9F\x94\x8C", "\xF0\x9F\x93\x9C"
};
static const char *page_labels[GUI_SHELL_PAGE_COUNT] = {
    "Dashboard", "Dispositivos USB", "Procesos", "Puertos", "Registros"
};

static void load_night_watch_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    gtk_css_provider_load_from_path(provider, GUI_SHELL_CSS_PATH, &error);
    if (error != NULL) {
        fprintf(stderr, "[gui_shell] No se pudo cargar %s: %s\n", GUI_SHELL_CSS_PATH, error->message);
        g_error_free(error);
    } else {
        GdkScreen *screen = gdk_screen_get_default();
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

static void on_rail_button_toggled(GtkToggleButton *button, gpointer user_data) {
    int index = GPOINTER_TO_INT(user_data);
    if (!gtk_toggle_button_get_active(button)) {
        return; // solo actuamos cuando un botón se prende, no cuando se apaga
    }
    for (int i = 0; i < GUI_SHELL_PAGE_COUNT; i++) {
        if (i != index) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rail_buttons[i]), FALSE);
        }
    }
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack), page_names[index]);
}

static GtkWidget *create_status_badge_bar(void) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "nw-status-bar");

    GtkWidget *title = gtk_label_new("\xF0\x9F\x9B\xA1\xEF\xB8\x8F MatCom Guard");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(bar), title, TRUE, TRUE, 0);

    GtkWidget *badge = gtk_label_new("\xE2\x97\x8F SISTEMA SEGURO");
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), "nw-badge");
    gtk_box_pack_end(GTK_BOX(bar), badge, FALSE, FALSE, 0);

    return bar;
}

static GtkWidget *create_icon_rail(void) {
    GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(rail), "nw-rail");
    gtk_widget_set_size_request(rail, 44, -1);

    for (int i = 0; i < GUI_SHELL_PAGE_COUNT; i++) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(page_icons[i]);
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "nw-rail-button");
        gtk_widget_set_tooltip_text(btn, page_labels[i]);
        g_signal_connect(btn, "toggled", G_CALLBACK(on_rail_button_toggled), GINT_TO_POINTER(i));
        rail_buttons[i] = btn;
        gtk_box_pack_start(GTK_BOX(rail), btn, FALSE, FALSE, 0);
    }

    return rail;
}

static GtkWidget *create_placeholder_page(const char *label_text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "nw-placeholder");
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);

    return box;
}

static void create_content_stack(void) {
    content_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(content_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    for (int i = 0; i < GUI_SHELL_PAGE_COUNT; i++) {
        if (i == 0) {
            // El Dashboard lo llena el llamador (ver gui_shell_get_page_container);
            // acá solo se crea un contenedor vacío.
            page_containers[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        } else {
            char message[128];
            snprintf(message, sizeof(message), "%s %s \xE2\x80\x94 pr\xC3\xB3ximamente", page_icons[i], page_labels[i]);
            page_containers[i] = create_placeholder_page(message);
        }
        gtk_stack_add_named(GTK_STACK(content_stack), page_containers[i], page_names[i]);
    }
}

GtkWidget *gui_shell_create(void) {
    load_night_watch_css();

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(root), "nw-shell");

    gtk_box_pack_start(GTK_BOX(root), create_status_badge_bar(), FALSE, FALSE, 0);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(body), create_icon_rail(), FALSE, FALSE, 0);

    create_content_stack();
    gtk_box_pack_start(GTK_BOX(body), content_stack, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rail_buttons[0]), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack), page_names[0]);

    return root;
}

GtkWidget *gui_shell_get_page_container(int page_index) {
    if (page_index < 0 || page_index >= GUI_SHELL_PAGE_COUNT) {
        return NULL;
    }
    return page_containers[page_index];
}
```

Note on the escaped UTF-8 byte sequences in the string literals above (e.g. `"\xF0\x9F\x93\x8A"` for 📊): this avoids relying on the source file's own encoding to carry multi-byte emoji correctly through every tool in the chain (editor, git, compiler) — the same defensive style already used elsewhere in this codebase after an earlier mojibake bug. If your editor and toolchain are confirmed UTF-8 clean end-to-end, plain UTF-8 emoji literals in the source are equivalent; the escaped form is the safe default.

- [ ] **Step 4: Manual visual verification (not committed)**

Save as `/tmp/gui_shell_preview.c`:

```c
#include <gtk/gtk.h>
#include "gui_shell.h"

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 700, 450);
    gtk_window_set_title(GTK_WINDOW(window), "gui_shell preview");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_container_add(GTK_CONTAINER(window), gui_shell_create());

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
```

Run (from WSL, from the repo root so the relative CSS path resolves):
```bash
gcc -Wall -Wextra -std=c99 -Iinclude `pkg-config --cflags --libs gtk+-3.0` -o /tmp/gui_shell_preview /tmp/gui_shell_preview.c src/gui/gui_shell.c
GDK_BACKEND=x11 /tmp/gui_shell_preview
```
Expected: a window with a dark status bar at the top ("🛡️ MatCom Guard" + a teal "● SISTEMA SEGURO" badge), a narrow dark icon rail on the left with 5 buttons (first one shown highlighted in amber), and a content area on the right showing an empty area for page 0 and "📊 Dashboard — próximamente"-style text if you click any other rail icon. Clicking each rail button switches pages and highlights only that button. Close the window to exit. Delete `/tmp/gui_shell_preview.c` and `/tmp/gui_shell_preview` when done.

- [ ] **Step 5: Commit**

```bash
git add include/gui_shell.h src/gui/gui_shell.c resources/style/night-watch.css
git commit -m "feat: add app shell (icon-rail nav, status bar, Night Watch CSS)"
```

---

### Task 5: Demo scan — real end-to-end proof on the Dashboard page

**Files:**
- Create: `include/gui_demo_scan.h`
- Create: `src/gui/gui_demo_scan.c`

**Interfaces:**
- Consumes: `guard_dial_new`/`guard_dial_set_progress` (Task 2), `GuiThreadBridgeTarget`/`gui_thread_bridge_post` (Task 3), `scan_ports_range`/`ScanResult` (`include/port_scanner.h`, already in the merged backend)
- Produces: `GtkWidget *gui_demo_scan_create_widget(void);` and `void gui_demo_scan_shutdown(void);` — Task 6 embeds the former into the Dashboard page and calls the latter from `on_window_destroy`.

This task has no automated test: it is GTK button-wiring plus one already-tested backend call and one already-tested bridge call. Its correctness is verified as part of Task 6's full manual smoke test (there is no way to exercise the "click a button, see a real scan run" behavior without the window it will live in).

- [ ] **Step 1: Write the implementation**

Create `include/gui_demo_scan.h`:

```c
#ifndef GUI_DEMO_SCAN_H
#define GUI_DEMO_SCAN_H

#include <gtk/gtk.h>

/**
 * Crea el widget de demo (dial + botones "Probar escaneo"/"Cancelar") para
 * embeber en la página Dashboard del shell. Solo puede existir una
 * instancia por proceso (el estado del escaneo demo es global) -- llamarla
 * más de una vez sobrescribe los widgets de la instancia anterior.
 */
GtkWidget *gui_demo_scan_create_widget(void);

/**
 * Si hay un escaneo demo en curso, pide su cancelación y une (pthread_join)
 * el hilo antes de devolver el control. Debe llamarse antes de liberar
 * cualquier recurso compartido durante el apagado de la aplicación (p.ej.
 * antes de cleanup_complete_backend_system() en gui_main.c). Si no hay
 * ningún escaneo en curso, no hace nada.
 */
void gui_demo_scan_shutdown(void);

#endif // GUI_DEMO_SCAN_H
```

Create `src/gui/gui_demo_scan.c`:

```c
#include "gui_demo_scan.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "port_scanner.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static GtkWidget *dial = NULL;
static GtkWidget *scan_button = NULL;
static GtkWidget *cancel_button = NULL;
static pthread_t scan_thread;
static volatile sig_atomic_t cancel_flag = 0;
static volatile sig_atomic_t scan_running = 0;

typedef struct {
    ScanResult result;
} ScanDoneMessage;

static void on_demo_scan_progress(const ProgressUpdate *update, void *ui_user_data) {
    GtkWidget *dial_widget = (GtkWidget *)ui_user_data;
    guard_dial_set_progress(dial_widget, update->current, update->total, update->phase);
}

static gboolean on_scan_done_idle(gpointer data) {
    ScanDoneMessage *msg = (ScanDoneMessage *)data;

    char summary[128];
    snprintf(summary, sizeof(summary), "Listo: %d abiertos de %d",
              msg->result.open_ports, msg->result.total_ports);
    guard_dial_set_progress(dial, msg->result.total_ports, msg->result.total_ports, summary);

    if (msg->result.ports != NULL) {
        free(msg->result.ports);
    }
    free(msg);

    gtk_widget_set_sensitive(scan_button, TRUE);
    gtk_widget_set_sensitive(cancel_button, FALSE);
    scan_running = 0;

    return G_SOURCE_REMOVE;
}

static void *demo_scan_thread_main(void *arg) {
    (void)arg;

    GuiThreadBridgeTarget target = {
        .handler = on_demo_scan_progress,
        .ui_user_data = dial,
        .watch = G_OBJECT(dial)
    };

    ScanDoneMessage *msg = malloc(sizeof(ScanDoneMessage));
    if (msg == NULL) {
        fprintf(stderr, "[gui_demo_scan] No se pudo asignar memoria para el resultado del escaneo\n");
        scan_running = 0;
        return NULL;
    }

    int rc = scan_ports_range(1, 100, 4, gui_thread_bridge_post, &target, &cancel_flag, &msg->result);
    if (rc != 0) {
        msg->result.ports = NULL;
        msg->result.total_ports = 0;
        msg->result.open_ports = 0;
        msg->result.suspicious_ports = 0;
    }

    g_idle_add(on_scan_done_idle, msg);
    return NULL;
}

static void on_scan_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (scan_running) {
        return;
    }
    cancel_flag = 0;
    scan_running = 1;
    gtk_widget_set_sensitive(scan_button, FALSE);
    gtk_widget_set_sensitive(cancel_button, TRUE);
    guard_dial_set_progress(dial, 0, 100, "Iniciando escaneo de prueba...");
    pthread_create(&scan_thread, NULL, demo_scan_thread_main, NULL);
}

static void on_cancel_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    cancel_flag = 1;
}

GtkWidget *gui_demo_scan_create_widget(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(box), dial, FALSE, FALSE, 0);

    GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(button_row, GTK_ALIGN_CENTER);

    scan_button = gtk_button_new_with_label("Probar escaneo (127.0.0.1:1-100)");
    g_signal_connect(scan_button, "clicked", G_CALLBACK(on_scan_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(button_row), scan_button, FALSE, FALSE, 0);

    cancel_button = gtk_button_new_with_label("Cancelar");
    gtk_widget_set_sensitive(cancel_button, FALSE);
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(on_cancel_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(button_row), cancel_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), button_row, FALSE, FALSE, 0);

    return box;
}

void gui_demo_scan_shutdown(void) {
    if (!scan_running) {
        return;
    }
    cancel_flag = 1;
    pthread_join(scan_thread, NULL);
    scan_running = 0;
}
```

- [ ] **Step 2: Confirm the file compiles clean on its own**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude `pkg-config --cflags gtk+-3.0` -c src/gui/gui_demo_scan.c -o /tmp/gui_demo_scan.o`
Expected: compiles with zero warnings. (This does not link or run it — `scan_ports_range` isn't available as an object file here; Task 6's full build is what actually links and exercises this file.)

- [ ] **Step 3: Commit**

```bash
git add include/gui_demo_scan.h src/gui/gui_demo_scan.c
git commit -m "feat: add demo port-scan widget wiring dial+bridge+backend"
```

---

### Task 6: Wire the shell into gui_main.c

**Files:**
- Modify: `src/gui/gui_main.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `gui_shell_create`/`gui_shell_get_page_container` (Task 4), `gui_demo_scan_create_widget`/`gui_demo_scan_shutdown` (Task 5)

- [ ] **Step 1: Add the new includes**

In `src/gui/gui_main.c`, add these two lines to the existing include block near the top (after `#include "gui_backend_adapters.h"`):

```c
#include "gui_shell.h"
#include "gui_demo_scan.h"
```

- [ ] **Step 2: Delete `create_main_notebook()`**

Delete the entire `create_main_notebook()` function (from `static GtkWidget* create_main_notebook() {` through its closing `}` — the block that builds the `GtkNotebook` and appends the 5 tabs). It has no remaining callers after Step 3 below, and it is `static`, so leaving it in place would trigger a `-Wunused-function` warning.

- [ ] **Step 3: Replace the notebook creation call site in `init_gui()`**

In `init_gui()`, find:

```c
    notebook = create_main_notebook();
    gtk_box_pack_start(GTK_BOX(main_container), notebook, TRUE, TRUE, 0);
```

Replace with:

```c
    GtkWidget *shell = gui_shell_create();
    gtk_box_pack_start(GTK_BOX(main_container), shell, TRUE, TRUE, 0);

    GtkWidget *dashboard_page = gui_shell_get_page_container(0);
    if (dashboard_page != NULL) {
        gtk_box_pack_start(GTK_BOX(dashboard_page), gui_demo_scan_create_widget(), TRUE, TRUE, 0);
    }
```

- [ ] **Step 4: Guard the now-dangling `notebook` references**

The global `GtkWidget *notebook = NULL;` is never assigned anymore after Step 3, so every existing call that does `gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), N)` would pass NULL and trigger a GTK critical warning at runtime if triggered from the still-present header-bar scan menu. Guard each of the 4 call sites with a NULL check:

In `on_scan_all_clicked`, change:
```c
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 4);
```
to:
```c
    if (notebook != NULL) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 4);
    }
```

Apply the same `if (notebook != NULL) { ... }` guard around the single `gtk_notebook_set_current_page(...)` call in each of `on_scan_usb_menu_clicked` (page `1`), `on_scan_processes_menu_clicked` (page `2`), and `on_scan_ports_menu_clicked` (page `3`).

- [ ] **Step 5: Shut the demo down cleanly on window close**

In `on_window_destroy`, add a call to `gui_demo_scan_shutdown()` as the very first line, before the existing `cleanup_complete_backend_system();` call:

```c
static void on_window_destroy(GtkWidget *widget __attribute__((unused)), gpointer data __attribute__((unused))) {
    gui_demo_scan_shutdown();

    gui_add_log_entry("SISTEMA", "INFO", "Cerrando MatCom Guard - iniciando secuencia de apagado seguro...");
    ...
```//^ keep the rest of the function body exactly as it is today.

- [ ] **Step 6: Add the new files to the Makefile**

In `Makefile`, add the 4 new `.c` files to `SRC`, right after `src/device_monitor.c` and before `src/gui/gui_main.c`:

```makefile
SRC = src/main.c \
		src/port_scanner.c \
		src/threadpool.c \
		src/process_monitor.c \
		src/device_monitor.c \
		src/gui/widgets/guard_dial.c \
		src/gui/gui_thread_bridge.c \
		src/gui/gui_shell.c \
		src/gui/gui_demo_scan.c \
		src/gui/gui_main.c \
		src/gui/window/gui_logging.c \
		src/gui/window/gui_stats.c \
		src/gui/window/gui_status.c \
		src/gui/window/gui_usb_panel.c \
		src/gui/window/gui_process_panel.c \
		src/gui/window/gui_ports_panel.c \
		src/gui/window/gui_config_dialog.c \
		src/gui/integration/gui_system_coordinator.c \
		src/gui/integration/gui_backend_adapters.c \
		src/gui/integration/gui_process_integration.c \
		src/gui/integration/gui_ports_integration.c \
		src/gui/integration/gui_usb_integration.c
```

- [ ] **Step 7: Full clean rebuild**

Run (from WSL): `make clean && make 2>&1 | tee /tmp/build.log; cat /tmp/build.log`
Expected: the log's only content is the single `gcc ...` compile/link command line — no warning text anywhere. (Read the log file directly rather than trusting a chained `grep`/exit-code check across a Windows↔WSL shell boundary — that combination produced misleading results earlier in this project's history.)

- [ ] **Step 8: Manual smoke test of the real application**

Run (from WSL, with a display — WSLg or similar): `GDK_BACKEND=x11 ./matcom-guard`
Expected, by hand:
1. The window opens showing the native title bar (unchanged) above the new Night Watch shell: a dark status bar with "🛡️ MatCom Guard" and a teal "● SISTEMA SEGURO" badge, a narrow icon rail on the left (Dashboard's icon highlighted amber), and the Dashboard page showing the guard dial plus "Probar escaneo (127.0.0.1:1-100)" and "Cancelar" buttons.
2. Clicking the other 4 rail icons switches to each one's "— próximamente" placeholder text and highlights only that icon.
3. Back on Dashboard, click "Probar escaneo": the dial's amber arc grows and the phase text updates as the scan progresses; "Cancelar" becomes clickable. Clicking "Cancelar" stops the scan promptly (matching the already-reviewed backend cancellation behavior) and the dial shows a "Listo: N abiertos de M" summary; "Probar escaneo" becomes clickable again.
4. Close the window: no crash, no hang, no GTK critical warnings printed to the console about the notebook.

Take a screenshot for the record (same WSLg + `import`/`xdotool` workflow already used earlier in this project for the README screenshots) if you want visual confirmation to keep.

- [ ] **Step 9: Commit**

```bash
git add src/gui/gui_main.c Makefile
git commit -m "feat: wire the new shell and demo scan into gui_main, retire the notebook"
```

---

### Task 7: Wire the 2 new unit tests into the Makefile and do a full verification pass

**Files:**
- Modify: `Makefile`

**Interfaces:**
- Consumes: everything from Tasks 1-6

- [ ] **Step 1: Add the two new unit-test targets**

In `Makefile`, add these two target definitions next to the existing unit-test targets (after `test-process-shutdown` and before the `test-unit` aggregate target):

```makefile
test-guard-dial-math: tests/unit/test_guard_dial_math.c src/gui/widgets/guard_dial.c
	$(CC) $(UNIT_CFLAGS) $(GTK_FLAGS) -o tests/unit/test_guard_dial_math tests/unit/test_guard_dial_math.c src/gui/widgets/guard_dial.c
	./tests/unit/test_guard_dial_math

test-gui-thread-bridge: tests/unit/test_gui_thread_bridge.c src/gui/gui_thread_bridge.c
	$(CC) $(UNIT_CFLAGS) `pkg-config --cflags --libs gobject-2.0` -o tests/unit/test_gui_thread_bridge tests/unit/test_gui_thread_bridge.c src/gui/gui_thread_bridge.c
	./tests/unit/test_gui_thread_bridge
```

Update the `test-unit` aggregate target to include both:

```makefile
test-unit: test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown test-guard-dial-math test-gui-thread-bridge
```

Update the `.PHONY` line for the unit-test targets to add the two new names:

```makefile
.PHONY: test-unit test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown test-guard-dial-math test-gui-thread-bridge benchmark-port-scan
```

Also extend `clean`'s `rm -f` line for the test binaries to include the two new ones:

```makefile
	rm -f tests/unit/test_progress tests/unit/test_threadpool tests/unit/test_port_classification tests/unit/test_port_scan_range tests/unit/test_hash_skip tests/unit/test_process_shutdown tests/unit/test_guard_dial_math tests/unit/test_gui_thread_bridge tests/unit/benchmark_port_scan
```

- [ ] **Step 2: Run the full unit-test suite**

Run (from WSL): `make test-unit`
Expected: all eight tests print their own success line (`test_progress: OK`, `test_threadpool: OK (...)`, `test_port_classification: OK`, `test_port_scan_range: OK`, `test_hash_skip: OK`, `test_parallel_hash_skip_integration: OK`, `test_process_shutdown: ... OK`, `test_guard_dial_math: OK`, and the 3 `test_gui_thread_bridge: ... -> OK` lines), zero compiler warnings anywhere in the output.

- [ ] **Step 3: Full clean rebuild of the real application**

Run (from WSL): `make clean && make 2>&1 | tee /tmp/build.log; cat /tmp/build.log`
Expected: only the single `gcc ...` compile/link line — zero warnings.

- [ ] **Step 4: Final manual smoke test**

Run (from WSL): `GDK_BACKEND=x11 ./matcom-guard`
Expected: same as Task 6 Step 8 — shell renders correctly, all 5 rail icons switch pages, the Dashboard demo scan runs and can be cancelled, no crash on close.

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "build: wire the two new unit-test targets into the Makefile"
```

Do not push or open a pull request as part of this plan — that happens after the human reviews the finished branch, the same way it did for the backend rewrite.

---

## Self-Review Notes

- **Spec coverage:** every Goal in the spec maps to a task — icon-rail/status-bar shell (Task 4), Night Watch CSS (Task 4), guard-dial widget (Tasks 1-2), thread bridge matching `ProgressCallback`'s signature (Task 3), the real end-to-end demo scan with cancellation (Task 5, wired in Task 6), zero-warning buildability throughout (every task's own build step, reconfirmed fully in Task 7). The `gui_shell_create(GtkApplication*)` vs `gui_shell_create(void)` signature correction is called out explicitly at the top of this plan rather than silently diverging from the spec.
- **Pre-flight conflict scan:**

| Task A | Task B | Shared file/interface | A produces | B consumes | Finding |
|---|---|---|---|---|---|
| 1 | 2 | `guard_dial_compute_arc_fraction` | pure function in guard_dial.c | called from the `draw` handler | Match; same file, sequential |
| 2 | 5 | `guard_dial_new`/`guard_dial_set_progress` | `GtkWidget*` factory + setter | demo widget creates one dial and updates it from the progress handler | Match |
| 3 | 5 | `GuiThreadBridgeTarget`/`gui_thread_bridge_post` | struct + function matching `ProgressCallback` | passed directly as `cb` to `scan_ports_range` | Match; signature checked against `include/progress.h`'s `ProgressCallback` by hand |
| (backend, already merged) | 5 | `scan_ports_range` | `include/port_scanner.h` | called with `(1, 100, 4, gui_thread_bridge_post, &target, &cancel_flag, &msg->result)` | Signature cross-checked against the current header; no backend file is modified |
| 4 | 6 | `gui_shell_create`/`gui_shell_get_page_container` | shell widget + page-lookup function | `gui_main.c` packs the shell and reaches into page 0 | Match |
| 5 | 6 | `gui_demo_scan_create_widget`/`gui_demo_scan_shutdown` | demo widget + shutdown hook | packed into page 0; shutdown called from `on_window_destroy` | Match |
| 1,2,3,4,5,6 | 7 | `Makefile` test targets | test filenames | Makefile references each by exact path | Both new filenames (`test_guard_dial_math.c`, `test_gui_thread_bridge.c`) cross-checked byte-for-byte |
| 6 | (self) | the `notebook` global and 4 call sites | — | each guarded with `if (notebook != NULL)` instead of deleted, since the variable and header-bar menu items are out of this plan's scope | Verified all 4 call sites (`on_scan_all_clicked`, `on_scan_usb_menu_clicked`, `on_scan_processes_menu_clicked`, `on_scan_ports_menu_clicked`) against the current file |

- **Self-consistency:** every header declaration matches its `.c` definition's signature exactly (checked by hand while writing this plan). No task deletes any of the 10 old panel/integration files or touches any backend source/header. `gui_demo_scan.c`'s module-level statics (`dial`, `scan_button`, `cancel_button`, `scan_thread`, `cancel_flag`, `scan_running`) are safe under the single-instance assumption stated in `gui_demo_scan.h`'s doc comment — there is exactly one call site (Task 6) and exactly one running instance of the app.
- **Global Constraints re-check:** no task touches any of the 10 old panel/integration files or any backend file; every new/changed file targets `-Wall -Wextra -std=c99` zero warnings; every GTK widget mutation from a worker thread goes through `gui_thread_bridge_post`/`g_idle_add` (Task 5's `on_scan_done_idle` also uses a plain `g_idle_add`, consistent with the spec's Data Flow step 6); malloc is checked in every new allocation (`guard_dial_new`'s state, `gui_thread_bridge_post`'s message, `gui_demo_scan`'s `ScanDoneMessage`).

**Scan is clean.**
