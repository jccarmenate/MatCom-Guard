// src/gui/panels/gui_process_panel.c
#include "gui_process_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include "gui_backend_adapters.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "process_monitor.h"
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget *dot;
    GtkWidget *primary;
    GtkWidget *secondary;
    GtkWidget *badge;
} ProcessRowRefs;

static GtkWidget *process_list_box = NULL;
static GtkWidget *process_dial = NULL;
static GtkWidget *process_kill_button = NULL;

static ProcessCallbacks process_backend_callbacks;
static GuiThreadBridgeTarget process_progress_target;

// ============================================================================
// FILA DE LISTA
// ============================================================================

static void apply_process_status_classes(GtkWidget *dot, GtkWidget *badge, const char *level) {
    GtkStyleContext *dot_ctx = gtk_widget_get_style_context(dot);
    GtkStyleContext *badge_ctx = gtk_widget_get_style_context(badge);

    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-normal");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-warning");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-critical");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-normal");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-warning");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-critical");
    gtk_style_context_add_class(badge_ctx, "nw-row-badge");

    char dot_class[32], badge_class[32];
    snprintf(dot_class, sizeof(dot_class), "nw-row-dot-%s", level);
    snprintf(badge_class, sizeof(badge_class), "nw-row-badge-%s", level);
    gtk_style_context_add_class(dot_ctx, dot_class);
    gtk_style_context_add_class(badge_ctx, badge_class);
}

static GtkWidget *find_row_by_pid(pid_t pid) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(process_list_box));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL; l = l->next) {
        pid_t row_pid = (pid_t)(intptr_t)g_object_get_data(G_OBJECT(l->data), "pid");
        if (row_pid == pid) { found = GTK_WIDGET(l->data); break; }
    }
    g_list_free(children);
    return found;
}

static void on_process_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)data;
    gtk_widget_set_sensitive(process_kill_button, row != NULL);
}

// gui_update_process() (gui.h) solo debe llamarse desde el hilo principal --
// las cuatro callbacks de ProcessCallbacks pasan por post_process_update().
void gui_update_process(GUIProcess *process) {
    if (!process_list_box || !process) return;

    gdouble cpu_threshold = get_cpu_threshold();
    gdouble mem_threshold = get_mem_threshold();

    const char *status;
    const char *level;
    if (process->is_whitelisted) {
        status = "Whitelisted"; level = "normal";
    } else if (process->is_suspicious) {
        status = "SOSPECHOSO"; level = "critical";
    } else if (process->cpu_usage > cpu_threshold && process->mem_usage > mem_threshold) {
        status = "Alto CPU+RAM"; level = "critical";
    } else if (process->cpu_usage > cpu_threshold) {
        status = "Alto CPU"; level = "warning";
    } else if (process->mem_usage > mem_threshold) {
        status = "Alta Memoria"; level = "warning";
    } else {
        status = "Normal"; level = "normal";
    }

    GtkWidget *row = find_row_by_pid(process->pid);
    ProcessRowRefs *refs;

    if (!row) {
        row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "nw-list-row");

        GtkWidget *dot = gtk_label_new("\xE2\x97\x8F");
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *primary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(primary), "nw-row-primary");
        gtk_widget_set_halign(primary, GTK_ALIGN_START);
        GtkWidget *secondary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(secondary), "nw-row-secondary");
        gtk_widget_set_halign(secondary, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(text_box), primary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), secondary, FALSE, FALSE, 0);
        GtkWidget *badge = gtk_label_new("");

        gtk_box_pack_start(GTK_BOX(hbox), dot, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), text_box, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), badge, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);

        refs = malloc(sizeof(ProcessRowRefs));
        if (!refs) {
            gtk_widget_destroy(row);
            return;
        }
        refs->dot = dot;
        refs->primary = primary;
        refs->secondary = secondary;
        refs->badge = badge;
        g_object_set_data(G_OBJECT(row), "pid", GINT_TO_POINTER(process->pid));
        g_object_set_data_full(G_OBJECT(row), "row-refs", refs, free);

        gtk_list_box_insert(GTK_LIST_BOX(process_list_box), row, -1);
        gtk_widget_show_all(row);
    } else {
        refs = g_object_get_data(G_OBJECT(row), "row-refs");
    }

    gtk_label_set_text(GTK_LABEL(refs->primary), process->name);
    char secondary_text[128];
    snprintf(secondary_text, sizeof(secondary_text), "PID %d . CPU %.1f%% . MEM %.1f%%",
             process->pid, process->cpu_usage, process->mem_usage);
    gtk_label_set_text(GTK_LABEL(refs->secondary), secondary_text);
    gtk_label_set_text(GTK_LABEL(refs->badge), status);
    apply_process_status_classes(refs->dot, refs->badge, level);

    gboolean exceeds = process->cpu_usage > cpu_threshold || process->mem_usage > mem_threshold;
    if ((process->is_suspicious || exceeds) && !process->is_whitelisted) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Proceso '%s' (PID: %d) - CPU: %.1f%%, RAM: %.1f%% - Estado: %s",
                 process->name, process->pid, process->cpu_usage, process->mem_usage, status);
        gui_add_log_entry("PROCESS_MONITOR", process->is_suspicious ? "ALERT" : "WARNING", log_msg);
    } else if ((process->is_suspicious || exceeds) && process->is_whitelisted) {
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Proceso whitelisted '%s' (PID: %d) - CPU: %.1f%%, RAM: %.1f%% - Estado: %s",
                 process->name, process->pid, process->cpu_usage, process->mem_usage, status);
        gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
    }
}

