#define _GNU_SOURCE  // Para strdup y funciones GNU
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include "process_monitor.h"

// ===== VARIABLES GLOBALES =====

Config config;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_cond = PTHREAD_COND_INITIALIZER;
FILE *log_file = NULL;

// Variables globales para procesos activos
ActiveProcess *procesos_activos = NULL;
int num_procesos_activos = 0;

// Variables para control de hilos de monitoreo
static pthread_t monitoring_thread;
static volatile int monitoring_active = 0;
static volatile int should_stop = 0;
// Se activa cuando stop_monitoring() agota su timeout de 3s y el hilo de
// monitoreo queda "huérfano" (detached) en vez de unido; cleanup_monitoring()
// lo usa para evitar destruir el mutex/estado compartido mientras ese hilo
// pueda seguir vivo.
static volatile int monitoring_thread_leaked = 0;

// Callbacks opcionales para eventos
static ProcessCallbacks *event_callbacks = NULL;

// ===== DECLARACIONES DE FUNCIONES PRIVADAS =====

// Funciones de acceso a /proc
static int read_proc_stat(pid_t pid, ProcStat *stat);
static unsigned long get_total_system_memory(void);

// Funciones de persistencia
static void get_stat_file_path(pid_t pid, char *path, size_t size);
static void read_prev_times(pid_t pid, unsigned long *prev_user_time, unsigned long *prev_sys_time);
static void write_prev_times(pid_t pid, unsigned long prev_user_time, unsigned long prev_sys_time);
static void get_config_path(char *path, size_t size);

// Funciones de gestión de procesos internos
static int find_process(pid_t pid);
static void add_process(ProcessInfo info);
static void remove_process(pid_t pid);
static void update_process(ProcessInfo info, int idx);
static void clear_process_list(void);
static void show_process_stats(void);

// Funciones de alertas
static void check_and_update_alert_status(ProcessInfo *info);
static void clear_alert_if_needed(ProcessInfo *info);

// Función del hilo de monitoreo
static void* monitoring_thread_function(void* arg);

// ===== FUNCIONES DE CONFIGURACIÓN =====

// Archivo único de configuración, compartido con el dialogo de la GUI --
// antes vivia en ./matcomguard.conf (relativo al directorio de arranque,
// fragil) y la GUI guardaba un config.ini aparte con formato distinto;
// ahora ambos leen/escriben el mismo archivo con este mismo formato simple.
static void get_config_path(char *path, size_t size) {
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(path, size, "%s/.config/matcom-guard/matcomguard.conf", home);
    } else {
        snprintf(path, size, "./matcomguard.conf");
    }
}

