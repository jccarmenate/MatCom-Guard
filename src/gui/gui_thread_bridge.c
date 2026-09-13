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
    // GWeakRef es seguro de inicializar desde un hilo que no tiene su
    // propia referencia fuerte al objeto observado -- a diferencia de
    // g_object_add_weak_pointer(), que necesita tocar la memoria propia
    // del objeto (su GData) para registrarse, lo cual sería inseguro si
    // esa memoria ya fue liberada por un finalize concurrente, GWeakRef
    // usa una tabla global indexada por el valor del puntero, protegida
    // por el mismo lock que toma dispose(). Solo se inicializa si
    // target.watch != NULL, y siempre se limpia con g_weak_ref_clear()
    // antes de liberar el mensaje.
    GWeakRef watch_ref;
} BridgeMessage;

static gboolean bridge_dispatch(gpointer data) {
    BridgeMessage *msg = (BridgeMessage *)data;

    gboolean should_deliver = TRUE;
    if (msg->target.watch != NULL) {
        gpointer still_alive = g_weak_ref_get(&msg->watch_ref);
        if (still_alive != NULL) {
            g_object_unref(still_alive);
        } else {
            should_deliver = FALSE;
        }
        g_weak_ref_clear(&msg->watch_ref);
    }

    if (should_deliver) {
        ProgressUpdate copy = { .phase = msg->phase, .current = msg->current, .total = msg->total };
        msg->target.handler(&copy, msg->target.ui_user_data);
    }

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

    if (target->watch != NULL) {
        g_weak_ref_init(&msg->watch_ref, target->watch);
    }

    g_idle_add(bridge_dispatch, msg);
}
