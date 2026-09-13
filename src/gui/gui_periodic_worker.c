// src/gui/gui_periodic_worker.c
#define _DEFAULT_SOURCE
#include "gui_periodic_worker.h"
#include <errno.h>
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