void load_config(void) {
    pthread_mutex_lock(&mutex);

    // Valores predeterminados (los mismos que ofrece "Valores por Defecto"
    // en el dialogo de configuracion de la GUI)
    config.max_cpu_usage = 70.0;
    config.max_ram_usage = 50.0;
    config.check_interval = 30;
    config.alert_duration = 10;
    config.num_white_processes = 0;
    config.white_list = NULL;
    config.usb_scan_interval = 30;
    config.process_scan_interval = 5;
    config.port_scan_interval = 300;
    config.auto_scan_usb = 1;
    config.auto_scan_processes = 1;
    config.auto_scan_ports = 0;
    config.enable_sound_alerts = 1;
    config.enable_notifications = 1;
    config.log_to_file = 1;
    config.port_scan_start = 1;
    config.port_scan_end = 1024;

    // Cargar desde archivo
    char config_path[512];
    get_config_path(config_path, sizeof(config_path));
    FILE *conf = fopen(config_path, "r");
    if (!conf) {
        printf("[INFO] No se encontró archivo de configuración, usando valores predeterminados\n");
        pthread_mutex_unlock(&mutex);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), conf)) {
        // Actualiza el umbral para alertas de uso de CPU
        if (strstr(line, "UMBRAL_CPU=")) {
            sscanf(line, "UMBRAL_CPU=%f", &config.max_cpu_usage);
        }
        // Actualiza el umbral para alertas de uso de RAM
        else if (strstr(line, "UMBRAL_RAM=")) {
            sscanf(line, "UMBRAL_RAM=%f", &config.max_ram_usage);
        }
        // Actualiza el intervalo de realización de chequeos
        else if (strstr(line, "INTERVALO=")) {
            sscanf(line, "INTERVALO=%d", &config.check_interval);
        }
        // Actualiza la duración del estado de alerta
        else if (strstr(line, "DURACION_ALERTA=")) {
            sscanf(line, "DURACION_ALERTA=%d", &config.alert_duration);
        }
        // Ajustes editables desde el dialogo de configuracion de la GUI
        else if (strstr(line, "INTERVALO_ESCANEO_USB=")) {
            sscanf(line, "INTERVALO_ESCANEO_USB=%d", &config.usb_scan_interval);
        }
        else if (strstr(line, "INTERVALO_ESCANEO_PROCESOS=")) {
            sscanf(line, "INTERVALO_ESCANEO_PROCESOS=%d", &config.process_scan_interval);
        }
        else if (strstr(line, "INTERVALO_ESCANEO_PUERTOS=")) {
            sscanf(line, "INTERVALO_ESCANEO_PUERTOS=%d", &config.port_scan_interval);
        }
        else if (strstr(line, "AUTO_ESCANEO_USB=")) {
            sscanf(line, "AUTO_ESCANEO_USB=%d", &config.auto_scan_usb);
        }
        else if (strstr(line, "AUTO_ESCANEO_PROCESOS=")) {
            sscanf(line, "AUTO_ESCANEO_PROCESOS=%d", &config.auto_scan_processes);
        }
        else if (strstr(line, "AUTO_ESCANEO_PUERTOS=")) {
            sscanf(line, "AUTO_ESCANEO_PUERTOS=%d", &config.auto_scan_ports);
        }
        else if (strstr(line, "ALERTAS_SONORAS=")) {
            sscanf(line, "ALERTAS_SONORAS=%d", &config.enable_sound_alerts);
        }
        else if (strstr(line, "NOTIFICACIONES=")) {
            sscanf(line, "NOTIFICACIONES=%d", &config.enable_notifications);
        }
        else if (strstr(line, "GUARDAR_LOGS=")) {
            sscanf(line, "GUARDAR_LOGS=%d", &config.log_to_file);
        }
        else if (strstr(line, "RANGO_PUERTOS_INICIO=")) {
            sscanf(line, "RANGO_PUERTOS_INICIO=%d", &config.port_scan_start);
        }
        else if (strstr(line, "RANGO_PUERTOS_FIN=")) {
            sscanf(line, "RANGO_PUERTOS_FIN=%d", &config.port_scan_end);
        }
        // Actualiza los procesos a tener en cuenta en la lista blanca
        else if (strstr(line, "WHITELIST=")) {
            char *list = strchr(line, '=') + 1;
            list[strcspn(list, "\n")] = 0;

            // Tokenizar la lista de procesos
            char *token = strtok(list, ",");

            while (token) {
                // Usar temp pointer para evitar perder referencia en caso de fallo de realloc
                char **temp_list = realloc(config.white_list,
                                          (config.num_white_processes + 1) * sizeof(char*));
                if (temp_list) {
                    config.white_list = temp_list;
                    config.white_list[config.num_white_processes] = strdup(token);
                    if (config.white_list[config.num_white_processes]) {
                        config.num_white_processes++;
                    } else {
                        fprintf(stderr, "[ERROR] No se pudo duplicar cadena para whitelist\n");
                    }
                } else {
                    fprintf(stderr, "[ERROR] No se pudo expandir whitelist\n");
                    break;
                }
                token = strtok(NULL, ",");
            }
        }
    }

    fclose(conf);
    printf("[INFO] Configuración cargada: CPU=%.1f%%, RAM=%.1f%%, Intervalo=%ds, Duración alerta=%ds\n",
           config.max_cpu_usage, config.max_ram_usage, config.check_interval, config.alert_duration);

    pthread_mutex_unlock(&mutex);
}


/**
 * Obtiene acceso de solo lectura a la configuración actual
 * 
 * @return Config*: Puntero a la configuración actual
 */
Config* get_config() {
    return &config;
}

/**
 * Guarda la configuración actual en matcomguard.conf (bajo
 * ~/.config/matcom-guard/). Mantiene persistencia entre sesiones.
 */
void save_config(void) {
    pthread_mutex_lock(&mutex);

    char config_path[512];
    get_config_path(config_path, sizeof(config_path));

    // Crear ~/.config/matcom-guard/ si todavia no existe (primera vez que
    // se guarda). mkdir() con EEXIST no es un error real aca.
    char config_dir[512];
    snprintf(config_dir, sizeof(config_dir), "%s", config_path);
    char *last_slash = strrchr(config_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        char parent_dir[512];
        snprintf(parent_dir, sizeof(parent_dir), "%s", config_dir);
        char *parent_slash = strrchr(parent_dir, '/');
        if (parent_slash) {
            *parent_slash = '\0';
            mkdir(parent_dir, 0755); // ej. ~/.config
        }
        mkdir(config_dir, 0755); // ej. ~/.config/matcom-guard
    }

    FILE *conf = fopen(config_path, "w");
    if (!conf) {
        printf("[ERROR] No se pudo abrir %s para escritura\n", config_path);
        pthread_mutex_unlock(&mutex);
        return;
    }

    // Escribir la configuración actual
    fprintf(conf, "UMBRAL_CPU=%.1f\n", config.max_cpu_usage);
    fprintf(conf, "UMBRAL_RAM=%.1f\n", config.max_ram_usage);
    fprintf(conf, "INTERVALO=%d\n", config.check_interval);
    fprintf(conf, "DURACION_ALERTA=%d\n", config.alert_duration);
    fprintf(conf, "INTERVALO_ESCANEO_USB=%d\n", config.usb_scan_interval);
    fprintf(conf, "INTERVALO_ESCANEO_PROCESOS=%d\n", config.process_scan_interval);
    fprintf(conf, "INTERVALO_ESCANEO_PUERTOS=%d\n", config.port_scan_interval);
    fprintf(conf, "AUTO_ESCANEO_USB=%d\n", config.auto_scan_usb);
    fprintf(conf, "AUTO_ESCANEO_PROCESOS=%d\n", config.auto_scan_processes);
    fprintf(conf, "AUTO_ESCANEO_PUERTOS=%d\n", config.auto_scan_ports);
    fprintf(conf, "ALERTAS_SONORAS=%d\n", config.enable_sound_alerts);
    fprintf(conf, "NOTIFICACIONES=%d\n", config.enable_notifications);
    fprintf(conf, "GUARDAR_LOGS=%d\n", config.log_to_file);
    fprintf(conf, "RANGO_PUERTOS_INICIO=%d\n", config.port_scan_start);
    fprintf(conf, "RANGO_PUERTOS_FIN=%d\n", config.port_scan_end);

    // Escribir la whitelist
    fprintf(conf, "WHITELIST=");
    for (int i = 0; i < config.num_white_processes; i++) {
        fprintf(conf, "%s", config.white_list[i]);
        if (i < config.num_white_processes - 1) {
            fprintf(conf, ",");
        }
    }
    fprintf(conf, "\n");

    fclose(conf);
    printf("[INFO] Configuración guardada en %s\n", config_path);

    pthread_mutex_unlock(&mutex);
}

