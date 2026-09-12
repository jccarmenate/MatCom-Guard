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
