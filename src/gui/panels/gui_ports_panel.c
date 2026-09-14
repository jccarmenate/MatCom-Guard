// src/gui/panels/gui_ports_panel.c
#include "gui_ports_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include "gui_backend_adapters.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "port_scanner.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PORT_SCAN_THREADS 50

typedef struct {
    GtkWidget *dot;
    GtkWidget *primary;
    GtkWidget *secondary;
    GtkWidget *badge;
} PortRowRefs;

typedef struct {
    int port;
    const char *description;
} PortDescription;

// Solo texto informativo para el panel de detalle -- la clasificacion real
// de sospecha viene de is_port_suspicious()/get_port_service_name() dentro
// de scan_ports_range(), no de esta tabla.
static const PortDescription port_descriptions[] = {
    {20, "FTP Data Transfer"}, {21, "File Transfer Protocol"}, {22, "Secure Shell"},
    {23, "Telnet (inseguro)"}, {25, "Simple Mail Transfer"}, {53, "Domain Name System"},
    {80, "Web Server"}, {110, "Post Office Protocol"}, {143, "Internet Message Access"},
    {443, "Secure Web Server"}, {445, "Server Message Block"}, {3306, "MySQL Database"},
    {5432, "PostgreSQL Database"}, {8080, "Alternative HTTP"}, {8443, "Alternative HTTPS"},
    {3389, "Remote Desktop"}, {5900, "Virtual Network Computing"}, {6379, "Redis Database"},
    {27017, "MongoDB Database"}, {31337, "Elite - Backdoor comun"}, {4444, "Metasploit default"},
    {6666, "IRC - a menudo usado por botnets"}, {6667, "IRC - a menudo usado por botnets"},
    {12345, "NetBus backdoor"},
    {0, NULL}
};

static const char *get_port_description(int port) {
    for (int i = 0; port_descriptions[i].description != NULL; i++) {
        if (port_descriptions[i].port == port) return port_descriptions[i].description;
    }
    return "Sin descripcion disponible";
}

static GtkWidget *ports_list_box = NULL;
static GtkWidget *ports_dial = NULL;
static GtkWidget *ports_detail_label = NULL;
static GtkWidget *port_start_spin = NULL;
static GtkWidget *port_end_spin = NULL;
static GtkWidget *ports_scan_button = NULL;
static GtkWidget *ports_quick_button = NULL;
static GtkWidget *ports_full_button = NULL;
static GtkWidget *ports_cancel_button = NULL;

static pthread_t port_scan_thread;
static volatile sig_atomic_t port_cancel_flag = 0;
static volatile sig_atomic_t port_scan_running = 0;
static int port_scan_start_value = 1;
static int port_scan_end_value = 1024;

// Protege las tres estadisticas de abajo: se escriben en on_port_scan_done_idle
// (hilo principal, via idle callback de GLib) y se leen desde
// get_port_statistics_for_gui(), que el coordinador del sistema invoca desde
// su propio hilo de fondo -- sin este mutex serian una carrera de datos entre
// hilos, igual que el equivalente usb_stats_mutex en gui_usb_panel.c.
static pthread_mutex_t ports_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static int port_last_total_open = 0;
static int port_last_total_suspicious = 0;
static time_t port_last_scan_time = 0;

// ============================================================================
// FILA DE LISTA
// ============================================================================

static void apply_port_status_classes(GtkWidget *dot, GtkWidget *badge, const char *level) {
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

static GtkWidget *find_row_by_port(int port) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(ports_list_box));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL; l = l->next) {
        int row_port = (int)(intptr_t)g_object_get_data(G_OBJECT(l->data), "port");
        if (row_port == port) { found = GTK_WIDGET(l->data); break; }
    }
    g_list_free(children);
    return found;
}

static void on_port_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)data;
    if (!row) return;
    int port = (int)(intptr_t)g_object_get_data(G_OBJECT(row), "port");
    PortRowRefs *refs = g_object_get_data(G_OBJECT(row), "row-refs");
    if (!refs) return;

    char detail[512];
    snprintf(detail, sizeof(detail), "<b>%s</b>\n%s\n\n<i>%s</i>",
             gtk_label_get_text(GTK_LABEL(refs->primary)),
             gtk_label_get_text(GTK_LABEL(refs->secondary)),
             get_port_description(port));
    gtk_label_set_markup(GTK_LABEL(ports_detail_label), detail);
}

