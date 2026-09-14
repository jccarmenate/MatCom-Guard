# MatCom Guard — Backend Rewrite (Sub-project 1 of 3)

**Status:** Approved by user, pending spec review
**Date:** 2026-09-12
**Scope:** `src/device_monitor.c`, `src/process_monitor.c`, `src/port_scanner.c` and their headers, plus one new shared utility (`threadpool.c/h`). No GUI files change in this sub-project.

## Context

MatCom Guard is a GTK+3/C system-monitoring tool (USB, process, and port
monitoring) built for an Operating Systems course, now being prepared for a
portfolio. This is the first of three sequential sub-projects rewriting the
application:

1. **Backend rewrite** (this spec) — device monitor, process monitor, port
   scanner.
2. **Frontend rewrite** — full GTK GUI redesign ("Night Watch" dark theme,
   custom progress-dial widget, icon-rail navigation), consuming this
   sub-project's new progress/cancellation contract.
3. **Polish** — updated test scripts, docs, README screenshots.

Each sub-project gets its own design → plan → implementation cycle.

### Why rewrite instead of patch

Live testing this session (full port scan, USB inspection, code reading)
surfaced concrete, verified issues:

- The port scanner already tracks scan progress (`ports_completed`,
  `total_ports_to_scan`, `last_progress_percentage` in
  `gui_ports_integration.c`) but never surfaces it in the GUI — it's only
  logged every 100 ports. USB deep-scan has no progress tracking at all.
- `scan_single_port()` scans strictly sequentially. `PortScanConfig` already
  has a `concurrent_scans` field — it's set but never used.
- `device_monitor.c`'s `scan_directory_recursive()` recomputes the SHA-256
  hash of every file on every scan, even files that haven't changed since
  the last snapshot.
- `gui_ports_integration.c`'s scan thread duplicates `port_scanner.c`'s
  service-name/suspicious-port classification instead of calling it
  (comment: *"como no tenemos acceso directo a get_service_name, usamos
  lógica similar"*) — the copy has already drifted (missing POP3/IMAP/DNS
  handling that the original has).

The user's own stated goal for choosing a full rewrite over patching: **a
single clean codebase is easier to reason about and to explain in a
portfolio review than working code with successive patches layered on.**
This spec optimizes for that: one consistent progress/cancellation contract
used everywhere, one shared thread-pool implementation instead of ad hoc
threading per module, no duplicated logic between backend and integration
layers.

### Key risk