typedef struct { GUIProcess process; } ProcessUpdateMsg;

static gboolean process_update_idle(gpointer data) {
    ProcessUpdateMsg *msg = (ProcessUpdateMsg *)data;
    gui_update_process(&msg->process);
    free(msg);
    return G_SOURCE_REMOVE;
}

static void post_process_update(const GUIProcess *process) {
    ProcessUpdateMsg *msg = malloc(sizeof(ProcessUpdateMsg));
    if (!msg) return;
    msg->process = *process;
    g_idle_add(process_update_idle, msg);
}

// ============================================================================
// CALLBACKS DEL BACKEND (corren en el hilo de monitoreo de process_monitor.c)
// ============================================================================

static void on_backend_new_process(ProcessInfo *info) {
    if (!info) return;
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Nuevo proceso detectado: %s (PID: %d) - CPU: %.1f%%, MEM: %.1f%%",
             info->name, info->pid, info->cpu_usage, info->mem_usage);
    gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
}

static void on_backend_process_terminated(pid_t pid, const char *name) {
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Proceso terminado: %s (PID: %d)", name ? name : "desconocido", pid);
    gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
}

static void on_backend_high_cpu_alert(ProcessInfo *info) {
    if (!info) return;
    if (info->is_whitelisted) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Intento de alerta CPU para proceso whitelisted '%s' (PID: %d)", info->name, info->pid);
        gui_add_log_entry("PROCESS_MONITOR", "WARNING", msg);
        return;
    }
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "ALERTA CPU: Proceso '%s' (PID: %d) usando %.1f%% de CPU",
             info->name, info->pid, info->cpu_usage);
    gui_add_log_entry("PROCESS_MONITOR", "ALERT", log_msg);
}

static void on_backend_high_memory_alert(ProcessInfo *info) {
    if (!info) return;
    if (info->is_whitelisted) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Intento de alerta RAM para proceso whitelisted '%s' (PID: %d)", info->name, info->pid);
        gui_add_log_entry("PROCESS_MONITOR", "WARNING", msg);
        return;
    }
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "ALERTA MEMORIA: Proceso '%s' (PID: %d) usando %.1f%% de RAM",
             info->name, info->pid, info->mem_usage);
    gui_add_log_entry("PROCESS_MONITOR", "ALERT", log_msg);
}

static void on_backend_alert_cleared(ProcessInfo *info) {
    if (!info) return;
    GUIProcess gp;
    if (adapt_process_info_to_gui(info, &gp) != 0) return;
    post_process_update(&gp);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Alerta despejada: Proceso '%s' (PID: %d) volvio a valores normales",
             info->name, info->pid);
    gui_add_log_entry("PROCESS_MONITOR", "INFO", log_msg);
}

static void on_process_status_update(const ProgressUpdate *update, void *ui_user_data) {
    guard_dial_set_progress((GtkWidget *)ui_user_data, update->current, update->total, update->phase);
}

// ============================================================================
// BOTONES
// ============================================================================

static int process_sync_from_backend(void) {
    int count = 0;
    ProcessInfo *list = get_process_list_copy(&count);
    if (!list) {
        gui_add_log_entry("PROCESS_INTEGRATION", "INFO", "No hay procesos monitoreados para sincronizar");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        GUIProcess gp;
        if (adapt_process_info_to_gui(&list[i], &gp) == 0) {
            gui_update_process(&gp); // ya estamos en el hilo principal (boton o intelligent_system_sync)
        }
    }
    free(list);
    return count;
}

static void on_scan_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    if (!is_monitoring_active()) {
        gui_add_log_entry("PROCESS_SCANNER", "INFO", "Iniciando monitoreo de procesos del sistema");
        if (start_monitoring() != 0) {
            gui_add_log_entry("PROCESS_SCANNER", "ERROR", "No se pudo iniciar el monitoreo de procesos");
            return;
        }
        guard_dial_set_progress(process_dial, 0, 0, "Monitoreo activo");
    } else {
        gui_add_log_entry("PROCESS_SCANNER", "INFO", "Actualizando vista de procesos...");
        process_sync_from_backend();
    }
}