// ===== FUNCIONES DE INFORMACIÓN DE PROCESOS =====

ProcessInfo* get_process_info(pid_t pid) {
    // Verificar que el proceso existe
    if (!process_exists(pid)) {
        return NULL;
    }
    
    ProcessInfo *info = malloc(sizeof(ProcessInfo));
    if (!info) {
        fprintf(stderr, "[ERROR] No se pudo asignar memoria para ProcessInfo\n");
        return NULL;
    }
    
    // Inicializar estructura
    memset(info, 0, sizeof(ProcessInfo));
    info->pid = pid;
    
    // Obtener nombre del proceso
    get_process_name(pid, info->name, sizeof(info->name));
    
    // Si no se pudo obtener el nombre, el proceso probablemente terminó
    if (strlen(info->name) == 0 || strstr(info->name, "unknown_")) {
        free(info);
        return NULL;
    }
    
    // Verificar si está en whitelist
    info->is_whitelisted = is_process_whitelisted(info->name);
    
    // Obtener uso de CPU
    info->cpu_usage = interval_cpu_usage(pid);
    
    // Obtener uso de memoria
    info->mem_usage = get_process_memory_usage(pid);
    
    // Inicializar campos de alerta
    info->alerta_activa = 0;
    info->inicio_alerta = 0;
    info->exceeds_thresholds = 0;
    info->first_threshold_exceed = 0;
    info->cpu_time = 0.0;
    
    return info;
}

