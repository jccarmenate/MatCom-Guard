// src/gui/gui_system_coordinator.c
#include "gui_system_coordinator.h"
#include "gui_internal.h"
#include "gui_periodic_worker.h"
#include "gui_thread_bridge.h"
#include "gui_dashboard_panel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static SystemGlobalState global_state;
static GuiPeriodicWorker *coordinator_worker = NULL;
static GuiThreadBridgeTarget coordinator_bridge_target;
static int security_evaluation_sensitivity = 5;

// ============================================================================
// EVALUACION Y AGREGACION (pura, sin GTK -- se ejecuta en el hilo del worker)
// ============================================================================

SystemSecurityLevel evaluate_system_security_level(void) {
    int security_score = 0;
    int total_threats = 0;

    if (global_state.aggregate_stats.suspicious_processes > 0) {
        security_score += global_state.aggregate_stats.suspicious_processes * 15;
        total_threats += global_state.aggregate_stats.suspicious_processes;
    }
    if (global_state.aggregate_stats.suspicious_usb_devices > 0) {
        security_score += global_state.aggregate_stats.suspicious_usb_devices * 25;
        total_threats += global_state.aggregate_stats.suspicious_usb_devices;
    }
    if (global_state.aggregate_stats.suspicious_ports > 0) {
        security_score += global_state.aggregate_stats.suspicious_ports * 20;
        total_threats += global_state.aggregate_stats.suspicious_ports;
    }
    security_score += (global_state.aggregate_stats.processes_exceeding_cpu_threshold * 2);
    security_score += (global_state.aggregate_stats.processes_exceeding_memory_threshold * 2);

    if (global_state.process_module_status == MODULE_STATUS_ERROR) security_score += 10;
    if (global_state.usb_module_status == MODULE_STATUS_ERROR) security_score += 10;
    if (global_state.ports_module_status == MODULE_STATUS_ERROR) security_score += 10;

    security_score = (security_score * security_evaluation_sensitivity) / 5;

    SystemSecurityLevel level;
    char description[512];

    if (security_score == 0) {
        level = SECURITY_LEVEL_SAFE;
        snprintf(description, sizeof(description), "Sistema seguro - No se detectaron amenazas");
    } else if (security_score <= 10) {
        level = SECURITY_LEVEL_MONITORING;
        snprintf(description, sizeof(description), "Monitoreo activo - Actividad menor detectada (puntuacion: %d)", security_score);
    } else if (security_score <= 30) {
        level = SECURITY_LEVEL_WARNING;
        snprintf(description, sizeof(description), "Advertencia - %d amenaza(s) detectada(s) (puntuacion: %d)", total_threats, security_score);
    } else {
        level = SECURITY_LEVEL_CRITICAL;
        snprintf(description, sizeof(description), "CRITICO - Multiples amenazas activas detectadas (puntuacion: %d)", security_score);
    }

    strncpy(global_state.security_description, description, sizeof(global_state.security_description) - 1);
    global_state.security_description[sizeof(global_state.security_description) - 1] = '\0';

    return level;
}

