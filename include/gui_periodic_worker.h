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
