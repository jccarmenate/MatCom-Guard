#define _GNU_SOURCE  // Para strdup y strnlen
#include <pthread.h>
#include <device_monitor.h>

// ============================================================================
// FUNCIONES DE MONITOREO DE DISPOSITIVOS
// ============================================================================

/**
 * Función que monitorea periódicamente el directorio /media para detectar
 * nuevos dispositivos conectados (puertas de entrada del reino)
 * 
 * @param interval_seconds: Intervalo en segundos entre cada verificación
 * @return DeviceList*: Puntero a estructura con lista de dispositivos conectados
 */
DeviceList* monitor_connected_devices(void) {
    // Inicializar la estructura de lista de dispositivos
    DeviceList *device_list = malloc(sizeof(DeviceList));
    if (!device_list) {
        perror("Error al asignar memoria para la lista de dispositivos");
        return NULL;
    }
    device_list->devices = malloc(10 * sizeof(char*)); // Capacidad inicial de 10 dispositivos
    if (!device_list->devices) {
        perror("Error al asignar memoria para el array de dispositivos");
        free(device_list);
        return NULL;
    }
    device_list->count = 0;
    device_list->capacity = 10;
    
    // Directorio típico donde se montan dispositivos USB en Linux
    const char *mount_dir = "/media";
    DIR *dir;
    struct dirent *entry;
    struct stat statbuf;
    char full_path[512];
    
    printf("Iniciando monitoreo de dispositivos conectados...\n");
    
    // Limpiar la lista anterior de dispositivos
        for (int i = 0; i < device_list->count; i++) {
            free(device_list->devices[i]);
        }
        device_list->count = 0;
        
        // Abrir el directorio de montaje
        dir = opendir(mount_dir);
        if (dir == NULL) {
            perror("Error al abrir directorio de montaje");
            return NULL;
        }
        
        // Leer todas las entradas del directorio
        while ((entry = readdir(dir)) != NULL) {
            // Saltar directorios especiales . y ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            // Construir la ruta completa del dispositivo
            snprintf(full_path, sizeof(full_path), "%s/%s", mount_dir, entry->d_name);
            
            // Verificar si es un directorio (dispositivo montado)
            if (stat(full_path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
                // Verificar si necesitamos expandir el array
                if (device_list->count >= device_list->capacity) {
                    int new_capacity = device_list->capacity * 2;
                    char **temp = realloc(device_list->devices, new_capacity * sizeof(char*));
                    if (!temp) {
                        perror("Error al expandir la lista de dispositivos");
                        break; // Conservar los dispositivos ya detectados
                    }
                    device_list->devices = temp;
                    device_list->capacity = new_capacity;
                }

                // Agregar el dispositivo a la lista
                device_list->devices[device_list->count] = malloc(strlen(entry->d_name) + 1);
                if (!device_list->devices[device_list->count]) {
                    perror("Error al asignar memoria para nombre de dispositivo");
                    continue;
                }
                strcpy(device_list->devices[device_list->count], entry->d_name);
                device_list->count++;
                
                printf("Dispositivo detectado: %s (Ruta: %s)\n", entry->d_name, full_path);
            }
        }
        
        // Cerrar el directorio
        closedir(dir);
        
        // Mostrar resumen de dispositivos conectados
        printf("Total de dispositivos conectados: %d\n", device_list->count);
        for (int i = 0; i < device_list->count; i++) {
            printf("  - %s\n", device_list->devices[i]);
        }
        printf("---\n");
    
    return device_list;
}

/**
 * Función para liberar la memoria de la lista de dispositivos
 * 
 * @param device_list: Puntero a la estructura DeviceList a liberar
 */
void free_device_list(DeviceList *device_list) {
    if (device_list != NULL) {
        // Liberar cada nombre de dispositivo
        for (int i = 0; i < device_list->count; i++) {
            free(device_list->devices[i]);
        }
        // Liberar el array de dispositivos
        free(device_list->devices);
        // Liberar la estructura principal
        free(device_list);
    }
}

// ============================================================================
// FUNCIONES DE MANEJO DE ARCHIVOS Y SNAPSHOTS
// ============================================================================

/**
 * Calcula el hash SHA-256 de un archivo
 * 
 * @param filepath: Ruta del archivo
 * @param hash_output: Buffer donde se almacenará el hash (debe ser de al menos 65 bytes)
 * @return int: 0 si es exitoso, -1 si hay error
 */
int calculate_sha256(const char *filepath, char *hash_output) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        return -1;
    }
    
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        fclose(file);
        return -1;
    }
    
    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(mdctx);
        fclose(file);
        return -1;
    }
    
    unsigned char buffer[8192];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (EVP_DigestUpdate(mdctx, buffer, bytes_read) != 1) {
            EVP_MD_CTX_free(mdctx);
            fclose(file);
            return -1;
        }
    }
    
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    
    if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        fclose(file);
        return -1;
    }
    
    // Convertir a string hexadecimal
    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(hash_output + (i * 2), "%02x", hash[i]);
    }
    hash_output[hash_len * 2] = '\0';
    
    EVP_MD_CTX_free(mdctx);
    fclose(file);
    return 0;
}