int update_aggregate_statistics(void) {
    int success_count = 0;
    const int total_modules = 3;

    int processes_monitored = 0, high_cpu = 0, high_mem = 0, suspicious_processes = 0;
    if (get_process_statistics_for_gui(&processes_monitored, &high_cpu, &high_mem, &suspicious_processes) == 0) {
        global_state.aggregate_stats.total_processes_monitored = processes_monitored;
        global_state.aggregate_stats.processes_exceeding_cpu_threshold = high_cpu;
        global_state.aggregate_stats.processes_exceeding_memory_threshold = high_mem;
        global_state.aggregate_stats.suspicious_processes = suspicious_processes;
        global_state.aggregate_stats.last_process_scan = time(NULL);
        global_state.process_module_status = is_process_monitoring_active() ? MODULE_STATUS_ACTIVE : MODULE_STATUS_INACTIVE;
        success_count++;
    } else {
        global_state.process_module_status = MODULE_STATUS_ERROR;
    }

    int usb_devices = 0, suspicious_usb = 0, total_files = 0, files_with_changes = 0;
    if (get_usb_statistics_for_gui(&usb_devices, &suspicious_usb, &total_files, &files_with_changes) == 0) {
        global_state.aggregate_stats.total_usb_devices = usb_devices;
        global_state.aggregate_stats.suspicious_usb_devices = suspicious_usb;
        global_state.aggregate_stats.total_files_monitored = total_files;
        global_state.aggregate_stats.files_with_changes = files_with_changes;
        global_state.aggregate_stats.last_usb_scan = time(NULL);
        if (is_usb_monitoring_active()) {
            global_state.usb_module_status = is_gui_usb_scan_in_progress() ? MODULE_STATUS_SCANNING : MODULE_STATUS_ACTIVE;
        } else {
            global_state.usb_module_status = MODULE_STATUS_INACTIVE;
        }
        success_count++;
    } else {
        global_state.usb_module_status = MODULE_STATUS_ERROR;
    }

    int open_ports = 0, suspicious_ports = 0;
    time_t last_port_scan;
    if (get_port_statistics_for_gui(&open_ports, &suspicious_ports, &last_port_scan) == 0) {
        global_state.aggregate_stats.open_ports_found = open_ports;
        global_state.aggregate_stats.suspicious_ports = suspicious_ports;
        global_state.aggregate_stats.last_port_scan = last_port_scan;
        global_state.ports_module_status = is_port_scan_active() ? MODULE_STATUS_SCANNING : MODULE_STATUS_ACTIVE;
        success_count++;
    } else {
        global_state.ports_module_status = MODULE_STATUS_ERROR;
    }

    global_state.aggregate_stats.statistics_last_updated = time(NULL);

    if (success_count == total_modules) return 0;
    if (success_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Solo %d de %d modulos respondieron correctamente", success_count, total_modules);
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", msg);
        return 0;
    }
    gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Ningun modulo respondio - posible fallo del sistema");
    return -1;
}

int detect_cross_module_correlations(void) {
    int correlations_detected = 0;

    if (global_state.aggregate_stats.suspicious_processes > 0 &&
        global_state.aggregate_stats.suspicious_usb_devices > 0) {
        time_t diff = labs((long)(global_state.aggregate_stats.last_process_scan - global_state.aggregate_stats.last_usb_scan));
        if (diff <= 300) {
            gui_add_log_entry("CORRELATION_ANALYSIS", "ALERT", "Correlacion detectada: actividad sospechosa simultanea en procesos y USB");
            correlations_detected++;
        }
    }

    if (global_state.aggregate_stats.open_ports_found > 20 &&
        global_state.aggregate_stats.processes_exceeding_cpu_threshold > 5) {
        gui_add_log_entry("CORRELATION_ANALYSIS", "WARNING", "Correlacion detectada: alta actividad de red con uso intensivo de CPU");
        correlations_detected++;
    }

    if (global_state.aggregate_stats.suspicious_ports > 0 &&
        global_state.aggregate_stats.files_with_changes > 10) {
        gui_add_log_entry("CORRELATION_ANALYSIS", "ALERT", "Correlacion detectada: puertos sospechosos con modificacion masiva de archivos");
        correlations_detected++;
    }

    int modules_with_errors = 0;
    if (global_state.process_module_status == MODULE_STATUS_ERROR) modules_with_errors++;
    if (global_state.usb_module_status == MODULE_STATUS_ERROR) modules_with_errors++;
    if (global_state.ports_module_status == MODULE_STATUS_ERROR) modules_with_errors++;
    if (modules_with_errors >= 2) {
        gui_add_log_entry("CORRELATION_ANALYSIS", "CRITICAL", "Correlacion detectada: fallo multiple de modulos - posible ataque al sistema");
        correlations_detected++;
    }

    return correlations_detected;
}

// ============================================================================
// PUENTE HACIA LA GUI (el handler corre en el hilo principal de GTK)
// ============================================================================

