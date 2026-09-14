#ifndef DEVICE_MONITOR_H
#define DEVICE_MONITOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>       // sig_atomic_t
#include "progress.h"
#include "threadpool.h"

// Estructura para almacenar información de dispositivos conectados
typedef struct {
    char **devices;     // Array de nombres de dispositivos
    int count;          // Número de dispositivos encontrados
    int capacity;       // Capacidad actual del array
} DeviceList;

// Estructura para almacenar información de un archivo
typedef struct {
    char *path;                 // Ruta completa del archivo
    char *name;                 // Nombre del archivo
    char *extension;            // Extensión del archivo
    off_t size;                 // Tamaño del archivo
    char sha256_hash[65];       // Hash SHA-256 (64 caracteres + null terminator)
    mode_t permissions;         // Permisos del archivo
    time_t last_modified;       // Última modificación
    time_t last_accessed;       // Último acceso
} FileInfo;

// Estructura para almacenar el snapshot de un dispositivo
typedef struct {
    char *device_name;          // Nombre del dispositivo
    FileInfo **files;           // Array de archivos
    int file_count;             // Número de archivos
    int capacity;               // Capacidad del array
    time_t snapshot_time;       // Tiempo del snapshot
} DeviceSnapshot;

// Funciones para monitoreo de dispositivos
DeviceList* monitor_connected_devices(void);
void free_device_list(DeviceList *device_list);

// Funciones para manejo de archivos y snapshots
int calculate_sha256(const char *filepath, char *hash_output);
char* get_file_extension(const char *filename);
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
 * Si `hash_pool` no es NULL, los campos `sha256_hash` de los archivos con
 * hash encolado NO son válidos hasta que el llamador invoque
 * `threadpool_wait(hash_pool)` -- `create_device_snapshot_ex` ya hace esto
 * internamente antes de devolver el snapshot.
 */
int scan_directory_recursive(DeviceSnapshot *snapshot, const char *dir_path,
                              const DeviceSnapshot *previous_snapshot,
                              int total_files,
                              ThreadPool *hash_pool,
                              ProgressCallback cb, void *user_data,
                              volatile sig_atomic_t *cancel);
DeviceSnapshot* create_device_snapshot(const char *device_name);

/**
 * Igual que create_device_snapshot(), pero permite reusar el hash de
 * archivos sin cambios (`previous_snapshot`, puede ser NULL), calcular
 * hashes en paralelo (`hash_pool`, puede ser NULL para calcular en el
 * hilo actual), reportar progreso (`cb`/`user_data`, pueden ser NULL) y
 * cancelar el escaneo (`cancel`, puede ser NULL).
 *
 * `was_cancelled` (puede ser NULL): si no es NULL, se pone en 1 si el
 * escaneo fue interrumpido — en ese caso el snapshot resultante está
 * incompleto y NO debe usarse para comparar contra un snapshot anterior
 * (p. ej. con detect_usb_changes), ya que los archivos no alcanzados a
 * escanear se leerían incorrectamente como eliminados.
 */
DeviceSnapshot* create_device_snapshot_ex(const char *device_name,
                                           const DeviceSnapshot *previous_snapshot,
                                           ThreadPool *hash_pool,
                                           ProgressCallback cb, void *user_data,
                                           volatile sig_atomic_t *cancel,
                                           int *was_cancelled);
void free_device_snapshot(DeviceSnapshot *snapshot);

#endif // DEVICE_MONITOR_H