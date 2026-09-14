// include/gui_system_coordinator.h
#ifndef GUI_SYSTEM_COORDINATOR_H
#define GUI_SYSTEM_COORDINATOR_H

#include "gui.h"
#include "gui_usb_panel.h"
#include "gui_process_panel.h"
#include "gui_ports_panel.h"
#include <pthread.h>
#include <time.h>

typedef enum {
    SECURITY_LEVEL_SAFE,
    SECURITY_LEVEL_MONITORING,
    SECURITY_LEVEL_WARNING,
    SECURITY_LEVEL_CRITICAL,
    SECURITY_LEVEL_UNKNOWN
} SystemSecurityLevel;

typedef enum {
    MODULE_STATUS_INACTIVE,
    MODULE_STATUS_ACTIVE,
    MODULE_STATUS_SCANNING,
    MODULE_STATUS_ERROR,
    MODULE_STATUS_MAINTENANCE
} ModuleStatus;

typedef struct {
    SystemSecurityLevel security_level;
    time_t last_security_evaluation;
    char security_description[512];

    ModuleStatus process_module_status;
    ModuleStatus usb_module_status;
    ModuleStatus ports_module_status;

    struct {
        int total_processes_monitored;
        int suspicious_processes;
        int processes_exceeding_cpu_threshold;
        int processes_exceeding_memory_threshold;
        time_t last_process_scan;

        int total_usb_devices;
        int suspicious_usb_devices;
        int total_files_monitored;
        int files_with_changes;
        time_t last_usb_scan;

        int total_ports_scanned;
        int open_ports_found;
        int suspicious_ports;
        time_t last_port_scan;

        time_t statistics_last_updated;
    } aggregate_stats;

    struct {
        time_t system_start_time;
        int coordination_cycles_completed;
        int gui_updates_sent;
        float average_coordination_time_ms;
    } performance_metrics;

    pthread_mutex_t state_mutex;
} SystemGlobalState;

int init_system_coordinator(void);
int start_system_coordinator(int update_interval_seconds);
int stop_system_coordinator(void);
void cleanup_system_coordinator(void);

SystemSecurityLevel evaluate_system_security_level(void);
int update_aggregate_statistics(void);
int detect_cross_module_correlations(void);

int get_global_state_copy(SystemGlobalState *state_copy);
SystemSecurityLevel get_current_security_level(void);
int get_consolidated_statistics(int *total_devices, int *total_processes,
                               int *total_open_ports, int *security_alerts);

int notify_security_event(const char *source_module, int event_severity,
                         const char *event_description);
int notify_module_status_change(const char *module_name, ModuleStatus new_status,
                               const char *status_description);

int update_coordinator_configuration(int update_interval, int security_evaluation_sensitivity);
int request_immediate_system_evaluation(void);

#endif // GUI_SYSTEM_COORDINATOR_H
