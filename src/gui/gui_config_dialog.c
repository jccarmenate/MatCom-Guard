// src/gui/gui_config_dialog.c
#include "gui_config_dialog.h"
#include "gui_internal.h"
#include "gui.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    gdouble cpu_threshold;
    gdouble mem_threshold;
    gint usb_scan_interval;
    gint process_scan_interval;
    gint port_scan_interval;
    gboolean auto_scan_usb;
    gboolean auto_scan_processes;
    gboolean auto_scan_ports;
    gboolean enable_sound_alerts;
    gboolean enable_notifications;
    gboolean log_to_file;
    gint port_scan_start;
    gint port_scan_end;
    gchar *whitelist_processes;
} AppConfig;

static AppConfig config = {
    .cpu_threshold = 70.0,
    .mem_threshold = 50.0,
    .usb_scan_interval = 30,
    .process_scan_interval = 5,
    .port_scan_interval = 300,
    .auto_scan_usb = TRUE,
    .auto_scan_processes = TRUE,
    .auto_scan_ports = FALSE,
    .enable_sound_alerts = TRUE,
    .enable_notifications = TRUE,
    .log_to_file = TRUE,
    .port_scan_start = 1,
    .port_scan_end = 1024,
    .whitelist_processes = NULL
};

// A static initializer can't call g_strdup() (not a constant expression), but every
// g_free(config.whitelist_processes) in this file assumes a heap-allocated string. Leaving the
// field as a string literal here means the first Aplicar/Aceptar click on a fresh install (no
// prior config.ini) frees a literal and crashes glibc with "free(): invalid pointer" -- reproduced
// while manually testing this dialog. This one-time conversion keeps the same default text while
// guaranteeing the pointer is always heap-allocated before any g_free() call can touch it.
static void ensure_default_whitelist(void) {
    if (!config.whitelist_processes) {
        config.whitelist_processes = g_strdup("firefox,chrome,systemd,gnome-shell");
    }
}

static GtkWidget *config_dialog = NULL;
static GtkWidget *cpu_threshold_spin = NULL;
static GtkWidget *mem_threshold_spin = NULL;
static GtkWidget *usb_interval_spin = NULL;
static GtkWidget *process_interval_spin = NULL;
static GtkWidget *port_interval_spin = NULL;
static GtkWidget *auto_usb_check = NULL;
static GtkWidget *auto_process_check = NULL;
static GtkWidget *auto_port_check = NULL;
static GtkWidget *sound_alerts_check = NULL;
static GtkWidget *notifications_check = NULL;
static GtkWidget *log_file_check = NULL;
static GtkWidget *port_start_spin = NULL;
static GtkWidget *port_end_spin = NULL;
static GtkWidget *whitelist_entry = NULL;

static void save_config_to_file(void) {
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "matcom-guard", "config.ini", NULL);
    gchar *config_dir = g_path_get_dirname(config_path);
    g_mkdir_with_parents(config_dir, 0755);
    g_free(config_dir);

    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_double(keyfile, "Thresholds", "cpu_threshold", config.cpu_threshold);
    g_key_file_set_double(keyfile, "Thresholds", "mem_threshold", config.mem_threshold);
    g_key_file_set_integer(keyfile, "Intervals", "usb_scan_interval", config.usb_scan_interval);
    g_key_file_set_integer(keyfile, "Intervals", "process_scan_interval", config.process_scan_interval);
    g_key_file_set_integer(keyfile, "Intervals", "port_scan_interval", config.port_scan_interval);
    g_key_file_set_boolean(keyfile, "AutoScan", "usb", config.auto_scan_usb);
    g_key_file_set_boolean(keyfile, "AutoScan", "processes", config.auto_scan_processes);
    g_key_file_set_boolean(keyfile, "AutoScan", "ports", config.auto_scan_ports);
    g_key_file_set_boolean(keyfile, "Alerts", "sound", config.enable_sound_alerts);
    g_key_file_set_boolean(keyfile, "Alerts", "notifications", config.enable_notifications);
    g_key_file_set_boolean(keyfile, "Alerts", "log_to_file", config.log_to_file);
    g_key_file_set_integer(keyfile, "Ports", "scan_start", config.port_scan_start);
    g_key_file_set_integer(keyfile, "Ports", "scan_end", config.port_scan_end);
    g_key_file_set_string(keyfile, "Whitelist", "processes", config.whitelist_processes);

    GError *error = NULL;
    if (!g_key_file_save_to_file(keyfile, config_path, &error)) {
        gui_add_log_entry("CONFIG", "ERROR", error->message);
        g_error_free(error);
    } else {
        gui_add_log_entry("CONFIG", "INFO", "Configuracion guardada exitosamente");
    }

    g_key_file_free(keyfile);
    g_free(config_path);
}

