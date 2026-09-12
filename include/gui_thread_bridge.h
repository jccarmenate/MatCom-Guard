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