static void on_kill_process_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(process_list_box));
    if (!row) return;

    pid_t pid = (pid_t)(intptr_t)g_object_get_data(G_OBJECT(row), "pid");
    ProcessRowRefs *refs = g_object_get_data(G_OBJECT(row), "row-refs");
    char name[256];
    utf8_safe_truncate(gtk_label_get_text(GTK_LABEL(refs->primary)), name, sizeof(name));

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
        "Esta seguro de que desea terminar el proceso '%s' (PID: %d)?", name, pid);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_YES) return;

    char log_msg[512];
    if (kill(pid, SIGTERM) == 0) {
        snprintf(log_msg, sizeof(log_msg), "Senal SIGTERM enviada a '%s' (PID: %d)", name, pid);
        gui_add_log_entry("PROCESS_MANAGER", "WARNING", log_msg);
    } else {
        snprintf(log_msg, sizeof(log_msg), "No se pudo terminar '%s' (PID: %d): %s", name, pid, strerror(errno));
        gui_add_log_entry("PROCESS_MANAGER", "ERROR", log_msg);
    }

    gtk_widget_destroy(GTK_WIDGET(row));
    gtk_widget_set_sensitive(process_kill_button, FALSE);
}

// ============================================================================
// CICLO DE VIDA DEL PANEL
// ============================================================================

GtkWidget *gui_process_panel_create(void) {
    load_config();

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new("Procesos del Sistema");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new(""), TRUE, TRUE, 0);

    process_kill_button = gtk_button_new_with_label("Terminar Proceso");
    gtk_style_context_add_class(gtk_widget_get_style_context(process_kill_button), "nw-button-secondary");
    gtk_widget_set_sensitive(process_kill_button, FALSE);
    g_signal_connect(process_kill_button, "clicked", G_CALLBACK(on_kill_process_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), process_kill_button, FALSE, FALSE, 0);

    GtkWidget *scan_button = gtk_button_new_with_label("Escanear Procesos");
    gtk_style_context_add_class(gtk_widget_get_style_context(scan_button), "nw-button-primary");
    g_signal_connect(scan_button, "clicked", G_CALLBACK(on_scan_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), scan_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), toolbar, FALSE, FALSE, 0);

    process_dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(panel), process_dial, FALSE, FALSE, 0);
    process_progress_target.handler = on_process_status_update;
    process_progress_target.ui_user_data = process_dial;
    process_progress_target.watch = G_OBJECT(process_dial);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    process_list_box = gtk_list_box_new();
    g_signal_connect(process_list_box, "row-selected", G_CALLBACK(on_process_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), process_list_box);
    gtk_box_pack_start(GTK_BOX(panel), scrolled, TRUE, TRUE, 0);

    memset(&process_backend_callbacks, 0, sizeof(process_backend_callbacks));
    process_backend_callbacks.on_new_process = on_backend_new_process;
    process_backend_callbacks.on_process_terminated = on_backend_process_terminated;
    process_backend_callbacks.on_high_cpu_alert = on_backend_high_cpu_alert;
    process_backend_callbacks.on_high_memory_alert = on_backend_high_memory_alert;
    process_backend_callbacks.on_alert_cleared = on_backend_alert_cleared;
    process_backend_callbacks.on_status_update = gui_thread_bridge_post;
    process_backend_callbacks.status_user_data = &process_progress_target;
    set_process_callbacks(&process_backend_callbacks);

    return panel;
}

void gui_process_panel_shutdown(void) {
    if (is_monitoring_active()) {
        stop_monitoring();
    }
    cleanup_monitoring();
}

// ============================================================================
// COMPATIBILIDAD CON EL COORDINADOR, intelligent_system_sync Y EL MENU "ESCANEAR"
// ============================================================================

int is_process_monitoring_active(void) {
    return is_monitoring_active();
}

int get_process_statistics_for_gui(int *total_processes, int *high_cpu_count,
                                   int *high_memory_count, int *suspicious_count) {
    if (!total_processes || !high_cpu_count || !high_memory_count || !suspicious_count) {
        return -1;
    }
    MonitoringStats stats = get_monitoring_stats();
    *total_processes = stats.total_processes;
    *high_cpu_count = stats.high_cpu_count;
    *high_memory_count = stats.high_memory_count;
    *suspicious_count = stats.active_alerts;
    return 0;
}

int sync_gui_with_backend_processes(void) {
    return process_sync_from_backend();
}

void gui_compatible_scan_processes(void) {
    if (!is_monitoring_active()) {
        if (start_monitoring() != 0) {
            gui_add_log_entry("PROCESS_INTEGRATION", "ERROR", "Error al iniciar monitoreo de procesos");
            return;
        }
        guard_dial_set_progress(process_dial, 0, 0, "Monitoreo activo");
    } else {
        process_sync_from_backend();
    }
}