int process_exists(pid_t pid) {
    char stat_path[256];
    sprintf(stat_path, "/proc/%d/stat", pid);
    FILE *fp = fopen(stat_path, "r");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

void get_process_name(pid_t pid, char *name, size_t size) {
    // Intentar primero /proc/[pid]/comm (más confiable)
    char comm_path[256];
    sprintf(comm_path, "/proc/%d/comm", pid);
    FILE *fp = fopen(comm_path, "r");
    
    if (fp) {
        if (fgets(name, size, fp)) {
            // Remover \n al final
            name[strcspn(name, "\n")] = '\0';
            fclose(fp);
            return;
        }
        fclose(fp);
    }
    
    // Si comm falla, intentar desde /proc/[pid]/stat
    char stat_path[256];
    sprintf(stat_path, "/proc/%d/stat", pid);
    fp = fopen(stat_path, "r");
    
    if (fp) {
        char comm_field[256];
        if (fscanf(fp, "%*d %255s", comm_field) == 1) {
            // Remover paréntesis del nombre
            if (comm_field[0] == '(' && comm_field[strlen(comm_field)-1] == ')') {
                comm_field[strlen(comm_field)-1] = '\0';
                strncpy(name, comm_field + 1, size - 1);
                name[size - 1] = '\0';
            } else {
                strncpy(name, comm_field, size - 1);
                name[size - 1] = '\0';
            }
            fclose(fp);
            return;
        }
        fclose(fp);
    }
    
    // Si todo falla, nombre por defecto
    snprintf(name, size, "unknown_%d", pid);
}

int is_process_whitelisted(const char *process_name) {
    if (!process_name || !config.white_list) return 0;
    
    for (int i = 0; i < config.num_white_processes; i++) {
        if (config.white_list[i] && strcmp(process_name, config.white_list[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// ===== FUNCIONES DE MONITOREO PRINCIPAL =====

void monitor_processes(void) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        perror("Error al abrir /proc");
        return;
    }

    // 1. Marcar todos los procesos actuales como "no encontrados"
    for (int i = 0; i < num_procesos_activos; i++) {
        procesos_activos[i].encontrado = 0;
    }

    // 2. Recorrer /proc y procesar cada PID
    struct dirent *entry;
    printf("=== CICLO DE MONITOREO ===\n");
    
    while ((entry = readdir(dir)) != NULL) {
        // Verificar si el nombre del directorio es un número (PID)
        char *endptr;
        long lpid = strtol(entry->d_name, &endptr, 10);
        if (*endptr == '\0') {
            pid_t pid = (pid_t)lpid;

            // Obtener información del proceso
            ProcessInfo *info_ptr = get_process_info(pid);
            if (!info_ptr || strlen(info_ptr->name) == 0) {
                if (info_ptr) free(info_ptr);
                continue;
            }
            
            int idx = find_process(pid);
            if (idx == -1) {
                // Proceso nuevo - agregarlo
                add_process(*info_ptr);
                // Callback para proceso nuevo
                if (event_callbacks && event_callbacks->on_new_process) {
                    event_callbacks->on_new_process(info_ptr);
                }
            } else {
                // Proceso existente - actualizar información y verificar alertas
                ProcessInfo *existing = &procesos_activos[idx].info;
                
                // Preservar información de alertas previas
                int prev_exceeds = existing->exceeds_thresholds;
                time_t prev_first_exceed = existing->first_threshold_exceed;
                int prev_alerta_activa = existing->alerta_activa;
                time_t prev_inicio_alerta = existing->inicio_alerta;
                
                // Actualizar información del proceso
                update_process(*info_ptr, idx);

                // Restaurar información de alertas
                existing->exceeds_thresholds = prev_exceeds;
                existing->first_threshold_exceed = prev_first_exceed;
                existing->alerta_activa = prev_alerta_activa;
                existing->inicio_alerta = prev_inicio_alerta;
                
                // Verificar y actualizar estado de alerta
                check_and_update_alert_status(existing);
            }
            
            // Verificar si el proceso excede umbrales y generar alertas con callbacks
            if (info_ptr->cpu_usage > config.max_cpu_usage) {
                printf("[ALERTA CPU] PID: %d, Nombre: %s, CPU: %.2f%%\n",
                       pid, info_ptr->name, info_ptr->cpu_usage);
                if (event_callbacks && event_callbacks->on_high_cpu_alert) {
                    event_callbacks->on_high_cpu_alert(info_ptr);
                }
            }
            
            if (info_ptr->mem_usage > config.max_ram_usage) {
                printf("[ALERTA MEM] PID: %d, Nombre: %s, Mem: %.2f%%\n",
                       pid, info_ptr->name, info_ptr->mem_usage);
                if (event_callbacks && event_callbacks->on_high_memory_alert) {
                    event_callbacks->on_high_memory_alert(info_ptr);
                }
            }
            
            free(info_ptr);
        }
    }
    closedir(dir);

    // 3. Eliminar procesos que no fueron encontrados (terminados)
    for (int i = num_procesos_activos - 1; i >= 0; i--) {
        if (!procesos_activos[i].encontrado) {
            pid_t terminated_pid = procesos_activos[i].info.pid;
            char terminated_name[256];
            strncpy(terminated_name, procesos_activos[i].info.name, sizeof(terminated_name) - 1);
            terminated_name[255] = '\0';
            
            // Callback para proceso terminado
            if (event_callbacks && event_callbacks->on_process_terminated) {
                event_callbacks->on_process_terminated(terminated_pid, terminated_name);
            }
            
            remove_process(terminated_pid);
        }
    }
    
    // 4. Mostrar estadísticas opcionales
    show_process_stats();
}

// ===== FUNCIONES DE CONTROL DE HILOS =====

void set_process_callbacks(ProcessCallbacks *callbacks) {
    pthread_mutex_lock(&mutex);
    event_callbacks = callbacks;
    pthread_mutex_unlock(&mutex);
}

static void* monitoring_thread_function(void* arg) {
    (void)arg; // Evitar warning de parámetro no usado

    pthread_mutex_lock(&mutex);
    while (!should_stop) {
        monitor_processes();
        int active_count = num_procesos_activos;
        ProgressCallback status_cb = NULL;
        void *status_user_data = NULL;
        if (event_callbacks) {
            status_cb = event_callbacks->on_status_update;
            status_user_data = event_callbacks->status_user_data;
        }

        if (should_stop) {
            break;
        }

        pthread_mutex_unlock(&mutex);
        if (status_cb) {
            char phase[128];
            snprintf(phase, sizeof(phase), "Monitoreando... %d procesos activos", active_count);
            ProgressUpdate update = { .phase = phase, .current = 0, .total = 0 };
            status_cb(&update, status_user_data);
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

int start_monitoring(void) {
    pthread_mutex_lock(&mutex);
    
    if (monitoring_active) {
        pthread_mutex_unlock(&mutex);
        printf("[INFO] El monitoreo ya está activo\n");
        return 1; // Ya está activo
    }
    
    should_stop = 0;
    monitoring_active = 1;
    monitoring_thread_leaked = 0;

    int result = pthread_create(&monitoring_thread, NULL, monitoring_thread_function, NULL);
    if (result != 0) {
        monitoring_active = 0;
        pthread_mutex_unlock(&mutex);
        fprintf(stderr, "[ERROR] No se pudo crear el hilo de monitoreo: %d\n", result);
        return -1;
    }
    
    pthread_mutex_unlock(&mutex);
    printf("[INFO] Monitoreo iniciado con intervalo de %d segundos\n", config.check_interval);
    return 0;
}

/**
 * @brief Detiene el monitoreo de procesos de forma robusta
 * 
 * Esta función implementa una estrategia de terminación inteligente que previene
 * los colgamientos comunes durante el cierre de aplicaciones multihilo. El problema
 * típico ocurre cuando pthread_join() se bloquea indefinidamente esperando por un
 * hilo que no responde, causando que toda la aplicación se "congele" al intentar salir.
 * 
 * ESTRATEGIA DE TERMINACIÓN ROBUSTA:
 * 1. Señalización limpia: Informa al hilo que debe terminar (should_stop = 1)
 * 2. Espera activa con timeout: Verifica periódicamente si el hilo terminó (3 segundos máximo)
 * 3. Terminación forzada: Si no responde, marca forzadamente como inactivo
 * 4. Join final: Permite que pthread_join complete si es posible, pero sin bloqueo indefinido
 * 
 * Esta aproximación garantiza que la aplicación nunca se cuelgue al cerrar, permitiendo
 * al usuario salir limpiamente incluso si hay hilos problemáticos.
 * 
 * @return 0 si se detuvo correctamente, 1 si no estaba activo, -1 si error
 */
int stop_monitoring(void) {
    pthread_mutex_lock(&mutex);

    if (!monitoring_active) {
        pthread_mutex_unlock(&mutex);
        printf("[INFO] El monitoreo no está activo\n");
        return 1;
    }

    should_stop = 1;
    pthread_cond_broadcast(&stop_cond);
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
    // El hilo no respondió a tiempo y puede seguir vivo (p. ej. bloqueado en
    // monitor_processes() o en un callback lento fuera del mutex). Lo separamos
    // (detach) para que el sistema operativo reclame sus recursos cuando
    // finalmente termine, en vez de dejar un handle de hilo sin unir. Nunca se
    // debe llamar pthread_join sobre monitoring_thread después de esto.
    pthread_detach(monitoring_thread);
    monitoring_thread_leaked = 1;
    pthread_mutex_unlock(&mutex);
    printf("[WARNING] Timeout al esperar terminación - marcando como inactivo\n");
    // Se mantiene el contrato original de stop_monitoring(): 0 indica que la
    // solicitud de detención fue procesada (incluso si el hilo quedó huérfano),
    // 1 solo cuando el monitoreo no estaba activo. Los llamadores existentes
    // (p. ej. la GUI) dependen de este valor para decidir su propio flujo de
    // limpieza tras un stop "exitoso".
    return 0;
}

int is_monitoring_active(void) {
    pthread_mutex_lock(&mutex);
    int active = monitoring_active;
    pthread_mutex_unlock(&mutex);
    return active;
}

MonitoringStats get_monitoring_stats(void) {
    MonitoringStats stats = {0};
    
    pthread_mutex_lock(&mutex);
    
    stats.total_processes = num_procesos_activos;
    stats.is_active = monitoring_active;
    stats.check_interval = config.check_interval;
    
    // Contar alertas activas
    for (int i = 0; i < num_procesos_activos; i++) {
        ProcessInfo *p = &procesos_activos[i].info;
        if (p->cpu_usage > config.max_cpu_usage) stats.high_cpu_count++;
        if (p->mem_usage > config.max_ram_usage) stats.high_memory_count++;
        if (p->alerta_activa) stats.active_alerts++;
    }
    
    pthread_mutex_unlock(&mutex);
    
    return stats;
}

ProcessInfo* get_process_list_copy(int *count) {
    pthread_mutex_lock(&mutex);
    
    *count = num_procesos_activos;
    if (num_procesos_activos == 0) {
        pthread_mutex_unlock(&mutex);
        return NULL;
    }
    
    ProcessInfo *copy = malloc(num_procesos_activos * sizeof(ProcessInfo));
    if (copy) {
        for (int i = 0; i < num_procesos_activos; i++) {
            copy[i] = procesos_activos[i].info;
        }
    }
    
    pthread_mutex_unlock(&mutex);
    return copy;
}

/**
 * @brief Limpia todos los recursos del sistema de monitoreo de procesos
 * 
 * Esta función realiza una limpieza exhaustiva y ordenada de todos los recursos
 * utilizados por el sistema de monitoreo. El orden de limpieza es crítico para
 * evitar condiciones de carrera y asegurar que no queden hilos huérfanos o
 * recursos bloqueados.
 * 
 * ORDEN DE LIMPIEZA IMPLEMENTADO:
 * 1. Detener hilos de monitoreo (con timeout de seguridad)
 * 2. Limpiar listas de procesos bajo protección de mutex
 * 3. Liberar memoria dinámica (whitelist)
 * 4. Limpiar archivos temporales del sistema
 * 5. Destruir primitivas de sincronización al final
 * 
 * Este enfoque asegura que el programa pueda cerrarse limpiamente sin dejar
 * recursos colgando en el sistema operativo.
 */
void cleanup_monitoring(void) {
    printf("[INFO] Iniciando limpieza de recursos de monitoreo...\n");
    
    // PASO 1: Detener hilos de forma segura usando la función mejorada con timeout
    stop_monitoring();

    // Si stop_monitoring() agotó su timeout de 3s, el hilo de monitoreo pudo
    // quedar vivo (ya detached, ver stop_monitoring()). En ese caso NO es seguro
    // destruir el mutex ni liberar el estado compartido (whitelist,
    // event_callbacks, lista de procesos) porque el hilo huérfano podría seguir
    // leyéndolos/escribiéndolos en cualquier momento, lo que sería un use-after-free.
    // Como cleanup_monitoring() se invoca durante el apagado de la aplicación,
    // preferimos "perder" (leak) deliberadamente ese estado y dejar que el
    // sistema operativo lo reclame al terminar el proceso, en vez de arriesgar
    // un use-after-free o bloquear un mutex ya destruido.
    if (monitoring_thread_leaked) {
        printf("[WARNING] El hilo de monitoreo no terminó a tiempo; omitiendo liberación de "
               "recursos compartidos para evitar condiciones de carrera con el hilo huérfano.\n");
        cleanup_stale_temp_files();
        cleanup_temp_files();
        return;
    }

    pthread_mutex_lock(&mutex);
    clear_process_list();
    
    // PASO 2: Liberar memoria dinámica de la whitelist de forma segura
    if (config.white_list) {
        for (int i = 0; i < config.num_white_processes; i++) {
            if (config.white_list[i]) {
                free(config.white_list[i]);
                config.white_list[i] = NULL;
            }
        }
        free(config.white_list);
        config.white_list = NULL;
        config.num_white_processes = 0;
    }
    
    event_callbacks = NULL;
    
    // PASO 3: Asegurar estado consistente antes de destruir recursos de sincronización
    monitoring_active = 0;
    should_stop = 1;
    
    pthread_mutex_unlock(&mutex);
    
    // PASO 4: Limpiar archivos temporales para evitar acumulación en el sistema
    cleanup_stale_temp_files();
    cleanup_temp_files();
    
    // PASO 5: Destruir mutex al final cuando ya no se necesita sincronización
    pthread_mutex_destroy(&mutex);
    printf("[INFO] ✅ Recursos de monitoreo liberados correctamente\n");
}

// ===== FUNCIONES DE ALERTAS =====

static void check_and_update_alert_status(ProcessInfo *info) {
    if (!info) return;
    
    // Si está en whitelist, no generar alertas
    if (info->is_whitelisted) {
        clear_alert_if_needed(info);
        return;
    }
    
    time_t current_time = time(NULL);
    
    // Verificar si excede umbrales según la fórmula del RF2:
    // alerta = (uso_CPU > UMBRAL_CPU) ∨ (uso_RAM > UMBRAL_RAM)
    int exceeds_now = (info->cpu_usage > config.max_cpu_usage) || 
                      (info->mem_usage > config.max_ram_usage);
    
    if (exceeds_now) {
        if (!info->exceeds_thresholds) {
            // Primera vez que excede umbrales
            info->exceeds_thresholds = 1;
            info->first_threshold_exceed = current_time;
            printf("[INFO] Proceso %s (PID: %d) comenzó a exceder umbrales. CPU: %.2f%%, MEM: %.2f%%\n",
                   info->name, info->pid, info->cpu_usage, info->mem_usage);
        } else {
            // Ya estaba excediendo, verificar duración
            int duration = (int)(current_time - info->first_threshold_exceed);
            
            if (duration >= config.alert_duration && !info->alerta_activa) {
                // Activar alerta después de la duración configurada
                info->alerta_activa = 1;
                info->inicio_alerta = current_time;
                
                printf("[ALERTA ACTIVADA] PID: %d, Nombre: %s, Duración: %d seg, CPU: %.2f%%, MEM: %.2f%%\n",
                       info->pid, info->name, duration, info->cpu_usage, info->mem_usage);
                
                // Callbacks específicos según el tipo de exceso
                if (info->cpu_usage > config.max_cpu_usage && event_callbacks && event_callbacks->on_high_cpu_alert) {
                    event_callbacks->on_high_cpu_alert(info);
                }
                if (info->mem_usage > config.max_ram_usage && event_callbacks && event_callbacks->on_high_memory_alert) {
                    event_callbacks->on_high_memory_alert(info);
                }
            }
        }
    } else {
        // Ya no excede umbrales
        clear_alert_if_needed(info);
    }
}

static void clear_alert_if_needed(ProcessInfo *info) {
    if (!info) return;
    
    if (info->exceeds_thresholds || info->alerta_activa) {
        int was_active = info->alerta_activa;
        
        // Resetear todos los campos de alerta
        info->exceeds_thresholds = 0;
        info->first_threshold_exceed = 0;
        info->alerta_activa = 0;
        info->inicio_alerta = 0;
        
        if (was_active) {
            printf("[ALERTA DESPEJADA] PID: %d, Nombre: %s volvió a valores normales. CPU: %.2f%%, MEM: %.2f%%\n",
                   info->pid, info->name, info->cpu_usage, info->mem_usage);
            
            // Callback para alerta despejada
            if (event_callbacks && event_callbacks->on_alert_cleared) {
                event_callbacks->on_alert_cleared(info);
            }
        }
    }
}

// ===== FUNCIONES DE ACCESO A /proc =====

static int read_proc_stat(pid_t pid, ProcStat *stat) {
    char stat_path[256];
    sprintf(stat_path, "/proc/%d/stat", pid);
    FILE *fp = fopen(stat_path, "r");
    if (!fp) return -1;    
    
    // Leer los campos 14 (utime), 15 (stime), 22 (starttime)
    int scanned = fscanf(fp,
        "%*d "      // pid (campo 1)
        "%*s "      // comm (campo 2)
        "%*c "      // state (campo 3)
        "%*d %*d %*d %*d %*d "  // campos 4-8
        "%*u %*u %*u %*u %*u "  // campos 9-13
        "%lu %lu %*d %*d %*d %*d %*d %*d %lu",  // campos 14, 15, 16-21, 22
        &stat->utime, &stat->stime, &stat->starttime);

    fclose(fp);
    return (scanned == 3) ? 0 : -1;
}

static unsigned long get_total_system_memory(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return 0;
    
    char line[256];
    unsigned long total_mem = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "MemTotal:")) {
            sscanf(line, "MemTotal: %lu kB", &total_mem);
            break;
        }
    }
    fclose(fp);
    return total_mem;
}

// ===== FUNCIONES DE PERSISTENCIA =====

static void get_stat_file_path(pid_t pid, char *path, size_t size) {
    snprintf(path, size, "/tmp/procstat_%d.dat", pid);
}

static void read_prev_times(pid_t pid, unsigned long *prev_user_time, unsigned long *prev_sys_time) {
    char path[64];
    get_stat_file_path(pid, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        int scanned = fscanf(f, "%lu %lu", prev_user_time, prev_sys_time);
        fclose(f);
        
        // Si no se pudieron leer ambos valores, inicializar a 0
        if (scanned != 2) {
            *prev_user_time = 0;
            *prev_sys_time = 0;
        }
    } else {
        *prev_user_time = 0;
        *prev_sys_time = 0;
    }
}

static void write_prev_times(pid_t pid, unsigned long prev_user_time, unsigned long prev_sys_time) {
    char path[64];
    get_stat_file_path(pid, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%lu %lu\n", prev_user_time, prev_sys_time);
        fclose(f);
    }
}

// ===== FUNCIONES DE CÁLCULO DE RECURSOS =====

float total_cpu_usage(pid_t pid) {
    ProcStat stat;
    if (read_proc_stat(pid, &stat) != 0) return 0.0;
    
    // Obtener uptime del sistema en segundos
    FILE *uptime_fp = fopen("/proc/uptime", "r");
    if (!uptime_fp) return 0.0;
    double system_uptime_seconds = 0.0;
    fscanf(uptime_fp, "%lf", &system_uptime_seconds);
    fclose(uptime_fp);

    long clk_tck = sysconf(_SC_CLK_TCK);
    
    // Calcular tiempo que el proceso ha estado vivo en segundos
    double process_start_time_seconds = stat.starttime / (double)clk_tck;
    double process_uptime_seconds = system_uptime_seconds - process_start_time_seconds;
    
    if (process_uptime_seconds <= 0) return 0.0;
    
    // Convertir tiempo de CPU usado a segundos
    double cpu_time_seconds = (stat.utime + stat.stime) / (double)clk_tck;
    
    // Calcular porcentaje: (tiempo_cpu_usado / tiempo_vida_proceso) * 100
    return (float)(100.0 * (cpu_time_seconds / process_uptime_seconds));
}

float interval_cpu_usage(pid_t pid) {
    ProcStat stat;
    if (read_proc_stat(pid, &stat) != 0) return 0.0;

    unsigned long prev_user_time = 0, prev_sys_time = 0;
    read_prev_times(pid, &prev_user_time, &prev_sys_time);

    // Verificar consistencia de datos (proceso reiniciado, overflow, etc.)
    if ((prev_user_time == 0 && prev_sys_time == 0) ||
        (stat.utime < prev_user_time || stat.stime <  prev_sys_time))
    {
        write_prev_times(pid, stat.utime, stat.stime);
        return total_cpu_usage(pid);
    }    
    
    unsigned long delta_user = stat.utime - prev_user_time;
    unsigned long delta_sys = stat.stime - prev_sys_time;
    unsigned long delta_total = delta_user + delta_sys;
    long clk_tck = sysconf(_SC_CLK_TCK);

    // Guardar tiempos actuales para próxima medición
    write_prev_times(pid, stat.utime, stat.stime);

    // Calcular porcentaje de CPU
    double cpu_percentage = 100.0 * (delta_total / ((double)clk_tck * config.check_interval));
    
    // Detectar valores extremadamente altos que indican problemas de datos
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    double max_theoretical_cpu = num_cores * 100.0;
    
    if (cpu_percentage > max_theoretical_cpu) {
        fprintf(stderr, "[WARNING] PID %d: CPU calculation suspicious: %.2f%% "
                "(max theoretical: %.2f%% for %ld cores)\n", 
                pid, cpu_percentage, max_theoretical_cpu, num_cores);
        return total_cpu_usage(pid);
    }
    
    return (float)cpu_percentage;
}

float get_process_memory_usage(pid_t pid) {
    char status_path[256];
    sprintf(status_path, "/proc/%d/status", pid);
    FILE *fp = fopen(status_path, "r");
    if (!fp) return 0.0;
    
    char line[256];
    unsigned long vmrss = 0; // Memoria física usada
    
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "VmRSS:")) {
            sscanf(line, "VmRSS: %lu kB", &vmrss);
            break;
        }
    }
    fclose(fp);
    
    if (vmrss == 0) return 0.0;
    
    unsigned long total_mem = get_total_system_memory();
    if (total_mem == 0) return 0.0;
    
    float mem_percentage = (float)(100.0 * vmrss / total_mem);
    
    // Validación: la memoria nunca debería exceder 100%
    if (mem_percentage > 100.0) {
        return 100.0;
    }
    
    return mem_percentage;
}

// ===== FUNCIONES DE GESTIÓN DE PROCESOS =====

static int find_process(pid_t pid) {
    for (int i = 0; i < num_procesos_activos; i++) {
        if (procesos_activos[i].info.pid == pid) {
            return i;
        }
    }
    return -1;
}

static void add_process(ProcessInfo info) {
    // Usar temp pointer para evitar perder referencia en caso de fallo de realloc
    ActiveProcess *temp_array = realloc(procesos_activos, 
                                       (num_procesos_activos + 1) * sizeof(ActiveProcess));
    
    if (temp_array == NULL) {
        fprintf(stderr, "[ERROR] No se pudo asignar memoria para nuevo proceso\n");
        return;
    }
    
    procesos_activos = temp_array;
    procesos_activos[num_procesos_activos].info = info;
    procesos_activos[num_procesos_activos].encontrado = 1;
    num_procesos_activos++;
    
    printf("[NUEVO PROCESO] PID: %d, Nombre: %s\n", info.pid, info.name);
}

static void remove_process(pid_t pid) {
    int idx = find_process(pid);
    if (idx == -1) return;
    
    printf("[PROCESO TERMINADO] PID: %d, Nombre: %s\n", 
           procesos_activos[idx].info.pid, procesos_activos[idx].info.name);
    
    // Mover todos los elementos hacia la izquierda
    for (int i = idx; i < num_procesos_activos - 1; i++) {
        procesos_activos[i] = procesos_activos[i + 1];
    }
    
    num_procesos_activos--;
    
    if (num_procesos_activos > 0) {
        // Usar temp pointer para evitar perder referencia en caso de fallo de realloc
        ActiveProcess *temp_array = realloc(procesos_activos, 
                                           num_procesos_activos * sizeof(ActiveProcess));
        // Solo actualizar si realloc fue exitoso (temp_array != NULL)
        // Si falla, mantenemos el array original que sigue siendo válido
        if (temp_array != NULL) {
            procesos_activos = temp_array;
        }
        // Si temp_array es NULL, simplemente mantenemos el array original
        // aunque será un poco más grande de lo necesario
    } else {
        // Si no quedan procesos, liberar completamente
        free(procesos_activos);
        procesos_activos = NULL;
    }
}

static void update_process(ProcessInfo info, int idx) {
    if (idx < 0 || idx >= num_procesos_activos || procesos_activos == NULL) {
        fprintf(stderr, "[ERROR] Índice inválido en update_process: %d (max: %d)\n", 
                idx, num_procesos_activos - 1);
        return;
    }
    
    procesos_activos[idx].info = info;
    procesos_activos[idx].encontrado = 1;
}

static void clear_process_list(void) {
    if (procesos_activos != NULL) {
        free(procesos_activos);
        procesos_activos = NULL;
    }
    num_procesos_activos = 0;
    printf("[INFO] Lista de procesos activos limpiada\n");
}

static void show_process_stats(void) {
    printf("\n=== ESTADÍSTICAS DE PROCESOS ACTIVOS ===\n");
    printf("Total de procesos monitoreados: %d\n", num_procesos_activos);
    
    int alertas_cpu = 0, alertas_mem = 0;
    for (int i = 0; i < num_procesos_activos; i++) {
        ProcessInfo *p = &procesos_activos[i].info;
        if (p->cpu_usage > config.max_cpu_usage) alertas_cpu++;
        if (p->mem_usage > config.max_ram_usage) alertas_mem++;
    }
    
    printf("Procesos con alta CPU: %d\n", alertas_cpu);
    printf("Procesos con alta memoria: %d\n", alertas_mem);
    printf("=========================================\n\n");
}

// ===== FUNCIONES DE LIMPIEZA =====

void cleanup_temp_files(void) {
    printf("[INFO] Limpiando archivos temporales de estadísticas...\n");
    
    // Eliminar archivos temporales del directorio /tmp
    int result = system("rm -f /tmp/procstat_*.dat 2>/dev/null");
    if (result == 0) {
        printf("[INFO] Archivos temporales limpiados exitosamente\n");
    } else {
        printf("[WARNING] No se pudieron limpiar algunos archivos temporales\n");
    }
}

void cleanup_stale_temp_files(void) {
    printf("[INFO] Limpiando archivos temporales antiguos...\n");

    if (system("find /tmp -name 'procstat_*.dat' -mtime +1 -delete 2>/dev/null") == 0) {
        printf("[INFO] Archivos temporales antiguos limpiados exitosamente\n");
    } else {
        printf("[WARNING] No se pudieron limpiar algunos archivos temporales viejos\n");
    }
}