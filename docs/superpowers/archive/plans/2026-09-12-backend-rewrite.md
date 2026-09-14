# Backend Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite `device_monitor.c`, `process_monitor.c`, and `port_scanner.c` with a shared progress/cancellation contract and a shared thread pool, parallelizing port scanning and USB file hashing, without changing any GUI file or breaking any existing caller.

**Architecture:** A new `ProgressUpdate`/`ProgressCallback` pair (push model) and a `volatile sig_atomic_t *` cancellation flag are added as a shared contract in `include/progress.h`, consumed by all three backend modules. A new generic `threadpool.c/h` (bounded worker pool over a shared task queue) is used by both the port scanner (parallel `connect()`) and the device monitor (parallel SHA-256 hashing). Every existing public function keeps its exact current signature and behavior; new capability is added through new entry points (`scan_ports_range`, `create_device_snapshot_ex`) so no existing GUI call site needs to change in this plan.

**Tech Stack:** C99, pthreads, OpenSSL (SHA-256), POSIX sockets — same as the existing project. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-12-backend-rewrite-design.md`

## Global Constraints

- C99 standard, built with `-Wall -Wextra -std=c99 -g -Iinclude -DDEBUG` — every new/changed file must compile with **zero warnings** under this exact flag set.
- No file under `src/gui/` or any `include/gui*.h` may be modified by this plan — that's sub-project 2.
- The port scanner's target stays hardcoded to `127.0.0.1` — do not add host/network configurability.
- `matcomguard.conf`'s format and loading behavior do not change.
- Every `malloc`/`realloc` return value is checked before use; every piece of state shared between threads is protected by a mutex for every access — these are the exact invariants fixed in commit `2b01d8a` and must not regress.
- `scan_directory_recursive` keeps using `lstat` (not `stat`) and skipping symlinks (`S_ISLNK`), to avoid infinite recursion on a symlink cycle.
- All work happens on branch `rewrite/backend`, created from the current tip of `main` — never commit this work directly to `main`.

---

## Setup (do this once, before Task 1)

```bash
cd /path/to/Matcom-Guard
git checkout main
git pull
git checkout -b rewrite/backend
```

All tasks below assume you're on `rewrite/backend`. All manual verification (running the built binary or test programs) happens under WSL/Linux, the same way this session verified builds — from a Windows shell that's `cd /mnt/c/.../Matcom-Guard && make ...` inside `wsl -d <distro> -- bash -c "..."`, or directly if you're already on Linux.

---

### Task 1: Shared progress contract (`include/progress.h`)

**Files:**
- Create: `include/progress.h`
- Test: `tests/unit/test_progress.c`

**Interfaces:**
- Produces: `ProgressUpdate` (struct: `const char *phase`, `int current`, `int total`), `ProgressCallback` (typedef: `void (*)(const ProgressUpdate *update, void *user_data)`)

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_progress.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "progress.h"

static int callback_invocations = 0;
static char last_phase[128];
static int last_current = -1;
static int last_total = -1;

static void record_progress(const ProgressUpdate *update, void *user_data) {
    int *counter = (int *)user_data;
    (*counter)++;
    callback_invocations++;
    strncpy(last_phase, update->phase, sizeof(last_phase) - 1);
    last_phase[sizeof(last_phase) - 1] = '\0';
    last_current = update->current;
    last_total = update->total;
}

int main(void) {
    int user_counter = 0;
    ProgressCallback cb = record_progress;

    ProgressUpdate update1 = { .phase = "Escaneando puertos", .current = 10, .total = 100 };
    cb(&update1, &user_counter);

    assert(callback_invocations == 1);
    assert(user_counter == 1);
    assert(strcmp(last_phase, "Escaneando puertos") == 0);
    assert(last_current == 10);
    assert(last_total == 100);

    ProgressUpdate update2 = { .phase = "Monitoreando...", .current = 0, .total = 0 };
    cb(&update2, &user_counter);

    assert(callback_invocations == 2);
    assert(last_total == 0);  // 0 = indeterminado

    printf("test_progress: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_progress tests/unit/test_progress.c`
Expected: FAIL — `progress.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `include/progress.h`:

```c
#ifndef PROGRESS_H
#define PROGRESS_H

/**
 * Reporte de avance de una operación potencialmente larga (escaneo de
 * puertos, análisis de un dispositivo USB, ciclo de monitoreo de procesos).
 *
 * `phase` describe qué está pasando ahora mismo (p.ej. "Escaneando puertos
 * altos"). Es válido solo durante la llamada al callback — si necesitás
 * conservarlo, copialo antes de que el callback retorne.
 *
 * `total == 0` significa progreso indeterminado (no hay un total conocido
 * de antemano, como en el monitoreo continuo de procesos); en ese caso
 * `current` no debe interpretarse como una fracción de `total`.
 */
typedef struct {
    const char *phase;
    int current;
    int total;
} ProgressUpdate;

/**
 * El backend puede invocar este callback desde cualquier hilo trabajador.
 * Quien lo implementa es responsable de despachar a su propio hilo principal
 * si necesita tocar una interfaz gráfica (p.ej. con g_idle_add en GTK) — el
 * backend no depende de ningún toolkit de GUI.
 */
typedef void (*ProgressCallback)(const ProgressUpdate *update, void *user_data);