// Llamada solo desde el hilo principal: on_port_scan_done_idle (idle
// callback de GLib) ya corre ahi, asi que no hace falta bridging adicional.
void gui_update_port(GUIPort *port) {
    if (!ports_list_box || !port) return;

    GtkWidget *row = find_row_by_port(port->port);
    PortRowRefs *refs;

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

        refs = malloc(sizeof(PortRowRefs));
        if (!refs) {
            gtk_widget_destroy(row);
            return;
        }
        refs->dot = dot;
        refs->primary = primary;
        refs->secondary = secondary;
        refs->badge = badge;
        g_object_set_data(G_OBJECT(row), "port", GINT_TO_POINTER(port->port));
        g_object_set_data_full(G_OBJECT(row), "row-refs", refs, free);

        gtk_list_box_insert(GTK_LIST_BOX(ports_list_box), row, -1);
        gtk_widget_show_all(row);
    } else {
        refs = g_object_get_data(G_OBJECT(row), "row-refs");
    }

    char primary_text[192];
    snprintf(primary_text, sizeof(primary_text), "Puerto %d - %s", port->port, port->service);
    gtk_label_set_text(GTK_LABEL(refs->primary), primary_text);

    char secondary_text[64];
    snprintf(secondary_text, sizeof(secondary_text), "puerto %d/tcp", port->port);
    gtk_label_set_text(GTK_LABEL(refs->secondary), secondary_text);

    gtk_label_set_text(GTK_LABEL(refs->badge), port->is_suspicious ? "SOSPECHOSO" : "Abierto");
    apply_port_status_classes(refs->dot, refs->badge, port->is_suspicious ? "critical" : "normal");

    if (port->is_suspicious) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "Puerto sospechoso: %d/tcp (%s)", port->port, port->service);
        gui_add_log_entry("PORT_SCANNER", "WARNING", log_msg);
    }
}

// ============================================================================
// ESCANEO (hilo real via scan_ports_range, mismo patron que gui_demo_scan)
// ============================================================================

static void on_port_scan_progress(const ProgressUpdate *update, void *ui_user_data) {
    guard_dial_set_progress((GtkWidget *)ui_user_data, update->current, update->total, update->phase);
}

typedef struct { ScanResult result; } PortScanDoneMessage;

static gboolean on_port_scan_done_idle(gpointer data) {
    pthread_join(port_scan_thread, NULL);
    PortScanDoneMessage *msg = (PortScanDoneMessage *)data;

    if (msg->result.ports) {
        for (int i = 0; i < msg->result.total_ports; i++) {
            if (msg->result.ports[i].is_open) {
                GUIPort gp;
                adapt_port_info_to_gui(&msg->result.ports[i], &gp);
                gui_update_port(&gp);
            }
        }
        pthread_mutex_lock(&ports_stats_mutex);
        aggregate_port_statistics(msg->result.ports, msg->result.total_ports,
                                   &port_last_total_open, &port_last_total_suspicious);
        pthread_mutex_unlock(&ports_stats_mutex);
        free(msg->result.ports);
    }
    pthread_mutex_lock(&ports_stats_mutex);
    port_last_scan_time = time(NULL);
    pthread_mutex_unlock(&ports_stats_mutex);

    char summary[128];
    if (port_cancel_flag) {
        snprintf(summary, sizeof(summary), "Cancelado (%d abiertos de %d)", msg->result.open_ports, msg->result.total_ports);
    } else {
        snprintf(summary, sizeof(summary), "Listo: %d abiertos de %d", msg->result.open_ports, msg->result.total_ports);
    }
    guard_dial_set_progress(ports_dial, msg->result.total_ports, msg->result.total_ports, summary);
    free(msg);

    gtk_widget_set_sensitive(ports_scan_button, TRUE);
    gtk_widget_set_sensitive(ports_quick_button, TRUE);
    gtk_widget_set_sensitive(ports_full_button, TRUE);
    gtk_widget_set_sensitive(ports_cancel_button, FALSE);
    port_scan_running = 0;

    return G_SOURCE_REMOVE;
}