static void on_coordinator_status_update(const ProgressUpdate *update, void *ui_user_data) {
    (void)ui_user_data;

    SystemGlobalState local_state;
    if (get_global_state_copy(&local_state) != 0) return;

    gui_update_statistics(local_state.aggregate_stats.total_usb_devices,
                           local_state.aggregate_stats.total_processes_monitored,
                           local_state.aggregate_stats.open_ports_found);
    gui_update_usb_suspicious_count(local_state.aggregate_stats.suspicious_usb_devices);

    gboolean system_healthy = (local_state.security_level <= SECURITY_LEVEL_MONITORING);
    gui_update_system_status(update->phase, system_healthy);

    if (local_state.security_level >= SECURITY_LEVEL_WARNING) {
        gboolean any_scanning = (local_state.process_module_status == MODULE_STATUS_SCANNING ||
                                  local_state.usb_module_status == MODULE_STATUS_SCANNING ||
                                  local_state.ports_module_status == MODULE_STATUS_SCANNING);
        gui_set_scanning_status(any_scanning);
    }

    pthread_mutex_lock(&global_state.state_mutex);
    global_state.performance_metrics.gui_updates_sent++;
    pthread_mutex_unlock(&global_state.state_mutex);
}

static void coordinator_work(volatile sig_atomic_t *cancel, void *user_data) {
    (void)cancel;
    (void)user_data;

    pthread_mutex_lock(&global_state.state_mutex);

    if (update_aggregate_statistics() != 0) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", "Error al actualizar estadisticas agregadas");
    }

    SystemSecurityLevel previous_level = global_state.security_level;
    global_state.security_level = evaluate_system_security_level();
    global_state.last_security_evaluation = time(NULL);

    if (previous_level != global_state.security_level) {
        const char *level_names[] = {"SAFE", "MONITORING", "WARNING", "CRITICAL", "UNKNOWN"};
        char msg[512];
        snprintf(msg, sizeof(msg), "Nivel de seguridad cambio de %s a %s",
                 level_names[previous_level], level_names[global_state.security_level]);
        gui_add_log_entry("SECURITY_EVALUATION",
                           global_state.security_level >= SECURITY_LEVEL_WARNING ? "ALERT" : "INFO", msg);
    }

    char description[512];
    strncpy(description, global_state.security_description, sizeof(description) - 1);
    description[sizeof(description) - 1] = '\0';

    global_state.performance_metrics.coordination_cycles_completed++;

    // detect_cross_module_correlations() reads aggregate_stats/*_module_status
    // fields with no locking of its own, so it must run before the unlock
    // below -- the old (deleted) coordinator called it while still holding
    // this same mutex, and request_immediate_system_evaluation() can invoke
    // coordinator_work() synchronously from the main thread while the
    // periodic worker thread is mid-cycle here, so those fields are genuinely
    // shared across threads and not just within this function.
    int correlations = detect_cross_module_correlations();
    if (correlations > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Detectadas %d correlaciones de seguridad entre modulos", correlations);
        gui_add_log_entry("CORRELATION_ANALYSIS", "WARNING", msg);
    }

    pthread_mutex_unlock(&global_state.state_mutex);

    ProgressUpdate update = { .phase = description, .current = 0, .total = 0 };
    gui_thread_bridge_post(&update, &coordinator_bridge_target);
}

// ============================================================================
// CICLO DE VIDA
// ============================================================================

int init_system_coordinator(void) {
    memset(&global_state, 0, sizeof(SystemGlobalState));
    global_state.security_level = SECURITY_LEVEL_UNKNOWN;
    strncpy(global_state.security_description, "Sistema iniciando - evaluacion pendiente",
            sizeof(global_state.security_description) - 1);
    global_state.process_module_status = MODULE_STATUS_INACTIVE;
    global_state.usb_module_status = MODULE_STATUS_INACTIVE;
    global_state.ports_module_status = MODULE_STATUS_INACTIVE;
    global_state.performance_metrics.system_start_time = time(NULL);

    if (pthread_mutex_init(&global_state.state_mutex, NULL) != 0) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Error al inicializar mutex del estado global");
        return -1;
    }

    coordinator_bridge_target.handler = on_coordinator_status_update;
    coordinator_bridge_target.ui_user_data = NULL;
    coordinator_bridge_target.watch = NULL; // no hay un widget unico que observar; el handler relee el estado global

    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Sistema coordinador inicializado exitosamente");
    return 0;
}

