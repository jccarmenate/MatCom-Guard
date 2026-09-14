// src/gui/panels/gui_dashboard_panel.c
#include "gui_dashboard_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include <stdio.h>
#include <time.h>

GtkWidget *stats_usb_count = NULL;
GtkWidget *stats_usb_suspicious = NULL;
GtkWidget *stats_process_count = NULL;
GtkWidget *stats_ports_open = NULL;
GtkWidget *stats_system_status = NULL;
GtkWidget *stats_last_scan = NULL;

static GtkWidget *create_stat_card(const char *label_text, GtkWidget **value_widget) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "nw-stat-card");

    GtkWidget *value = gtk_label_new("0");
    gtk_style_context_add_class(gtk_widget_get_style_context(value), "nw-stat-value");
    gtk_widget_set_halign(value, GTK_ALIGN_START);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "nw-stat-label");
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(card), value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), label, FALSE, FALSE, 0);

    *value_widget = value;
    return card;
}

GtkWidget *gui_dashboard_panel_create(void) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *title = gtk_label_new("Estado del Sistema de Proteccion");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(panel), title, FALSE, FALSE, 0);

    GtkWidget *cards_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("Dispositivos USB", &stats_usb_count), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("USB Sospechosos", &stats_usb_suspicious), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("Procesos Monitoreados", &stats_process_count), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cards_row), create_stat_card("Puertos Abiertos", &stats_ports_open), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(panel), cards_row, FALSE, FALSE, 0);

    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    stats_system_status = gtk_label_new("Sistema Activo");
    gtk_style_context_add_class(gtk_widget_get_style_context(stats_system_status), "nw-row-primary");
    GtkWidget *status_label = gtk_label_new("Estado General");
    gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "nw-stat-label");
    gtk_box_pack_start(GTK_BOX(status_box), stats_system_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_box), status_label, FALSE, FALSE, 0);

    GtkWidget *scan_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    stats_last_scan = gtk_label_new("Nunca");
    gtk_style_context_add_class(gtk_widget_get_style_context(stats_last_scan), "nw-row-primary");
    GtkWidget *scan_label = gtk_label_new("Ultimo Escaneo");
    gtk_style_context_add_class(gtk_widget_get_style_context(scan_label), "nw-stat-label");
    gtk_box_pack_start(GTK_BOX(scan_box), stats_last_scan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(scan_box), scan_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(status_row), status_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_row), scan_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), status_row, FALSE, FALSE, 0);

    return panel;
}

static void set_value_class(GtkWidget *label, const char *value_class) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(label);
    gtk_style_context_remove_class(ctx, "nw-value-warning");
    gtk_style_context_remove_class(ctx, "nw-value-critical");
    if (value_class) {
        gtk_style_context_add_class(ctx, value_class);
    }
}

void gui_update_statistics(int usb_devices, int processes, int open_ports) {
    if (!stats_usb_count || !stats_process_count || !stats_ports_open) {
        return;
    }

    char text[32];

    snprintf(text, sizeof(text), "%d", usb_devices);
    gtk_label_set_text(GTK_LABEL(stats_usb_count), text);
    set_value_class(stats_usb_count, usb_devices > 5 ? "nw-value-warning" : NULL);

    snprintf(text, sizeof(text), "%d", processes);
    gtk_label_set_text(GTK_LABEL(stats_process_count), text);
    set_value_class(stats_process_count, processes > 50 ? "nw-value-critical" : NULL);

    snprintf(text, sizeof(text), "%d", open_ports);
    gtk_label_set_text(GTK_LABEL(stats_ports_open), text);
    const char *ports_class = NULL;
    if (open_ports > 20) ports_class = "nw-value-critical";
    else if (open_ports > 10) ports_class = "nw-value-warning";
    set_value_class(stats_ports_open, ports_class);

    // El conteo de USB sospechosos no llega por este agregado -- lo entrega
    // el coordinador del sistema via gui_update_usb_suspicious_count(), mas
    // abajo, a partir de local_state.aggregate_stats.suspicious_usb_devices.

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    if (stats_last_scan) {
        gtk_label_set_text(GTK_LABEL(stats_last_scan), timestamp);
    }

    char log_message[256];
    snprintf(log_message, sizeof(log_message),
             "Estadisticas actualizadas - USB: %d, Procesos: %d, Puertos: %d",
             usb_devices, processes, open_ports);
    gui_add_log_entry("ESTADISTICAS", "INFO", log_message);
}

void gui_update_usb_suspicious_count(int suspicious_count) {
    if (!stats_usb_suspicious) return;

    char text[32];
    snprintf(text, sizeof(text), "%d", suspicious_count);
    gtk_label_set_text(GTK_LABEL(stats_usb_suspicious), text);
    set_value_class(stats_usb_suspicious, suspicious_count > 0 ? "nw-value-critical" : NULL);
}