This rewrite touches code that had thread-safety races, missing NULL
checks, and memory-leak issues fixed only one session ago (commit
`2b01d8a`, "fix: thread-safety, NULL checks, and dead security-stats
fields"). **Every one of those fixed defect classes must not reappear.**
Section "Error handling" below restates the specific invariants that must
hold, and the implementation plan must verify each one explicitly rather
than assuming the rewrite inherits the old file's fixes.

## Goals

- Rewrite `device_monitor.c`, `process_monitor.c`, `port_scanner.c` (and
  headers) from scratch, preserving their current external behavior and
  public API shape (same feature set: USB snapshot/diff/threat heuristics,
  process CPU/RAM monitoring with whitelist and alerts, TCP port scanning
  with service/suspicion detection).
- Add a single shared progress-reporting contract
  (`ProgressUpdate`/`ProgressCallback`) used by all three modules for any
  operation that can take a perceptible amount of time.
- Add a single shared cancellation contract (`volatile sig_atomic_t *`)
  replacing the mutex-guarded `should_stop_scan` pattern.
- Add a shared `threadpool.c/h` utility, used by both the port scanner
  (parallel `connect()` attempts) and the USB device monitor (parallel
  SHA-256 hashing of changed files).
- Parallelize port scanning across a configurable number of worker threads.
- Skip re-hashing files whose `(size, mtime)` are unchanged since the
  device's last recorded snapshot.
- Expose `get_service_name()`/`is_port_suspicious()` publicly so the
  integration layer has one source of truth instead of a duplicate copy.
- Replace the process monitor's `sleep(1)`-and-check-flag shutdown loop
  with `pthread_cond_timedwait`, for immediate shutdown response.
- Add small `assert()`-based unit tests for the logic that's testable in
  isolation (port classification table, thread pool, hash-skip decision).

## Non-goals

- No GUI changes (sub-project 2).
- No change to the target of the port scanner — it stays `127.0.0.1` only.
  This is a local system guard, not a network scanner; making the target
  configurable is out of scope and not something the user asked for.
- No change to `matcomguard.conf`'s format or to the on-disk config
  loading/saving behavior.
- No change to the USB threat-heuristic thresholds (mass deletion >10%,
  mass modification >20%, etc.) — only how the underlying snapshot data is
  computed, not the analysis built on top of it.
- No dependency changes (still GTK+3/Cairo/OpenSSL/libudev/pthreads, still
  C99, still built by the existing `Makefile` pattern — new source files
  get added to `SRC`, nothing else about the build changes).

## Architecture

Unchanged 3-layer shape (Presentation / Integration / Backend) — the
layering itself isn't the problem, so it isn't being redesigned. This
sub-project only rewrites the Backend layer's three modules plus adds one
new shared utility they both depend on.

### Shared progress contract

```c
// include/progress.h (new)
typedef struct {
    const char *phase;   // e.g. "Escaneando puertos altos", "Calculando hashes..."
    int current;
    int total;            // 0 = indeterminate
} ProgressUpdate;

typedef void (*ProgressCallback)(const ProgressUpdate *update, void *user_data);
```

Backend functions that can take a perceptible amount of time accept an
optional `ProgressCallback cb, void *user_data` pair (either may be `NULL`
to opt out). The backend calls `cb` directly — a push model — instead of
writing to a shared struct that the caller must poll. **The callback may be
invoked from any worker thread.** Documented contract: a caller that needs
to touch GTK from the callback is responsible for marshaling onto the main
thread (e.g. via `g_idle_add`) — this keeps the backend free of any GTK
dependency, preserving the existing layer separation.

### Shared cancellation contract

```c
volatile sig_atomic_t *cancel_requested;
```

A single flag, checked by worker loops between units of work (one port,
one file). Replaces the port scanner's current
`pthread_mutex`-guarded `should_stop_scan` int — a plain `sig_atomic_t`
read needs no lock for a single boolean flag, and this removes a
lock/unlock pair from the hot loop.

### Shared thread pool

```c
// include/threadpool.h (new)
typedef struct ThreadPool ThreadPool;

ThreadPool *threadpool_create(int num_threads);
void threadpool_submit(ThreadPool *pool, void (*task)(void *arg), void *arg);
void threadpool_wait(ThreadPool *pool);   // blocks until all submitted tasks finish
void threadpool_destroy(ThreadPool *pool);
```

One tested implementation (bounded worker threads pulling from a shared
task queue behind a mutex + condition variable), used by:
- The port scanner, to run `scan_single_port()` concurrently across a
  range.
- The device monitor, to run `calculate_sha256()` concurrently across the
  files that need re-hashing.

## Per-module design

### Port scanner (`src/port_scanner.c`, `include/port_scanner.h`)

```c
int scan_ports_range(int start_port, int end_port, int num_threads,
                      ProgressCallback cb, void *user_data,
                      volatile sig_atomic_t *cancel, ScanResult *out);
```

A shared `next_port` counter, protected by a small mutex (lock, read,
increment, unlock — nothing else in the critical section), lets
`num_threads` workers each pull the next unscanned port and call the
existing `scan_single_port()` connect-with-timeout logic unchanged — that
part already works correctly. Each worker reports its own completion via
the progress callback after finishing a port. `get_service_name()` and
`is_port_suspicious()` move from `static` to public functions declared in
`port_scanner.h`, so `gui_ports_integration.c` calls them directly instead
of maintaining its own copy.

`scan_specific_port()` (single-port scan, used by the "scan one port" path)
and `scan_ports()`/`scan_common_ports()` (the simple CLI-style wrappers)
keep their current signatures — only the range-scanning path used by the
GUI's Quick/Full scan gains threading and progress reporting.

The scan target stays hardcoded to `127.0.0.1` (see Non-goals).

`num_threads` is caller-supplied, not hardcoded inside the scanner — for
this sub-project's own manual verification and unit tests (before
sub-project 2's GUI exists to supply a value), a constant default of 50 is
used. Whether the GUI later exposes this as a setting or keeps a fixed
constant is a sub-project 2 decision (see "Open items" below).

### Device monitor (`src/device_monitor.c`, `include/device_monitor.h`)

New signature:

```c
int scan_directory_recursive(DeviceSnapshot *snapshot, const char *dir_path,
                              const DeviceSnapshot *previous_snapshot, // nullable
                              ThreadPool *hash_pool,
                              ProgressCallback cb, void *user_data,
                              volatile sig_atomic_t *cancel);
```

Two changes from the current implementation:

1. **Pre-count pass**: before hashing, walk the tree once with
   `readdir()`/`lstat()` only (no hashing) to get a total file count, so
   progress can be reported as a determinate `current/total` rather than
   an indeterminate counter. This doubles directory-walking I/O
   (`readdir`/`lstat` are cheap; this is a deliberate trade — accurate
   progress for slightly more I/O, not an oversight), but hashing is
   unaffected since only the files that need it get hashed.
2. **Hash-skip fast path**: for each file, look up `previous_snapshot`'s
   `FileInfo` for that exact path (`previous_snapshot` is `NULL` for the
   very first snapshot of a device, or non-`NULL` when re-scanning against
   an existing baseline — e.g. Deep Scan comparing against the last
   Refresh). If `st_size` and `st_mtime` both match the previous record
   exactly, copy the previous hash instead of calling
   `calculate_sha256()` again. A file with no match in `previous_snapshot`
   (new file, or `previous_snapshot` itself is `NULL`) is always hashed.

Files that do need a fresh hash are submitted to `hash_pool` (the shared
thread pool) so hashing happens concurrently rather than one file at a
time; passing `hash_pool` in (rather than creating one internally) lets
the caller size and reuse one pool across a whole multi-device scan
instead of spinning threads up per directory. The existing symlink-skip
(`lstat`, `S_ISLNK`) and malloc/realloc-failure handling from the current
implementation are preserved as-is — that logic is already correct (fixed
in `2b01d8a`) and isn't part of what's being changed.

### Process monitor (`src/process_monitor.c`, `include/process_monitor.h`)

Functionally unchanged (poll `/proc` at the configured interval, track
CPU/RAM deltas, whitelist matching, threshold alerts, cleanup of processes
that disappeared). Two changes:

1. Adopts the same `ProgressCallback` contract for status reporting. Since
   this is continuous monitoring rather than a one-shot scan, most updates
   are indeterminate (`total = 0`) with a phase string like "Monitoreando…
   42 procesos activos" — this exists so the GUI has one consistent
   mechanism for "what's happening right now" across all three modules,
   not just the on-demand scans.
2. The monitoring thread's shutdown wait changes from a `sleep(1)` +
   flag-check loop to `pthread_cond_timedwait()` on a condition variable
   signaled by `stop_monitoring()` — shutdown becomes immediate instead of
   taking up to one second, and there's no periodic wake-up needed just to
   check a flag.

## Data flow

Each backend module keeps its existing init/start/stop/cleanup lifecycle
shape. The integration layer (unchanged in this sub-project, since it's
GUI-adjacent — sub-project 2 rewrites it) is the boundary that:

- Passes a `ProgressCallback` into the new backend entry points.
- Its callback implementation copies the `ProgressUpdate` onto the heap and
  calls `g_idle_add()` to marshal the GTK update onto the main thread, then
  frees the copy once applied.
- Passes a `volatile sig_atomic_t *cancel` that a "Cancel" button in the
  GUI sets to `1`.

This is a placeholder note for sub-project 2's design, not something this
sub-project implements — it's recorded here so the contract this sub-project
ships is verified against how it'll actually be consumed.

## Error handling

Non-negotiable invariants, carried over from this session's thread-safety
fixes and restated explicitly so the rewrite doesn't quietly drop them:

- Every `malloc`/`realloc` return value is checked before use; on failure,
  the caller unwinds cleanly (frees what it already allocated) instead of
  dereferencing NULL.
- All state shared between threads is protected by a mutex for every
  access, not just the ones that "looked like they needed it."
- `scan_directory_recursive()` uses `lstat` (not `stat`) and skips
  symlinks, to avoid infinite recursion on a symlink cycle.
- If `threadpool_create()` gets a `pthread_create` failure partway through
  spinning up workers, it degrades to the workers that did start rather
  than aborting the whole operation.
- A single file's hashing failure (`calculate_sha256()` returning an
  error) marks that file's hash as an error sentinel and the scan
  continues — one bad file doesn't abort an entire device snapshot.