int start_system_coordinator(int update_interval_seconds_param) {
    if (coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", "Coordinador ya esta activo");
        return 0;
    }

    int interval = (update_interval_seconds_param >= 1 && update_interval_seconds_param <= 300)
                   ? update_interval_seconds_param : 5;

    pthread_mutex_lock(&global_state.state_mutex);
    update_aggregate_statistics();
    global_state.security_level = evaluate_system_security_level();
    global_state.last_security_evaluation = time(NULL);
    pthread_mutex_unlock(&global_state.state_mutex);

    coordinator_worker = gui_periodic_worker_start(interval, coordinator_work, NULL);
    if (!coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Error al iniciar el coordinador del sistema");
        return -1;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Coordinador del sistema iniciado (intervalo: %d segundos)", interval);
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", msg);
    return 0;
}

int stop_system_coordinator(void) {
    if (!coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Coordinador no esta activo");
        return 0;
    }
    gui_periodic_worker_stop(coordinator_worker); // señala + join, casi instantaneo
    coordinator_worker = NULL;
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Coordinador detenido exitosamente");
    return 0;
}

void cleanup_system_coordinator(void) {
    if (coordinator_worker) {
        stop_system_coordinator();
    }
    // stop_system_coordinator() -> gui_periodic_worker_stop() has already
    // joined the worker thread by this point, so no thread can be inside
    // coordinator_work() anymore. But coordinator_work()'s last tick may have
    // called gui_thread_bridge_post(), which queues its delivery on GTK's
    // main context via g_idle_add() instead of running it immediately; that
    // queued on_coordinator_status_update() call could still be pending here.
    // coordinator_bridge_target.watch is NULL (there's no single widget whose
    // lifetime the coordinator's updates are tied to, unlike the panels), so
    // nothing skips that idle callback if it is dispatched after this mutex
    // is destroyed below -- it would call get_global_state_copy(), which
    // locks an already-destroyed mutex: undefined behavior. Same class of
    // narrow, end-of-process-only hazard as the abandoned on_usb_scan_done_idle
    // in gui_usb_panel.c's shutdown (see its comment): today it isn't
    // reachable because on_window_destroy calls cleanup_system_coordinator()
    // and then gtk_main_quit() without pumping the main loop in between, so
    // any idle queued by this final tick never actually gets dispatched. It
    // would only become reachable if shutdown started pumping the main loop
    // (e.g. via gtk_main_iteration()) between stopping the coordinator and
    // this destroy -- not the case today, so this is documented rather than
    // engineered around with a watch object or extra synchronization.
    pthread_mutex_destroy(&global_state.state_mutex);
    memset(&global_state, 0, sizeof(SystemGlobalState));
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Recursos del coordinador liberados correctamente");
}

// ============================================================================
// ACCESO THREAD-SAFE AL ESTADO GLOBAL
// ============================================================================

int get_global_state_copy(SystemGlobalState *state_copy) {
    if (!state_copy) return -1;
    pthread_mutex_lock(&global_state.state_mutex);
    *state_copy = global_state;
    pthread_mutex_unlock(&global_state.state_mutex);
    return 0;
}

SystemSecurityLevel get_current_security_level(void) {
    pthread_mutex_lock(&global_state.state_mutex);
    SystemSecurityLevel level = global_state.security_level;
    pthread_mutex_unlock(&global_state.state_mutex);
    return level;
}

int get_consolidated_statistics(int *total_devices, int *total_processes,
                               int *total_open_ports, int *security_alerts) {
    if (!total_devices || !total_processes || !total_open_ports || !security_alerts) return -1;

    pthread_mutex_lock(&global_state.state_mutex);
    *total_devices = global_state.aggregate_stats.total_usb_devices;
    *total_processes = global_state.aggregate_stats.total_processes_monitored;
    *total_open_ports = global_state.aggregate_stats.open_ports_found;
    *security_alerts = global_state.aggregate_stats.suspicious_processes +
                      global_state.aggregate_stats.suspicious_usb_devices +
                      global_state.aggregate_stats.suspicious_ports;
    pthread_mutex_unlock(&global_state.state_mutex);
    return 0;
}

// ============================================================================
// NOTIFICACIONES ENTRE MODULOS
// ============================================================================

int notify_security_event(const char *source_module, int event_severity, const char *event_description) {
    if (!source_module || !event_description) return -1;
    if (event_severity < 1 || event_severity > 10) event_severity = 5;

    char msg[1024];
    snprintf(msg, sizeof(msg), "Evento de seguridad reportado por %s (severidad %d/10): %s",
             source_module, event_severity, event_description);
    gui_add_log_entry("SECURITY_EVENTS", event_severity >= 8 ? "ALERT" : event_severity >= 6 ? "WARNING" : "INFO", msg);

    if (event_severity >= 7) {
        request_immediate_system_evaluation();
    }
    return 0;
}

int notify_module_status_change(const char *module_name, ModuleStatus new_status, const char *status_description) {
    if (!module_name || !status_description) return -1;

    pthread_mutex_lock(&global_state.state_mutex);
    if (strcmp(module_name, "process") == 0) global_state.process_module_status = new_status;
    else if (strcmp(module_name, "usb") == 0) global_state.usb_module_status = new_status;
    else if (strcmp(module_name, "ports") == 0) global_state.ports_module_status = new_status;
    pthread_mutex_unlock(&global_state.state_mutex);

    const char *status_names[] = {"INACTIVE", "ACTIVE", "SCANNING", "ERROR", "MAINTENANCE"};
    char msg[512];
    snprintf(msg, sizeof(msg), "Modulo %s cambio estado a %s: %s", module_name, status_names[new_status], status_description);
    gui_add_log_entry("MODULE_STATUS", new_status == MODULE_STATUS_ERROR ? "ERROR" : "INFO", msg);
    return 0;
}

int update_coordinator_configuration(int update_interval, int security_evaluation_sensitivity_param) {
    if (update_interval < 1 || update_interval > 300) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Intervalo de actualizacion invalido (1-300 segundos)");
        return -1;
    }
    if (security_evaluation_sensitivity_param < 1 || security_evaluation_sensitivity_param > 10) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "ERROR", "Sensibilidad de evaluacion invalida (1-10)");
        return -1;
    }

    security_evaluation_sensitivity = security_evaluation_sensitivity_param;

    if (coordinator_worker) {
        // Reiniciar con el nuevo intervalo: el periodic worker no soporta
        // cambiar su propio intervalo en caliente.
        gui_periodic_worker_stop(coordinator_worker);
        coordinator_worker = gui_periodic_worker_start(update_interval, coordinator_work, NULL);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Configuracion coordinador actualizada: intervalo=%ds, sensibilidad=%d/10",
             update_interval, security_evaluation_sensitivity_param);
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", msg);
    return 0;
}

int request_immediate_system_evaluation(void) {
    if (!coordinator_worker) {
        gui_add_log_entry("SYSTEM_COORDINATOR", "WARNING", "No se puede evaluar: coordinador no esta activo");
        return -1;
    }
    // gui_periodic_worker no expone una senal de "despertar ya" -- una
    // evaluacion inmediata simplemente corre el mismo trabajo aca mismo,
    // en el hilo del llamador; coordinator_work() solo toca estado protegido
    // por mutex y hace un post seguro por el bridge, asi que es seguro
    // llamarla desde cualquier hilo, incluido el principal.
    coordinator_work(NULL, NULL);
    gui_add_log_entry("SYSTEM_COORDINATOR", "INFO", "Evaluacion inmediata del sistema completada");
    return 0;
}
