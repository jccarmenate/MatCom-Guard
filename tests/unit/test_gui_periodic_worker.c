// tests/unit/test_gui_periodic_worker.c
#define _DEFAULT_SOURCE
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