## Testing

No test framework exists in this repo; new tests follow the project's
existing lightweight style (small `assert()`-based C programs, compiled by
the Makefile) rather than introducing one. New unit tests cover the logic
that's testable without GTK or real hardware:

- `get_service_name()` / `is_port_suspicious()` against the known service
  table (including the previously-missing POP3/IMAP/DNS cases that had
  drifted out of sync).
- `threadpool.c`: submit N tasks, confirm all run exactly once,
  `threadpool_wait()` blocks until completion, `threadpool_destroy()`
  shuts down cleanly with no leaked threads.
- The hash-skip decision: given two `FileInfo` records with matching
  `(size, mtime)` → skip; with either differing → re-hash.

`tests/*.sh` (the existing manual/interactive USB, process, and port test
scripts) are left as-is in this sub-project and get updated in sub-project
3, once the GUI they drive has also been rewritten.

Manual verification for this sub-project: rebuild under WSL (as done
earlier this session), confirm the build stays at zero warnings, and
compare full-port-scan wall-clock time before/after threading.

## Migration strategy

This rewrite touches code currently on `main`, which is already pushed and
presented as portfolio-ready. Work happens on a dedicated branch
(`rewrite/backend`) or an isolated worktree, merged to `main` only once
this sub-project builds clean and passes its manual verification — `main`
stays in a working state for the whole duration of the rewrite.

## Open items for sub-project 2 (frontend), not decided here

- Exact number of worker threads for the port-scan pool (a GUI-configurable
  value, a fixed constant, or derived from range size) — the frontend
  design will pick this since it's the one exposing any such control.
- How the progress dial widget consumes indeterminate (`total = 0`)
  updates from the process monitor differently from determinate ones from
  port/USB scans.
