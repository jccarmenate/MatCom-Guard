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