static gboolean on_port_scan_start_failed_idle(gpointer data) {
    (void)data;
    pthread_join(port_scan_thread, NULL);
    gtk_widget_set_sensitive(ports_scan_button, TRUE);
    gtk_widget_set_sensitive(ports_quick_button, TRUE);
    gtk_widget_set_sensitive(ports_full_button, TRUE);
    gtk_widget_set_sensitive(ports_cancel_button, FALSE);
    guard_dial_set_progress(ports_dial, 0, 0, "No se pudo iniciar el escaneo");
    port_scan_running = 0;
    return G_SOURCE_REMOVE;
}

static void *port_scan_thread_main(void *arg) {
    (void)arg;
    GuiThreadBridgeTarget target = { .handler = on_port_scan_progress, .ui_user_data = ports_dial, .watch = G_OBJECT(ports_dial) };

    PortScanDoneMessage *msg = malloc(sizeof(PortScanDoneMessage));
    if (!msg) {
        g_idle_add(on_port_scan_start_failed_idle, NULL);
        return NULL;
    }

    int rc = scan_ports_range(port_scan_start_value, port_scan_end_value, PORT_SCAN_THREADS,
                               gui_thread_bridge_post, &target, &port_cancel_flag, &msg->result);
    if (rc != 0) {
        msg->result.ports = NULL;
        msg->result.total_ports = 0;
        msg->result.open_ports = 0;
        msg->result.suspicious_ports = 0;
    }

    g_idle_add(on_port_scan_done_idle, msg);
    return NULL;
}

static void clear_ports_list(void) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(ports_list_box));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

static void start_port_scan_with_range(int start, int end) {
    if (port_scan_running) {
        gui_add_log_entry("PORT_SCANNER", "WARNING", "Escaneo de puertos ya en progreso");
        return;
    }

    port_scan_start_value = start;
    port_scan_end_value = end;
    port_cancel_flag = 0;
    port_scan_running = 1;

    gtk_widget_set_sensitive(ports_scan_button, FALSE);
    gtk_widget_set_sensitive(ports_quick_button, FALSE);
    gtk_widget_set_sensitive(ports_full_button, FALSE);
    gtk_widget_set_sensitive(ports_cancel_button, TRUE);
    guard_dial_set_progress(ports_dial, 0, end - start + 1, "Iniciando escaneo de puertos...");
    clear_ports_list();

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Escaneando puertos %d-%d", start, end);
    gui_add_log_entry("PORT_SCANNER", "INFO", log_msg);

    if (pthread_create(&port_scan_thread, NULL, port_scan_thread_main, NULL) != 0) {
        gui_add_log_entry("PORT_SCANNER", "ERROR", "No se pudo crear el hilo de escaneo de puertos");
        port_scan_running = 0;
        gtk_widget_set_sensitive(ports_scan_button, TRUE);
        gtk_widget_set_sensitive(ports_quick_button, TRUE);
        gtk_widget_set_sensitive(ports_full_button, TRUE);
        gtk_widget_set_sensitive(ports_cancel_button, FALSE);
        guard_dial_set_progress(ports_dial, 0, 0, "No se pudo iniciar el escaneo");
    }
}

void on_scan_ports_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    int start = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(port_start_spin));
    int end = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(port_end_spin));
    start_port_scan_with_range(start, end);
}

static void on_quick_scan_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
    start_port_scan_with_range(1, 1024);
}

static void on_full_scan_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
        "El escaneo completo de 65535 puertos puede tomar mucho tiempo.\nEsta seguro de que desea continuar?");
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_YES) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 65535.0);
        start_port_scan_with_range(1, 65535);
    }
}

static void on_cancel_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    port_cancel_flag = 1;
}

// ============================================================================
// CICLO DE VIDA DEL PANEL
// ============================================================================