static void load_config_from_file(void) {
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "matcom-guard", "config.ini", NULL);
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        g_error_free(error);
        g_key_file_free(keyfile);
        g_free(config_path);
        return; // Sin archivo previo: se mantienen los valores por defecto.
    }

    config.cpu_threshold = g_key_file_get_double(keyfile, "Thresholds", "cpu_threshold", NULL);
    config.mem_threshold = g_key_file_get_double(keyfile, "Thresholds", "mem_threshold", NULL);
    config.usb_scan_interval = g_key_file_get_integer(keyfile, "Intervals", "usb_scan_interval", NULL);
    config.process_scan_interval = g_key_file_get_integer(keyfile, "Intervals", "process_scan_interval", NULL);
    config.port_scan_interval = g_key_file_get_integer(keyfile, "Intervals", "port_scan_interval", NULL);
    config.auto_scan_usb = g_key_file_get_boolean(keyfile, "AutoScan", "usb", NULL);
    config.auto_scan_processes = g_key_file_get_boolean(keyfile, "AutoScan", "processes", NULL);
    config.auto_scan_ports = g_key_file_get_boolean(keyfile, "AutoScan", "ports", NULL);
    config.enable_sound_alerts = g_key_file_get_boolean(keyfile, "Alerts", "sound", NULL);
    config.enable_notifications = g_key_file_get_boolean(keyfile, "Alerts", "notifications", NULL);
    config.log_to_file = g_key_file_get_boolean(keyfile, "Alerts", "log_to_file", NULL);
    config.port_scan_start = g_key_file_get_integer(keyfile, "Ports", "scan_start", NULL);
    config.port_scan_end = g_key_file_get_integer(keyfile, "Ports", "scan_end", NULL);

    gchar *whitelist = g_key_file_get_string(keyfile, "Whitelist", "processes", NULL);
    if (whitelist) {
        g_free(config.whitelist_processes);
        config.whitelist_processes = whitelist;
    }

    g_key_file_free(keyfile);
    g_free(config_path);
    gui_add_log_entry("CONFIG", "INFO", "Configuracion cargada desde archivo");
}

static void on_config_apply_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;

    config.cpu_threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(cpu_threshold_spin));
    config.mem_threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(mem_threshold_spin));
    config.usb_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(usb_interval_spin));
    config.process_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(process_interval_spin));
    config.port_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_interval_spin));
    config.auto_scan_usb = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_usb_check));
    config.auto_scan_processes = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_process_check));
    config.auto_scan_ports = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_port_check));
    config.enable_sound_alerts = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sound_alerts_check));
    config.enable_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications_check));
    config.log_to_file = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(log_file_check));
    config.port_scan_start = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_start_spin));
    config.port_scan_end = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_end_spin));

    const gchar *whitelist = gtk_entry_get_text(GTK_ENTRY(whitelist_entry));
    g_free(config.whitelist_processes);
    config.whitelist_processes = g_strdup(whitelist);

    save_config_to_file();
    gui_add_log_entry("CONFIG", "INFO", "Configuracion aplicada exitosamente");
}

