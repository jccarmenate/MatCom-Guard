#ifndef PROCESS_MONITOR_H  
#define PROCESS_MONITOR_H  

#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include "progress.h"

#define CONFIG_PATH "./matcomguard.conf"

// ===== ESTRUCTURAS PÚBLICAS =====

typedef struct {
    pid_t pid;
    char name[256];
    float cpu_usage;
    float cpu_time;
    float mem_usage;
    time_t inicio_alerta;
    int alerta_activa;
    int exceeds_thresholds;  // 1 si excede umbrales, 0 si no
    time_t first_threshold_exceed; // Momento en que empezó a exceder umbrales
    int is_whitelisted;      // 1 si está en whitelist, 0 si no
} ProcessInfo;

typedef struct {
    float max_cpu_usage;
    float max_ram_usage;
    int check_interval;
    int alert_duration;
    char **white_list;
    int num_white_processes;
} Config;

// ===== CALLBACKS PARA EVENTOS =====

// Estructura para callbacks de eventos
typedef struct {
    void (*on_new_process)(ProcessInfo *info);
    void (*on_process_terminated)(pid_t pid, const char *name);
    void (*on_high_cpu_alert)(ProcessInfo *info);
    void (*on_high_memory_alert)(ProcessInfo *info);
    void (*on_alert_cleared)(ProcessInfo *info);
    ProgressCallback on_status_update;
    void *status_user_data;
} ProcessCallbacks;

// Estructura para estadísticas de monitoreo
typedef struct {
    int total_processes;
    int high_cpu_count;
    int high_memory_count;
    int active_alerts;
    int is_active;
    int check_interval;
} MonitoringStats;

// ===== ESTRUCTURAS INTERNAS =====

// Estructura para estadísticas de /proc/[pid]/stat
typedef struct {
    unsigned long utime;
    unsigned long stime;
    unsigned long starttime;
} ProcStat;

// Estructura auxiliar para rastrear procesos activos
typedef struct {
    ProcessInfo info;
    int encontrado;  // Flag para marcar si fue encontrado en el ciclo actual
} ActiveProcess;

// ===== FUNCIONES API PÚBLICAS =====

// Configuración
void load_config(void);
void save_config(void);
Config* get_config();

// Control de monitoreo básico
void monitor_processes();
ProcessInfo* get_process_info(pid_t pid);

// Control de hilos de monitoreo
int start_monitoring();
int stop_monitoring();
int is_monitoring_active();
void set_process_callbacks(ProcessCallbacks *callbacks);

// Funciones thread-safe para acceder a datos
MonitoringStats get_monitoring_stats();
ProcessInfo* get_process_list_copy(int *count);
void cleanup_monitoring();

// ===== FUNCIONES AUXILIARES PÚBLICAS =====

// Funciones de cálculo de recursos
float total_cpu_usage(pid_t pid);
float interval_cpu_usage(pid_t pid);
float get_process_memory_usage(pid_t pid);

// Funciones de utilidad de procesos
int process_exists(pid_t pid);
void get_process_name(pid_t pid, char *name, size_t size);
int is_process_whitelisted(const char *process_name);

// Funciones de limpieza
void cleanup_temp_files();
void cleanup_stale_temp_files();

// Variables globales externas
extern Config config;

#endif