GtkWidget *gui_ports_panel_create(void) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new("Escaner de Puertos de Red");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new(""), TRUE, TRUE, 0);

    ports_cancel_button = gtk_button_new_with_label("Cancelar");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_cancel_button), "nw-button-secondary");
    gtk_widget_set_sensitive(ports_cancel_button, FALSE);
    g_signal_connect(ports_cancel_button, "clicked", G_CALLBACK(on_cancel_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), ports_cancel_button, FALSE, FALSE, 0);

    ports_scan_button = gtk_button_new_with_label("Escanear Rango");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_scan_button), "nw-button-primary");
    g_signal_connect(ports_scan_button, "clicked", G_CALLBACK(on_scan_ports_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), ports_scan_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), toolbar, FALSE, FALSE, 0);

    ports_dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(panel), ports_dial, FALSE, FALSE, 0);

    GtkWidget *range_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(range_row), gtk_label_new("Rango de puertos:"), FALSE, FALSE, 0);

    port_start_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
    gtk_box_pack_start(GTK_BOX(range_row), port_start_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(range_row), gtk_label_new("-"), FALSE, FALSE, 0);

    port_end_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
    gtk_box_pack_start(GTK_BOX(range_row), port_end_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(range_row), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);

    ports_quick_button = gtk_button_new_with_label("Escaneo Rapido (1-1024)");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_quick_button), "nw-button-secondary");
    g_signal_connect(ports_quick_button, "clicked", G_CALLBACK(on_quick_scan_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(range_row), ports_quick_button, FALSE, FALSE, 0);

    ports_full_button = gtk_button_new_with_label("Escaneo Completo (1-65535)");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_full_button), "nw-button-secondary");
    g_signal_connect(ports_full_button, "clicked", G_CALLBACK(on_full_scan_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(range_row), ports_full_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), range_row, FALSE, FALSE, 0);

    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    ports_list_box = gtk_list_box_new();
    g_signal_connect(ports_list_box, "row-selected", G_CALLBACK(on_port_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), ports_list_box);
    gtk_box_pack_start(GTK_BOX(content_box), scrolled, TRUE, TRUE, 0);

    ports_detail_label = gtk_label_new("Seleccione un puerto para ver detalles");
    gtk_style_context_add_class(gtk_widget_get_style_context(ports_detail_label), "nw-row-secondary");
    gtk_label_set_line_wrap(GTK_LABEL(ports_detail_label), TRUE);
    gtk_widget_set_size_request(ports_detail_label, 240, -1);
    gtk_widget_set_valign(ports_detail_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content_box), ports_detail_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), content_box, TRUE, TRUE, 0);

    return panel;
}

void gui_ports_panel_shutdown(void) {
    if (port_scan_running) {
        port_cancel_flag = 1;
        pthread_join(port_scan_thread, NULL);
        port_scan_running = 0;
    }
}

void gui_ports_panel_cancel_current_scan(void) {
    gui_ports_panel_shutdown();
}

// ============================================================================
// COMPATIBILIDAD CON EL COORDINADOR Y EL MENU "ESCANEAR"
// ============================================================================

int is_port_scan_active(void) {
    return port_scan_running;
}

int get_port_statistics_for_gui(int *total_open, int *total_suspicious, time_t *last_scan_time) {
    if (!total_open || !total_suspicious || !last_scan_time) {
        return -1;
    }
    pthread_mutex_lock(&ports_stats_mutex);
    *total_open = port_last_total_open;
    *total_suspicious = port_last_total_suspicious;
    *last_scan_time = port_last_scan_time;
    pthread_mutex_unlock(&ports_stats_mutex);
    return 0;
}

void gui_compatible_scan_ports(void) {
    if (port_scan_running) {
        gui_add_log_entry("PORT_SCANNER", "INFO", "Escaneo de puertos en progreso");
        return;
    }
    gui_add_log_entry("PORT_INTEGRATION", "INFO", "Iniciando escaneo rapido de puertos solicitado por usuario");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
    start_port_scan_with_range(1, 1024);
}