static void on_config_defaults_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(config_dialog), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Esta seguro de que desea restaurar los valores por defecto?");

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(cpu_threshold_spin), 70.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(mem_threshold_spin), 50.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(usb_interval_spin), 30.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(process_interval_spin), 5.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_interval_spin), 300.0);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_usb_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_process_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_port_check), FALSE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound_alerts_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check), TRUE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(log_file_check), TRUE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), 1.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), 1024.0);
        gtk_entry_set_text(GTK_ENTRY(whitelist_entry), "firefox,chrome,systemd,gnome-shell");
        gui_add_log_entry("CONFIG", "INFO", "Valores por defecto restaurados");
    }

    gtk_widget_destroy(dialog);
}

static GtkWidget *page_title(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "nw-panel-title");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

static GtkWidget *create_thresholds_page(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "nw-panel");
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    gtk_grid_attach(GTK_GRID(grid), page_title("Umbrales de Alerta"), 0, 0, 2, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Umbral de CPU (%):"), 0, 1, 1, 1);
    cpu_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cpu_threshold_spin), config.cpu_threshold);
    gtk_grid_attach(GTK_GRID(grid), cpu_threshold_spin, 1, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Umbral de Memoria (%):"), 0, 2, 1, 1);
    mem_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mem_threshold_spin), config.mem_threshold);
    gtk_grid_attach(GTK_GRID(grid), mem_threshold_spin, 1, 2, 1, 1);

    return grid;
}

static GtkWidget *create_intervals_page(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "nw-panel");
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    gtk_grid_attach(GTK_GRID(grid), page_title("Intervalos de Escaneo Automatico"), 0, 0, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Modulo"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Intervalo (seg)"), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Activar"), 2, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Dispositivos USB:"), 0, 2, 1, 1);
    usb_interval_spin = gtk_spin_button_new_with_range(5.0, 3600.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(usb_interval_spin), config.usb_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), usb_interval_spin, 1, 2, 1, 1);
    auto_usb_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_usb_check), config.auto_scan_usb);
    gtk_grid_attach(GTK_GRID(grid), auto_usb_check, 2, 2, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Procesos:"), 0, 3, 1, 1);
    process_interval_spin = gtk_spin_button_new_with_range(1.0, 300.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(process_interval_spin), config.process_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), process_interval_spin, 1, 3, 1, 1);
    auto_process_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_process_check), config.auto_scan_processes);
    gtk_grid_attach(GTK_GRID(grid), auto_process_check, 2, 3, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Puertos de Red:"), 0, 4, 1, 1);
    port_interval_spin = gtk_spin_button_new_with_range(60.0, 7200.0, 60.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_interval_spin), config.port_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), port_interval_spin, 1, 4, 1, 1);
    auto_port_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_port_check), config.auto_scan_ports);
    gtk_grid_attach(GTK_GRID(grid), auto_port_check, 2, 4, 1, 1);

    return grid;
}