#endif // PROGRESS_H
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_progress tests/unit/test_progress.c && ./tests/unit/test_progress`
Expected: PASS — prints `test_progress: OK`

- [ ] **Step 5: Commit**

```bash
git add include/progress.h tests/unit/test_progress.c
git commit -m "feat: add shared ProgressUpdate/ProgressCallback contract"
```

---

### Task 2: Shared thread pool (`src/threadpool.c`, `include/threadpool.h`)

**Files:**
- Create: `include/threadpool.h`
- Create: `src/threadpool.c`
- Test: `tests/unit/test_threadpool.c`

**Interfaces:**
- Consumes: nothing from Task 1 (thread pool is generic, doesn't reference `ProgressUpdate`)
- Produces: `ThreadPool` (opaque struct), `threadpool_create(int num_threads) -> ThreadPool*`, `threadpool_submit(ThreadPool*, void (*task)(void *arg), void *arg) -> int`, `threadpool_wait(ThreadPool*) -> void`, `threadpool_destroy(ThreadPool*) -> void`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_threadpool.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "threadpool.h"

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int counter = 0;

static void increment_task(void *arg) {
    (void)arg;
    pthread_mutex_lock(&counter_lock);
    counter++;
    pthread_mutex_unlock(&counter_lock);
}

int main(void) {
    assert(threadpool_create(0) == NULL);

    ThreadPool *pool = threadpool_create(4);
    assert(pool != NULL);

    const int NUM_TASKS = 200;
    for (int i = 0; i < NUM_TASKS; i++) {
        int rc = threadpool_submit(pool, increment_task, NULL);
        assert(rc == 0);
    }

    threadpool_wait(pool);
    assert(counter == NUM_TASKS);

    threadpool_destroy(pool);

    printf("test_threadpool: OK (%d tareas ejecutadas)\n", counter);
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_threadpool tests/unit/test_threadpool.c -lpthread`
Expected: FAIL — `threadpool.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `include/threadpool.h`:

```c
#ifndef THREADPOOL_H
#define THREADPOOL_H

typedef struct ThreadPool ThreadPool;

/** Crea un pool con `num_threads` trabajadores. NULL si num_threads <= 0 o falla la reserva. */
ThreadPool *threadpool_create(int num_threads);

/** Encola una tarea; `arg` se pasa tal cual a `task`. 0 si se encoló, -1 si falló. */
int threadpool_submit(ThreadPool *pool, void (*task)(void *arg), void *arg);

/** Bloquea hasta que todas las tareas encoladas hasta el momento terminen. */
void threadpool_wait(ThreadPool *pool);

/** Espera las tareas pendientes, detiene los hilos y libera el pool. */
void threadpool_destroy(ThreadPool *pool);

#endif // THREADPOOL_H
```

Create `src/threadpool.c`:

```c
#include <stdlib.h>
#include <pthread.h>
#include "threadpool.h"

typedef struct Task {
    void (*func)(void *arg);
    void *arg;
    struct Task *next;
} Task;

struct ThreadPool {
    pthread_t *workers;
    int num_threads;

    Task *queue_head;
    Task *queue_tail;

    pthread_mutex_t lock;
    pthread_cond_t task_available;
    pthread_cond_t all_done;

    int pending_tasks;  // tareas encoladas o en ejecución, no terminadas
    int shutdown;
};

static void *worker_loop(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->queue_head == NULL && !pool->shutdown) {
            pthread_cond_wait(&pool->task_available, &pool->lock);
        }
        if (pool->queue_head == NULL && pool->shutdown) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        Task *task = pool->queue_head;
        pool->queue_head = task->next;
        if (pool->queue_head == NULL) {
            pool->queue_tail = NULL;
        }
        pthread_mutex_unlock(&pool->lock);

        task->func(task->arg);
        free(task);

        pthread_mutex_lock(&pool->lock);
        pool->pending_tasks--;
        if (pool->pending_tasks == 0) {
            pthread_cond_signal(&pool->all_done);
        }
        pthread_mutex_unlock(&pool->lock);
    }

    return NULL;
}

ThreadPool *threadpool_create(int num_threads) {
    if (num_threads <= 0) {
        return NULL;
    }

    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool) {
        return NULL;
    }

    pool->workers = malloc(num_threads * sizeof(pthread_t));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }

    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->pending_tasks = 0;
    pool->shutdown = 0;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->task_available, NULL);
    pthread_cond_init(&pool->all_done, NULL);

    int created = 0;
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_loop, pool) != 0) {
            break;  // degradar: seguir con los hilos que sí se crearon
        }
        created++;
    }

    if (created == 0) {
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->task_available);
        pthread_cond_destroy(&pool->all_done);
        free(pool->workers);
        free(pool);
        return NULL;
    }

    pool->num_threads = created;
    return pool;
}

int threadpool_submit(ThreadPool *pool, void (*task_func)(void *arg), void *arg) {
    if (!pool || !task_func) {
        return -1;
    }

    Task *task = malloc(sizeof(Task));
    if (!task) {
        return -1;
    }

    task->func = task_func;
    task->arg = arg;
    task->next = NULL;

    pthread_mutex_lock(&pool->lock);
    if (pool->queue_tail == NULL) {
        pool->queue_head = task;
        pool->queue_tail = task;
    } else {
        pool->queue_tail->next = task;
        pool->queue_tail = task;
    }
    pool->pending_tasks++;
    pthread_cond_signal(&pool->task_available);
    pthread_mutex_unlock(&pool->lock);

    return 0;
}