/**
 * Obtiene la extensión de un archivo
 * 
 * @param filename: Nombre del archivo
 * @return char*: Extensión del archivo (debe ser liberada)
 */
char* get_file_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return strdup(""); // Sin extensión
    }
    return strdup(dot + 1);
}

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

/**
 * Escanea recursivamente un directorio y almacena información de archivos
 *
 * @param snapshot: Puntero al snapshot donde almacenar la información
 * @param dir_path: Ruta del directorio a escanear
 * @return int: 0 si es exitoso, -1 si hay error
 */
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

        // lstat (no stat) para no seguir symlinks: un enlace simbólico que
        // apunte a un ancestro del propio árbol causaría recursión infinita
        if (lstat(full_path, &file_stat) != 0) {
            continue; // Error al obtener información del archivo
        }

        if (S_ISLNK(file_stat.st_mode)) {
            continue; // No seguir enlaces simbólicos
        } else if (S_ISDIR(file_stat.st_mode)) {
            // Es un directorio, escanear recursivamente
            scan_directory_recursive(snapshot, full_path, previous_snapshot,
                                      total_files, hash_pool, cb, user_data, cancel);
        } else if (S_ISREG(file_stat.st_mode)) {
            // Es un archivo regular, almacenar información

            // Verificar si necesitamos expandir el array
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

            // Crear nueva entrada de archivo
            FileInfo *file_info = malloc(sizeof(FileInfo));
            if (!file_info) {
                perror("Error al asignar memoria para información de archivo");
                continue;
            }

            // Almacenar información básica
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
                    if (threadpool_submit(hash_pool, hash_file_task, task) != 0) {
                        // El pool no pudo encolar la tarea (p.ej. fallo de
                        // malloc interno): liberar y calcular el hash aquí
                        // mismo para no dejar sha256_hash vacío para siempre.
                        free(task);
                        if (calculate_sha256(full_path, file_info->sha256_hash) != 0) {
                            strcpy(file_info->sha256_hash, "ERROR_CALCULATING_HASH");
                        }
                    }
                } else if (calculate_sha256(full_path, file_info->sha256_hash) != 0) {
                    strcpy(file_info->sha256_hash, "ERROR_CALCULATING_HASH");
                }
            } else if (calculate_sha256(full_path, file_info->sha256_hash) != 0) {
                strcpy(file_info->sha256_hash, "ERROR_CALCULATING_HASH");
            }

            // Agregar a la lista
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

/**
 * Crea un snapshot completo de un dispositivo
 * 
 * @param device_name: Nombre del dispositivo
 * @return DeviceSnapshot*: Puntero al snapshot creado
 */
DeviceSnapshot* create_device_snapshot_ex(const char *device_name,
                                           const DeviceSnapshot *previous_snapshot,
                                           ThreadPool *hash_pool,
                                           ProgressCallback cb, void *user_data,
                                           volatile sig_atomic_t *cancel,
                                           int *was_cancelled) {
    if (was_cancelled) {
        *was_cancelled = 0;
    }

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

    if (was_cancelled && cancel && *cancel) {
        *was_cancelled = 1;
    }

    if (hash_pool) {
        threadpool_wait(hash_pool);
    }

    printf("Snapshot completado: %d archivos encontrados\n", snapshot->file_count);
    printf("---\n");

    return snapshot;
}

DeviceSnapshot* create_device_snapshot(const char *device_name) {
    return create_device_snapshot_ex(device_name, NULL, NULL, NULL, NULL, NULL, NULL);
}

/**
 * Libera la memoria de un snapshot de dispositivo
 * 
 * @param snapshot: Puntero al snapshot a liberar
 */
void free_device_snapshot(DeviceSnapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }

    // Liberar información de cada archivo de forma segura
    if (snapshot->files != NULL && snapshot->file_count > 0) {
        for (int i = 0; i < snapshot->file_count; i++) {
            FileInfo *file = snapshot->files[i];
            if (file != NULL) {
                // Verificar punteros antes de liberar
                if (file->path != NULL) {
                    free(file->path);
                    file->path = NULL;
                }
                if (file->name != NULL) {
                    free(file->name);
                    file->name = NULL;
                }
                if (file->extension != NULL) {
                    free(file->extension);
                    file->extension = NULL;
                }
                free(file);
                snapshot->files[i] = NULL;
            }
        }
    }
    
    // Liberar arrays y estructura
    if (snapshot->files != NULL) {
        free(snapshot->files);
        snapshot->files = NULL;
    }
    
    if (snapshot->device_name != NULL) {
        free(snapshot->device_name);
        snapshot->device_name = NULL;
    }
    
    // Limpiar campos para detectar uso después de liberación
    snapshot->file_count = -1;
    snapshot->capacity = -1;
    snapshot->snapshot_time = 0;
    
    free(snapshot);
}