static GtkWidget *create_alerts_page(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(vbox), "nw-panel");

    gtk_box_pack_start(GTK_BOX(vbox), page_title("Configuracion de Alertas"), FALSE, FALSE, 0);

    sound_alerts_check = gtk_check_button_new_with_label("Activar alertas sonoras");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound_alerts_check), config.enable_sound_alerts);
    gtk_box_pack_start(GTK_BOX(vbox), sound_alerts_check, FALSE, FALSE, 0);

    notifications_check = gtk_check_button_new_with_label("Mostrar notificaciones del sistema");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check), config.enable_notifications);
    gtk_box_pack_start(GTK_BOX(vbox), notifications_check, FALSE, FALSE, 0);

    log_file_check = gtk_check_button_new_with_label("Guardar logs en archivo");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(log_file_check), config.log_to_file);
    gtk_box_pack_start(GTK_BOX(vbox), log_file_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(vbox), page_title("Rango de Escaneo de Puertos"), FALSE, FALSE, 0);

    GtkWidget *ports_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(ports_box), gtk_label_new("Desde:"), FALSE, FALSE, 0);
    port_start_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), config.port_scan_start);
    gtk_box_pack_start(GTK_BOX(ports_box), port_start_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ports_box), gtk_label_new("Hasta:"), FALSE, FALSE, 0);
    port_end_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), config.port_scan_end);
    gtk_box_pack_start(GTK_BOX(ports_box), port_end_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), ports_box, FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *create_whitelist_page(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(vbox), "nw-panel");

    gtk_box_pack_start(GTK_BOX(vbox), page_title("Lista Blanca de Procesos"), FALSE, FALSE, 0);

    GtkWidget *desc = gtk_label_new(
        "Los procesos en esta lista no generaran alertas aunque excedan los umbrales.\n"
        "Separe los nombres con comas (ejemplo: firefox,chrome,code)");
    gtk_label_set_line_wrap(GTK_LABEL(desc), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), desc, FALSE, FALSE, 0);

    whitelist_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(whitelist_entry), config.whitelist_processes);
    gtk_box_pack_start(GTK_BOX(vbox), whitelist_entry, FALSE, FALSE, 0);

    GtkWidget *common_list = gtk_label_new(
        "Procesos comunes que puede agregar:\n"
        "firefox, chrome, chromium - Navegadores web\n"
        "code, sublime_text, atom - Editores de codigo\n"
        "spotify, vlc, mpv - Reproductores multimedia\n"
        "discord, slack, teams - Aplicaciones de comunicacion\n"
        "gnome-shell, plasmashell - Entornos de escritorio\n"
        "systemd, init - Procesos del sistema");
    gtk_label_set_xalign(GTK_LABEL(common_list), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), common_list, FALSE, FALSE, 0);

    return vbox;
}

void show_config_dialog(GtkWindow *parent) {
    ensure_default_whitelist();
    load_config_from_file();

    config_dialog = gtk_dialog_new_with_buttons("Configuracion de MatCom Guard", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancelar", GTK_RESPONSE_CANCEL,
        "_Valores por Defecto", GTK_RESPONSE_REJECT,
        "_Aplicar", GTK_RESPONSE_APPLY,
        "_Aceptar", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(config_dialog), 600, 500);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_thresholds_page(), gtk_label_new("Umbrales"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_intervals_page(), gtk_label_new("Intervalos"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_alerts_page(), gtk_label_new("Alertas"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_whitelist_page(), gtk_label_new("Lista Blanca"));

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(config_dialog));
    gtk_box_pack_start(GTK_BOX(content_area), notebook, TRUE, TRUE, 0);

    g_signal_connect(gtk_dialog_get_widget_for_response(GTK_DIALOG(config_dialog), GTK_RESPONSE_REJECT),
                     "clicked", G_CALLBACK(on_config_defaults_clicked), NULL);

    gtk_widget_show_all(config_dialog);

    gint response;
    do {
        response = gtk_dialog_run(GTK_DIALOG(config_dialog));
        if (response == GTK_RESPONSE_APPLY || response == GTK_RESPONSE_ACCEPT) {
            on_config_apply_clicked(NULL, NULL);
        }
    } while (response == GTK_RESPONSE_APPLY);

    gtk_widget_destroy(config_dialog);
    config_dialog = NULL;
}

gdouble get_cpu_threshold(void) { return config.cpu_threshold; }
gdouble get_mem_threshold(void) { return config.mem_threshold; }
gint get_usb_scan_interval(void) { return config.usb_scan_interval; }
gint get_process_scan_interval(void) { return config.process_scan_interval; }
gint get_port_scan_interval(void) { return config.port_scan_interval; }
gboolean is_auto_scan_usb_enabled(void) { return config.auto_scan_usb; }
gboolean is_auto_scan_processes_enabled(void) { return config.auto_scan_processes; }
gboolean is_auto_scan_ports_enabled(void) { return config.auto_scan_ports; }
gboolean is_sound_alerts_enabled(void) { return config.enable_sound_alerts; }
gboolean is_notifications_enabled(void) { return config.enable_notifications; }