void threadpool_wait(ThreadPool *pool) {
    if (!pool) {
        return;
    }
    pthread_mutex_lock(&pool->lock);
    while (pool->pending_tasks > 0) {
        pthread_cond_wait(&pool->all_done, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}

void threadpool_destroy(ThreadPool *pool) {
    if (!pool) {
        return;
    }

    threadpool_wait(pool);

    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->task_available);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->task_available);
    pthread_cond_destroy(&pool->all_done);
    free(pool->workers);
    free(pool);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_threadpool tests/unit/test_threadpool.c src/threadpool.c -lpthread && ./tests/unit/test_threadpool`
Expected: PASS — prints `test_threadpool: OK (200 tareas ejecutadas)`

- [ ] **Step 5: Commit**

```bash
git add include/threadpool.h src/threadpool.c tests/unit/test_threadpool.c
git commit -m "feat: add shared threadpool utility"
```

---

### Task 3: Expose port classification functions

**Files:**
- Modify: `include/port_scanner.h`
- Modify: `src/port_scanner.c`
- Test: `tests/unit/test_port_classification.c`

**Interfaces:**
- Produces: `int get_service_name(int port, char *service_name, size_t buffer_size)` (now public), `int is_port_suspicious(int port, const char *service_name)` (now public) — same signatures as the existing `static` functions, just no longer `static`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_port_classification.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "port_scanner.h"

int main(void) {
    char name[64];

    int is_common = get_service_name(22, name, sizeof(name));
    assert(strcmp(name, "SSH") == 0);
    assert(is_common == 1);
    assert(is_port_suspicious(22, name) == 0);

    get_service_name(53, name, sizeof(name));
    assert(strcmp(name, "DNS") == 0);
    assert(is_port_suspicious(53, name) == 0);

    get_service_name(110, name, sizeof(name));
    assert(strcmp(name, "POP3") == 0);

    get_service_name(143, name, sizeof(name));
    assert(strcmp(name, "IMAP") == 0);

    get_service_name(23, name, sizeof(name));
    assert(strcmp(name, "Telnet") == 0);
    assert(is_port_suspicious(23, name) == 1);

    get_service_name(31337, name, sizeof(name));
    assert(strcmp(name, "Elite/Backdoor") == 0);
    assert(is_port_suspicious(31337, name) == 1);

    int is_common_unknown = get_service_name(50000, name, sizeof(name));
    assert(strcmp(name, "Unknown") == 0);
    assert(is_common_unknown == 0);
    assert(is_port_suspicious(50000, name) == 1);  // puerto alto sin justificación

    printf("test_port_classification: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_port_classification tests/unit/test_port_classification.c src/port_scanner.c -lpthread`
Expected: FAIL — linker error, `get_service_name`/`is_port_suspicious` undefined (they're still `static` in `port_scanner.c`, invisible outside it)

- [ ] **Step 3: Expose the functions**

In `src/port_scanner.c`, change:

```c
static int get_service_name(int port, char *service_name, size_t buffer_size) {
```

to:

```c
int get_service_name(int port, char *service_name, size_t buffer_size) {
```

And change:

```c
static int is_port_suspicious(int port, const char *service_name) {
```

to:

```c
int is_port_suspicious(int port, const char *service_name) {
```

In `include/port_scanner.h`, add these two declarations after the existing `ServiceMapping` struct (before the "FUNCIONES PÚBLICAS DE ESCANEO DE PUERTOS" section):

```c
// ============================================================================
// CLASIFICACIÓN DE PUERTOS (antes privada de port_scanner.c)
// ============================================================================

/**
 * Obtiene el nombre del servicio asociado a un puerto.
 * @return 1 si es un servicio común/esperado, 0 si es desconocido.
 */
int get_service_name(int port, char *service_name, size_t buffer_size);

/**
 * Determina si un puerto es sospechoso según los mismos criterios que usa
 * el escaneo de puertos internamente.
 */
int is_port_suspicious(int port, const char *service_name);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_port_classification tests/unit/test_port_classification.c src/port_scanner.c -lpthread && ./tests/unit/test_port_classification`
Expected: PASS — prints `test_port_classification: OK`

- [ ] **Step 5: Rebuild the full project to confirm nothing broke**

Run (from WSL): `make clean && make 2>&1 | grep -i warning`
Expected: no output (zero warnings) — `gui_ports_integration.c` still has its own separate copy of this logic (untouched, unaffected by making `port_scanner.c`'s versions public; there's no name collision since the GUI file doesn't declare functions with these exact names).

- [ ] **Step 6: Commit**

```bash
git add include/port_scanner.h src/port_scanner.c tests/unit/test_port_classification.c
git commit -m "refactor: expose port_scanner's service/suspicion classification"
```

---

### Task 4: Threaded, progress-reporting port range scan

**Files:**
- Modify: `include/port_scanner.h`
- Modify: `src/port_scanner.c`
- Test: `tests/unit/test_port_scan_range.c`
- Create: `tests/unit/benchmark_port_scan.c`

**Interfaces:**
- Consumes: `ProgressUpdate`/`ProgressCallback` (Task 1), `ThreadPool`/`threadpool_create`/`threadpool_submit`/`threadpool_wait`/`threadpool_destroy` (Task 2), `get_port_service_name`/`is_port_suspicious` (Task 3 — renamed from `get_service_name` during Task 3's fix loop; see plan ledger)
- Produces: `int scan_ports_range(int start_port, int end_port, int num_threads, ProgressCallback cb, void *user_data, volatile sig_atomic_t *cancel, ScanResult *out)`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_port_scan_range.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "port_scanner.h"

int main(void) {
    ScanResult result;

    // Rango inválido: debe fallar sin tocar `result`.
    int rc = scan_ports_range(100, 1, 4, NULL, NULL, NULL, &result);
    assert(rc == -1);

    // Rango válido, sin callback ni cancelación.
    rc = scan_ports_range(1, 200, 4, NULL, NULL, NULL, &result);
    assert(rc == 0);
    assert(result.total_ports == 200);
    assert(result.open_ports >= 0);
    assert(result.suspicious_ports >= 0);
    assert(result.suspicious_ports <= result.open_ports);
    free(result.ports);

    printf("test_port_scan_range: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_port_scan_range tests/unit/test_port_scan_range.c src/port_scanner.c src/threadpool.c -lpthread`
Expected: FAIL — linker error, `scan_ports_range` undefined

- [ ] **Step 3: Write minimal implementation**

In `include/port_scanner.h`, add near the top (after the existing includes):

```c
#include <signal.h>       // sig_atomic_t
#include "progress.h"
#include "threadpool.h"
```

And add this declaration alongside the other public scan functions:

```c
/**
 * Escanea un rango de puertos usando `num_threads` hilos trabajadores.
 * `cb`/`user_data` son opcionales (pueden ser NULL) y se invocan a medida
 * que cada puerto termina de escanearse — pueden llegar desde cualquier
 * hilo trabajador. `cancel`, si no es NULL, permite pedir la interrupción
 * del escaneo desde otro hilo (se revisa entre puerto y puerto).
 * @return 0 si tuvo éxito, -1 si los parámetros son inválidos o falló la
 *         reserva de memoria/hilos.
 */
int scan_ports_range(int start_port, int end_port, int num_threads,
                      ProgressCallback cb, void *user_data,
                      volatile sig_atomic_t *cancel, ScanResult *out);
```

In `src/port_scanner.c`, add near the top:

```c
#include <pthread.h>
```

And add this new code after `is_port_suspicious()` and before `scan_port_range()`:

```c
// ============================================================================
// ESCANEO CONCURRENTE DE RANGOS DE PUERTOS
// ============================================================================

typedef struct {
    int start_port;
    int end_port;
    int next_port;             // próximo puerto a tomar; protegido por lock
    pthread_mutex_t lock;

    PortInfo *ports;            // un slot por puerto del rango
    int open_ports;              // protegido por lock
    int suspicious_ports;        // protegido por lock
    int ports_completed;         // protegido por lock

    ProgressCallback cb;
    void *user_data;
    volatile sig_atomic_t *cancel;
} PortScanJob;

static void scan_port_task(void *arg) {
    PortScanJob *job = (PortScanJob *)arg;

    for (;;) {
        if (job->cancel && *job->cancel) {
            return;
        }

        pthread_mutex_lock(&job->lock);
        if (job->next_port > job->end_port) {
            pthread_mutex_unlock(&job->lock);
            return;
        }
        int port = job->next_port++;
        pthread_mutex_unlock(&job->lock);

        int index = port - job->start_port;
        PortInfo *info = &job->ports[index];
        info->port = port;
        info->is_open = scan_single_port(port);
        info->is_suspicious = 0;
        info->service_name[0] = '\0';

        if (info->is_open) {
            get_port_service_name(port, info->service_name, sizeof(info->service_name));
            info->is_suspicious = is_port_suspicious(port, info->service_name);
        }

        pthread_mutex_lock(&job->lock);
        if (info->is_open) {
            job->open_ports++;
            if (info->is_suspicious) {
                job->suspicious_ports++;
            }
        }
        job->ports_completed++;
        int completed = job->ports_completed;
        pthread_mutex_unlock(&job->lock);

        if (job->cb) {
            ProgressUpdate update = {
                .phase = "Escaneando puertos",
                .current = completed,
                .total = job->end_port - job->start_port + 1
            };
            job->cb(&update, job->user_data);
        }
    }
}

int scan_ports_range(int start_port, int end_port, int num_threads,
                      ProgressCallback cb, void *user_data,
                      volatile sig_atomic_t *cancel, ScanResult *out) {
    if (!out || start_port < 1 || end_port > 65535 || start_port > end_port || num_threads < 1) {
        return -1;
    }

    int total_ports = end_port - start_port + 1;
    PortInfo *ports = malloc(total_ports * sizeof(PortInfo));
    if (!ports) {
        return -1;
    }

    PortScanJob job;
    job.start_port = start_port;
    job.end_port = end_port;
    job.next_port = start_port;
    pthread_mutex_init(&job.lock, NULL);
    job.ports = ports;
    job.open_ports = 0;
    job.suspicious_ports = 0;
    job.ports_completed = 0;
    job.cb = cb;
    job.user_data = user_data;
    job.cancel = cancel;

    ThreadPool *pool = threadpool_create(num_threads);
    if (!pool) {
        pthread_mutex_destroy(&job.lock);
        free(ports);
        return -1;
    }

    // Cada tarea recorre la cola compartida de puertos hasta agotarla —
    // alcanza con una tarea por hilo, no una por puerto.
    for (int i = 0; i < num_threads; i++) {
        threadpool_submit(pool, scan_port_task, &job);
    }

    threadpool_wait(pool);
    threadpool_destroy(pool);
    pthread_mutex_destroy(&job.lock);

    out->ports = ports;
    out->total_ports = total_ports;
    out->open_ports = job.open_ports;
    out->suspicious_ports = job.suspicious_ports;

    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_port_scan_range tests/unit/test_port_scan_range.c src/port_scanner.c src/threadpool.c -lpthread && ./tests/unit/test_port_scan_range`
Expected: PASS — prints `test_port_scan_range: OK`

- [ ] **Step 5: Write and run the timing benchmark**

Create `tests/unit/benchmark_port_scan.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "port_scanner.h"

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    ScanResult result;
    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    scan_ports_range(1, 20000, 1, NULL, NULL, NULL, &result);
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("1 hilo:   %.3fs (%d abiertos)\n", elapsed_seconds(start, end), result.open_ports);
    free(result.ports);

    clock_gettime(CLOCK_MONOTONIC, &start);
    scan_ports_range(1, 20000, 50, NULL, NULL, NULL, &result);
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("50 hilos: %.3fs (%d abiertos)\n", elapsed_seconds(start, end), result.open_ports);
    free(result.ports);

    return 0;
}
```

Run: `gcc -Wall -Wextra -std=c99 -O2 -Iinclude -o tests/unit/benchmark_port_scan tests/unit/benchmark_port_scan.c src/port_scanner.c src/threadpool.c -lpthread && ./tests/unit/benchmark_port_scan`
Expected: the "50 hilos" line reports a lower time than "1 hilo" (exact numbers depend on the machine — this is a manual sanity check, not an assertion, since absolute timing isn't portable across environments).

- [ ] **Step 6: Rebuild the full project to confirm nothing broke**

Run (from WSL): `make clean && make 2>&1 | grep -i warning`
Expected: no output.

- [ ] **Step 7: Commit**

```bash
git add include/port_scanner.h src/port_scanner.c tests/unit/test_port_scan_range.c tests/unit/benchmark_port_scan.c
git commit -m "feat: parallelize port range scanning with progress reporting"
```

---

### Task 5: USB device monitor — hash-skip and parallel hashing

**Files:**
- Modify: `include/device_monitor.h`
- Modify: `src/device_monitor.c`
- Test: `tests/unit/test_hash_skip.c`

**Interfaces:**
- Consumes: `ProgressUpdate`/`ProgressCallback` (Task 1), `ThreadPool`/`threadpool_submit`/`threadpool_wait` (Task 2)
- Produces: `int device_monitor_file_unchanged(const FileInfo *previous, off_t size, time_t mtime)`, `const FileInfo *device_monitor_find_file(const DeviceSnapshot *snapshot, const char *path)`, `int count_files_recursive(const char *dir_path)`, new signature for `scan_directory_recursive` (see below), `DeviceSnapshot *create_device_snapshot_ex(const char *device_name, const DeviceSnapshot *previous_snapshot, ThreadPool *hash_pool, ProgressCallback cb, void *user_data, volatile sig_atomic_t *cancel)`. `create_device_snapshot(const char *device_name)` keeps its exact current signature and behavior (calls the `_ex` version with `NULL`s).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_hash_skip.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "device_monitor.h"

int main(void) {
    FileInfo previous;
    previous.size = 1024;
    previous.last_modified = 1700000000;
    strcpy(previous.sha256_hash, "abc123");

    assert(device_monitor_file_unchanged(&previous, 1024, 1700000000) == 1);
    assert(device_monitor_file_unchanged(&previous, 2048, 1700000000) == 0);
    assert(device_monitor_file_unchanged(&previous, 1024, 1700000001) == 0);
    assert(device_monitor_file_unchanged(NULL, 1024, 1700000000) == 0);

    FileInfo file_a = previous;
    file_a.path = "/media/test/a.txt";
    FileInfo *files[1] = { &file_a };

    DeviceSnapshot snapshot;
    snapshot.device_name = "test";
    snapshot.files = files;
    snapshot.file_count = 1;
    snapshot.capacity = 1;

    const FileInfo *found = device_monitor_find_file(&snapshot, "/media/test/a.txt");
    assert(found == &file_a);

    const FileInfo *missing = device_monitor_find_file(&snapshot, "/media/test/b.txt");
    assert(missing == NULL);

    assert(device_monitor_find_file(NULL, "/media/test/a.txt") == NULL);

    printf("test_hash_skip: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_hash_skip tests/unit/test_hash_skip.c src/device_monitor.c src/threadpool.c -lcrypto -lpthread`
Expected: FAIL — linker error, `device_monitor_file_unchanged`/`device_monitor_find_file` undefined

- [ ] **Step 3: Write minimal implementation**

In `include/device_monitor.h`, add near the top (after the existing includes):

```c
#include <signal.h>       // sig_atomic_t
#include "progress.h"
#include "threadpool.h"
```

Replace the existing declaration:

```c
int scan_directory_recursive(DeviceSnapshot *snapshot, const char *dir_path);
```

with:

```c
/** Cuenta archivos regulares bajo dir_path (recursivo, sin calcular hashes). */
int count_files_recursive(const char *dir_path);

/**
 * true si `previous` describe exactamente el mismo tamaño y fecha de
 * modificación que los valores actuales — en ese caso no hace falta
 * recalcular el hash SHA-256. `previous` puede ser NULL (no hay registro
 * anterior, p.ej. archivo nuevo o primer snapshot del dispositivo).
 */
int device_monitor_file_unchanged(const FileInfo *previous, off_t size, time_t mtime);

/** Busca un archivo por ruta exacta dentro de un snapshot. NULL si no está o si snapshot es NULL. */
const FileInfo* device_monitor_find_file(const DeviceSnapshot *snapshot, const char *path);

/**
 * Recorre dir_path recursivamente y agrega cada archivo regular a
 * `snapshot`. Si `previous_snapshot` tiene un registro con el mismo path,
 * tamaño y fecha de modificación, reutiliza su hash en vez de
 * recalcularlo; si no, y `hash_pool` no es NULL, encola el cálculo del
 * hash en el pool (concurrente); si `hash_pool` es NULL, lo calcula en el
 * hilo actual. `total_files` (de `count_files_recursive`) y `cb` permiten
 * reportar progreso determinado; `cancel` permite interrumpir el recorrido.
 */
int scan_directory_recursive(DeviceSnapshot *snapshot, const char *dir_path,
                              const DeviceSnapshot *previous_snapshot,
                              int total_files,
                              ThreadPool *hash_pool,
                              ProgressCallback cb, void *user_data,
                              volatile sig_atomic_t *cancel);
```

And add this declaration next to `create_device_snapshot`:

```c
/**
 * Igual que create_device_snapshot(), pero permite reusar el hash de
 * archivos sin cambios (`previous_snapshot`, puede ser NULL), calcular
 * hashes en paralelo (`hash_pool`, puede ser NULL para calcular en el
 * hilo actual), reportar progreso (`cb`/`user_data`, pueden ser NULL) y
 * cancelar el escaneo (`cancel`, puede ser NULL).
 */
DeviceSnapshot* create_device_snapshot_ex(const char *device_name,
                                           const DeviceSnapshot *previous_snapshot,
                                           ThreadPool *hash_pool,
                                           ProgressCallback cb, void *user_data,
                                           volatile sig_atomic_t *cancel);
```

In `src/device_monitor.c`, add near the top:

```c
#include <pthread.h>
```

Add these new functions right before `scan_directory_recursive`'s current definition:

```c
int device_monitor_file_unchanged(const FileInfo *previous, off_t size, time_t mtime) {
    if (!previous) {
        return 0;
    }
    return previous->size == size && previous->last_modified == mtime;
}

const FileInfo* device_monitor_find_file(const DeviceSnapshot *snapshot, const char *path) {
    if (!snapshot || !path) {
        return NULL;
    }
    // Búsqueda lineal: el tamaño típico de un dispositivo (cientos/miles de
    // archivos) hace que esto sea mucho más barato que el hash SHA-256 que
    // reemplaza — no vale la pena una estructura más compleja acá.
    for (int i = 0; i < snapshot->file_count; i++) {
        if (snapshot->files[i] && snapshot->files[i]->path &&
            strcmp(snapshot->files[i]->path, path) == 0) {
            return snapshot->files[i];
        }
    }
    return NULL;
}

int count_files_recursive(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    struct stat file_stat;
    char full_path[1024];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        if (lstat(full_path, &file_stat) != 0) {
            continue;
        }
        if (S_ISLNK(file_stat.st_mode)) {
            continue;
        }

        if (S_ISDIR(file_stat.st_mode)) {
            count += count_files_recursive(full_path);
        } else if (S_ISREG(file_stat.st_mode)) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

typedef struct {
    char full_path[1024];
    FileInfo *file_info;
} HashTask;

static void hash_file_task(void *arg) {
    HashTask *task = (HashTask *)arg;
    if (calculate_sha256(task->full_path, task->file_info->sha256_hash) != 0) {
        strcpy(task->file_info->sha256_hash, "ERROR_CALCULATING_HASH");
    }
    free(task);
}
```

Replace the existing `scan_directory_recursive` function body with:

```c
int scan_directory_recursive(DeviceSnapshot *snapshot, const char *dir_path,
                              const DeviceSnapshot *previous_snapshot,
                              int total_files,
                              ThreadPool *hash_pool,
                              ProgressCallback cb, void *user_data,
                              volatile sig_atomic_t *cancel) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    struct stat file_stat;
    char full_path[1024];

    while ((entry = readdir(dir)) != NULL) {
        if (cancel && *cancel) {
            closedir(dir);
            return 0;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        if (lstat(full_path, &file_stat) != 0) {
            continue;
        }

        if (S_ISLNK(file_stat.st_mode)) {
            continue;
        } else if (S_ISDIR(file_stat.st_mode)) {
            scan_directory_recursive(snapshot, full_path, previous_snapshot,
                                      total_files, hash_pool, cb, user_data, cancel);
        } else if (S_ISREG(file_stat.st_mode)) {
            if (snapshot->file_count >= snapshot->capacity) {
                int new_capacity = snapshot->capacity * 2;
                FileInfo **temp = realloc(snapshot->files, new_capacity * sizeof(FileInfo*));
                if (!temp) {
                    perror("Error al expandir el array de archivos del snapshot");
                    closedir(dir);
                    return -1;
                }
                snapshot->files = temp;
                snapshot->capacity = new_capacity;
            }

            FileInfo *file_info = malloc(sizeof(FileInfo));
            if (!file_info) {
                perror("Error al asignar memoria para información de archivo");
                continue;
            }

            file_info->path = strdup(full_path);
            file_info->name = strdup(entry->d_name);
            file_info->extension = get_file_extension(entry->d_name);
            file_info->size = file_stat.st_size;
            file_info->permissions = file_stat.st_mode;
            file_info->last_modified = file_stat.st_mtime;
            file_info->last_accessed = file_stat.st_atime;

            const FileInfo *previous = device_monitor_find_file(previous_snapshot, full_path);
            if (device_monitor_file_unchanged(previous, file_info->size, file_info->last_modified)) {
                strncpy(file_info->sha256_hash, previous->sha256_hash, sizeof(file_info->sha256_hash) - 1);
                file_info->sha256_hash[sizeof(file_info->sha256_hash) - 1] = '\0';
            } else if (hash_pool) {
                file_info->sha256_hash[0] = '\0';  // se completa cuando la tarea corre en el pool
                HashTask *task = malloc(sizeof(HashTask));
                if (task) {
                    strncpy(task->full_path, full_path, sizeof(task->full_path) - 1);
                    task->full_path[sizeof(task->full_path) - 1] = '\0';
                    task->file_info = file_info;
                    threadpool_submit(hash_pool, hash_file_task, task);
                } else if (calculate_sha256(full_path, file_info->sha256_hash) != 0) {
                    strcpy(file_info->sha256_hash, "ERROR_CALCULATING_HASH");
                }
            } else if (calculate_sha256(full_path, file_info->sha256_hash) != 0) {
                strcpy(file_info->sha256_hash, "ERROR_CALCULATING_HASH");
            }

            snapshot->files[snapshot->file_count] = file_info;
            snapshot->file_count++;

            if (cb) {
                ProgressUpdate update = {
                    .phase = "Analizando archivos",
                    .current = snapshot->file_count,
                    .total = total_files
                };
                cb(&update, user_data);
            }
        }
    }

    closedir(dir);
    return 0;
}
```

Note: if `hash_pool` is non-NULL, the caller **must** call `threadpool_wait(hash_pool)` after this function returns and before reading any `FileInfo.sha256_hash` filled via the pool — `create_device_snapshot_ex` (next step) does exactly that.

Replace the existing `create_device_snapshot` function with:

```c
DeviceSnapshot* create_device_snapshot_ex(const char *device_name,
                                           const DeviceSnapshot *previous_snapshot,
                                           ThreadPool *hash_pool,
                                           ProgressCallback cb, void *user_data,
                                           volatile sig_atomic_t *cancel) {
    if (!device_name) {
        printf("Error: device_name es NULL\n");
        return NULL;
    }

    size_t name_len = strnlen(device_name, 256);
    if (name_len == 0 || name_len >= 256) {
        printf("Error: device_name inválido (longitud: %zu)\n", name_len);
        return NULL;
    }

    for (size_t i = 0; i < name_len; i++) {
        char c = device_name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            printf("Error: device_name contiene caracteres inválidos\n");
            return NULL;
        }
    }

    DeviceSnapshot *snapshot = malloc(sizeof(DeviceSnapshot));
    if (!snapshot) {
        printf("Error: No se pudo asignar memoria para snapshot\n");
        return NULL;
    }

    snapshot->device_name = malloc(name_len + 1);
    if (!snapshot->device_name) {
        printf("Error: No se pudo asignar memoria para device_name\n");
        free(snapshot);
        return NULL;
    }

    strncpy(snapshot->device_name, device_name, name_len);
    snapshot->device_name[name_len] = '\0';

    snapshot->files = malloc(100 * sizeof(FileInfo*));
    if (!snapshot->files) {
        printf("Error: No se pudo asignar memoria para files array\n");
        free(snapshot->device_name);
        free(snapshot);
        return NULL;
    }

    snapshot->file_count = 0;
    snapshot->capacity = 100;
    snapshot->snapshot_time = time(NULL);

    char device_path[512];
    snprintf(device_path, sizeof(device_path), "/media/%s", device_name);

    printf("Creando snapshot del dispositivo: %s\n", device_name);
    printf("Escaneando directorio: %s\n", device_path);

    int total_files = count_files_recursive(device_path);

    if (scan_directory_recursive(snapshot, device_path, previous_snapshot,
                                  total_files, hash_pool, cb, user_data, cancel) != 0) {
        printf("Error al escanear el dispositivo %s\n", device_name);
    }

    if (hash_pool) {
        threadpool_wait(hash_pool);
    }

    printf("Snapshot completado: %d archivos encontrados\n", snapshot->file_count);
    printf("---\n");

    return snapshot;
}

DeviceSnapshot* create_device_snapshot(const char *device_name) {
    return create_device_snapshot_ex(device_name, NULL, NULL, NULL, NULL, NULL);
}
```

This is the complete replacement for the current `create_device_snapshot` — the function ends at `return snapshot;` in both the old and new version; only the signature, the snapshot-building call, and the split into a thin `create_device_snapshot` wrapper are new.

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_hash_skip tests/unit/test_hash_skip.c src/device_monitor.c src/threadpool.c -lcrypto -lpthread && ./tests/unit/test_hash_skip`
Expected: PASS — prints `test_hash_skip: OK`

- [ ] **Step 5: Rebuild the full project to confirm nothing broke**

Run (from WSL): `make clean && make 2>&1 | grep -i warning`
Expected: no output. The four existing GUI call sites of `create_device_snapshot(device_name)` keep compiling and behaving exactly as before (they get `previous_snapshot = NULL`, `hash_pool = NULL` — same fully-synchronous behavior as today; the new hash-skip/parallel path only activates when sub-project 2 calls `create_device_snapshot_ex` with real arguments).

- [ ] **Step 6: Commit**

```bash
git add include/device_monitor.h src/device_monitor.c tests/unit/test_hash_skip.c
git commit -m "feat: skip re-hashing unchanged USB files, hash changed files in parallel"
```

---

### Task 6: Process monitor — status callback and condition-variable shutdown

**Files:**
- Modify: `include/process_monitor.h`
- Modify: `src/process_monitor.c`
- Test: `tests/unit/test_process_shutdown.c`

**Interfaces:**
- Consumes: `ProgressUpdate` (Task 1)
- Produces: new `ProcessCallbacks.on_status_update` field (`void (*)(const ProgressUpdate *update)`); `start_monitoring()`/`stop_monitoring()`/`cleanup_monitoring()` keep their exact current signatures — only their internal shutdown mechanism changes.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_process_shutdown.c`:

```c
// _POSIX_C_SOURCE se necesita para exponer clock_gettime()/CLOCK_MONOTONIC/
// struct timespec bajo `-std=c99` estricto (glibc los oculta si no se pide
// explícitamente soporte POSIX cuando se compila en modo ISO C99 puro) —
// ver el mismo fix aplicado en tests/unit/benchmark_port_scan.c (Task 4).
#define _POSIX_C_SOURCE 199309L
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "process_monitor.h"

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    load_config();

    int rc = start_monitoring();
    assert(rc == 0);

    sleep(1);  // dejar que el hilo entre en su primera espera

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    stop_monitoring();
    clock_gettime(CLOCK_MONOTONIC, &end);

    double shutdown_time = elapsed_seconds(start, end);
    printf("test_process_shutdown: apagado en %.3fs\n", shutdown_time);
    assert(shutdown_time < 0.5);

    cleanup_monitoring();

    printf("test_process_shutdown: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_process_shutdown tests/unit/test_process_shutdown.c src/process_monitor.c -lpthread && ./tests/unit/test_process_shutdown`
Expected: FAIL — the assertion `shutdown_time < 0.5` fails (current implementation takes close to 1s or more, since it polls every full second).

- [ ] **Step 3: Write minimal implementation**

In `include/process_monitor.h`, add near the top:

```c
#include "progress.h"
```

Add a new field to `ProcessCallbacks` (after `on_alert_cleared`):

```c
    void (*on_status_update)(const ProgressUpdate *update);
```

In `src/process_monitor.c`, add near the top:

```c
#include <errno.h>
```

Add a condition variable next to the existing `mutex`/`should_stop` globals:

```c
static pthread_cond_t stop_cond = PTHREAD_COND_INITIALIZER;
```

Replace `monitoring_thread_function`'s body:

```c
static void* monitoring_thread_function(void* arg) {
    (void)arg;

    pthread_mutex_lock(&mutex);
    while (!should_stop) {
        monitor_processes();
        int active_count = num_procesos_activos;

        if (should_stop) {
            break;
        }

        pthread_mutex_unlock(&mutex);
        if (event_callbacks && event_callbacks->on_status_update) {
            char phase[128];
            snprintf(phase, sizeof(phase), "Monitoreando... %d procesos activos", active_count);
            ProgressUpdate update = { .phase = phase, .current = 0, .total = 0 };
            event_callbacks->on_status_update(&update);
        }
        pthread_mutex_lock(&mutex);

        if (should_stop) {
            break;
        }

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += config.check_interval;

        while (!should_stop) {
            int rc = pthread_cond_timedwait(&stop_cond, &mutex, &deadline);
            if (rc == ETIMEDOUT) {
                break;
            }
        }
    }
    monitoring_active = 0;
    pthread_mutex_unlock(&mutex);

    pthread_exit(NULL);
    return NULL;
}
```

Replace `stop_monitoring`'s body (keep the doc comment above it as-is):

```c
int stop_monitoring(void) {
    pthread_mutex_lock(&mutex);

    if (!monitoring_active) {
        pthread_mutex_unlock(&mutex);
        printf("[INFO] El monitoreo no está activo\n");
        return 1;
    }

    should_stop = 1;
    pthread_cond_signal(&stop_cond);
    pthread_mutex_unlock(&mutex);

    printf("[INFO] Esperando terminación del hilo de monitoreo...\n");

    // El hilo ya recibió la señal y debería despertar de inmediato; sondeamos
    // cada 100ms (en vez de cada 1s como antes) para detectar que terminó lo
    // antes posible, con un tope defensivo de 3s por si quedó bloqueado
    // dentro de monitor_processes() y no llega a revisar should_stop.
    const int max_attempts = 30;  // 30 * 100ms = 3s
    for (int attempts = 0; attempts < max_attempts; attempts++) {
        pthread_mutex_lock(&mutex);
        int still_active = monitoring_active;
        pthread_mutex_unlock(&mutex);

        if (!still_active) {
            pthread_join(monitoring_thread, NULL);
            printf("[INFO] Hilo de monitoreo terminó\n");
            return 0;
        }

        struct timespec poll_interval = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&poll_interval, NULL);
    }

    pthread_mutex_lock(&mutex);
    monitoring_active = 0;
    pthread_mutex_unlock(&mutex);
    printf("[WARNING] Timeout al esperar terminación - marcando como inactivo\n");
    return -1;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -std=c99 -Iinclude -o tests/unit/test_process_shutdown tests/unit/test_process_shutdown.c src/process_monitor.c -lpthread && ./tests/unit/test_process_shutdown`
Expected: PASS — prints an apagado (shutdown) time well under 0.5s, then `test_process_shutdown: OK`

- [ ] **Step 5: Rebuild the full project to confirm nothing broke**

Run (from WSL): `make clean && make 2>&1 | grep -i warning`
Expected: no output. `gui_process_integration.c`'s `static ProcessCallbacks backend_callbacks;` is zero-initialized, so the new `on_status_update` field is `NULL` there until sub-project 2 sets it — no behavior change for the existing GUI.

- [ ] **Step 6: Commit**

```bash
git add include/process_monitor.h src/process_monitor.c tests/unit/test_process_shutdown.c
git commit -m "perf: replace polling shutdown with condition-variable wakeup in process monitor"
```

---

### Task 7: Wire the Makefile and do a full verification pass

**Files:**
- Modify: `Makefile`

**Interfaces:**
- Consumes: everything from Tasks 1–6

- [x] **Step 1: Add `threadpool.c` to the main build — already done in Task 4**

Task 4 discovered that the main build fails to link as soon as `port_scanner.c` calls into the thread pool (undefined references to `threadpool_create`/`submit`/`wait`/`destroy`), so it added `src/threadpool.c` to `SRC` itself rather than leaving the main build broken until this task. Confirm it's there (it should already read exactly like this) and do NOT add it again:

```makefile
SRC = src/main.c \
		src/port_scanner.c \
		src/threadpool.c \
		src/process_monitor.c \
		src/device_monitor.c \
		src/gui/gui_main.c \
```
(the rest of `SRC` stays exactly as it is today)

- [ ] **Step 2: Add unit-test targets**

Add this block to the `Makefile`, after the existing `check-deps` target:

```makefile
UNIT_CFLAGS = -Wall -Wextra -std=c99 -g -Iinclude

test-progress: tests/unit/test_progress.c include/progress.h
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_progress tests/unit/test_progress.c
	./tests/unit/test_progress

test-threadpool: tests/unit/test_threadpool.c src/threadpool.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_threadpool tests/unit/test_threadpool.c src/threadpool.c -lpthread
	./tests/unit/test_threadpool

test-port-classification: tests/unit/test_port_classification.c src/port_scanner.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_port_classification tests/unit/test_port_classification.c src/port_scanner.c src/threadpool.c -lpthread
	./tests/unit/test_port_classification

test-port-scan-range: tests/unit/test_port_scan_range.c src/port_scanner.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_port_scan_range tests/unit/test_port_scan_range.c src/port_scanner.c src/threadpool.c -lpthread
	./tests/unit/test_port_scan_range

test-hash-skip: tests/unit/test_hash_skip.c src/device_monitor.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_hash_skip tests/unit/test_hash_skip.c src/device_monitor.c src/threadpool.c -lcrypto -lpthread
	./tests/unit/test_hash_skip

test-process-shutdown: tests/unit/test_process_shutdown.c src/process_monitor.c
	$(CC) $(UNIT_CFLAGS) -o tests/unit/test_process_shutdown tests/unit/test_process_shutdown.c src/process_monitor.c -lpthread
	./tests/unit/test_process_shutdown

test-unit: test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown

benchmark-port-scan: tests/unit/benchmark_port_scan.c src/port_scanner.c
	$(CC) $(UNIT_CFLAGS) -O2 -o tests/unit/benchmark_port_scan tests/unit/benchmark_port_scan.c src/port_scanner.c src/threadpool.c -lpthread
	./tests/unit/benchmark_port_scan

.PHONY: test-unit test-progress test-threadpool test-port-classification test-port-scan-range test-hash-skip test-process-shutdown benchmark-port-scan
```

- [ ] **Step 3: Run the full unit-test suite**

Run (from WSL): `make test-unit`
Expected: all six `OK` lines print, no failed assertion, no compiler warnings in the build output.

- [ ] **Step 4: Full clean rebuild of the real application**

Run (from WSL): `make clean && make 2>&1 | tee /tmp/build.log; grep -i warning /tmp/build.log; echo "warnings: $?"`
Expected: `warnings: 1` (grep found nothing — zero warnings).

- [ ] **Step 5: Manual smoke test of the built binary**

Run (from WSL): `timeout 6 ./matcom-guard 2>&1 | head -40`
Expected: the same startup sequence seen throughout this session (module initialization messages, no crash) — this confirms the rewritten backend still links and runs correctly inside the unchanged GUI, since nothing wired the new capabilities into it yet.

- [ ] **Step 6: Commit and open the PR**

```bash
git add Makefile
git commit -m "build: wire threadpool.c and unit-test targets into the Makefile"
git push -u origin rewrite/backend
```

Open a PR from `rewrite/backend` into `main` titled "Backend rewrite: shared progress/threadpool contract" summarizing: parallelized port scanning, USB hash-skip, unified progress reporting, de-duplicated port classification, faster process-monitor shutdown — and noting explicitly that no GUI file changed, so `main`'s current behavior is unaffected until sub-project 2 wires the GUI to these new entry points.

---

## Self-Review Notes

- **Spec coverage:** every "Goals" bullet in the spec maps to a task — shared progress contract (Task 1), shared thread pool (Task 2), parallel port scanning (Task 4), hash-skip (Task 5), exposed classification functions (Task 3), condition-variable process shutdown (Task 6), unit tests (Tasks 1–6, each has one), Makefile wiring (Task 7). The two "Open items for sub-project 2" in the spec are intentionally not tasks here — they're GUI decisions for the next sub-project.
- **No GUI files appear in any task's Files list** — matches the spec's scope line and the Global Constraints.
- **Signature consistency check:** `scan_ports_range`, `create_device_snapshot_ex`, `device_monitor_file_unchanged`, `device_monitor_find_file`, `count_files_recursive`, and the `ProcessCallbacks.on_status_update` field are each defined once (in the task that introduces them) and referenced with identical signatures everywhere else they're used across later tasks.
