#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include <time.h>
#include "gui.h"

// Variables principales de la ventana (definidas en gui_main.c)
extern GtkWidget *main_window;
extern GtkWidget *main_container;

// Variables de callbacks del backend (definidas en gui_main.c)
extern ScanUSBCallback usb_callback;
extern ScanProcessesCallback processes_callback;
extern ScanPortsCallback ports_callback;
extern ExportReportCallback report_callback;

// Variables específicas del sistema de logging (definidas en gui_logs_panel.c)
extern GtkWidget *log_text_view;
extern GtkTextBuffer *log_buffer;
extern GtkTextTag *info_tag;
extern GtkTextTag *warning_tag;
extern GtkTextTag *error_tag;
extern GtkTextTag *alert_tag;

// Variables específicas del panel de estadísticas (definidas en gui_dashboard_panel.c)
extern GtkWidget *stats_usb_count;
extern GtkWidget *stats_usb_suspicious;
extern GtkWidget *stats_process_count;
extern GtkWidget *stats_ports_open;
extern GtkWidget *stats_system_status;
extern GtkWidget *stats_last_scan;

// Funciones del panel de puertos (implementadas en gui_ports_panel.c)
void on_scan_ports_clicked(GtkButton *button, gpointer data);

// Funciones del diálogo de configuración (implementadas en gui_config_dialog.c)
void show_config_dialog(GtkWindow *parent);
gdouble get_cpu_threshold(void);
gdouble get_mem_threshold(void);

#endif