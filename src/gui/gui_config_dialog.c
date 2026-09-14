// src/gui/gui_config_dialog.c
#define _GNU_SOURCE // strdup() en whitelist_from_string()
//
// Esta ventana ya no es dueña de ningún estado de configuración propio --
// lee y escribe directamente el Config compartido de process_monitor.h
// (mismo struct, mismo archivo matcomguard.conf que usa el backend para su
// propio bucle de alertas). Antes este diálogo mantenía su propia copia en
// un formato distinto (GKeyFile, ~/.config/matcom-guard/config.ini), lo que
// permitía que sus umbrales de CPU/RAM divergieran silenciosamente de los
// que el backend realmente usaba para disparar alertas.
#include "gui_config_dialog.h"
#include "gui_internal.h"
#include "gui.h"
#include "process_monitor.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Construye "firefox,chrome,..." a partir del array white_list del Config
// compartido, para mostrarlo en whitelist_entry. Devuelve memoria de GLib
// que el llamador debe liberar con g_free().
static gchar *whitelist_to_string(void) {
    Config *cfg = get_config();
    GString *joined = g_string_new(NULL);
    for (int i = 0; i < cfg->num_white_processes; i++) {
        if (i > 0) {
            g_string_append_c(joined, ',');
        }
        g_string_append(joined, cfg->white_list[i]);
    }
    return g_string_free(joined, FALSE);
}

// Reemplaza el white_list del Config compartido con los procesos de `text`
// (separados por comas), liberando el array anterior. Usa malloc/strdup/free
// simples -- no g_malloc/g_free -- porque process_monitor.c es dueño de este
// array y lo libera con free() plano en su propia limpieza.
static void whitelist_from_string(const gchar *text) {
    Config *cfg = get_config();

    for (int i = 0; i < cfg->num_white_processes; i++) {
        free(cfg->white_list[i]);
    }
    free(cfg->white_list);
    cfg->white_list = NULL;
    cfg->num_white_processes = 0;

    gchar *copy = g_strdup(text);
    char *token = strtok(copy, ",");
    while (token) {
        char **temp_list = realloc(cfg->white_list, (cfg->num_white_processes + 1) * sizeof(char *));
        if (!temp_list) {
            break;
        }
        cfg->white_list = temp_list;
        cfg->white_list[cfg->num_white_processes] = strdup(token);
        if (cfg->white_list[cfg->num_white_processes]) {
            cfg->num_white_processes++;
        }
        token = strtok(NULL, ",");
    }
    g_free(copy);
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

static void on_config_apply_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;

    Config *cfg = get_config();
    cfg->max_cpu_usage = gtk_spin_button_get_value(GTK_SPIN_BUTTON(cpu_threshold_spin));
    cfg->max_ram_usage = gtk_spin_button_get_value(GTK_SPIN_BUTTON(mem_threshold_spin));
    cfg->usb_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(usb_interval_spin));
    cfg->process_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(process_interval_spin));
    cfg->port_scan_interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_interval_spin));
    cfg->auto_scan_usb = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_usb_check));
    cfg->auto_scan_processes = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_process_check));
    cfg->auto_scan_ports = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_port_check));
    cfg->enable_sound_alerts = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sound_alerts_check));
    cfg->enable_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications_check));
    cfg->log_to_file = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(log_file_check));
    cfg->port_scan_start = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_start_spin));
    cfg->port_scan_end = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(port_end_spin));

    whitelist_from_string(gtk_entry_get_text(GTK_ENTRY(whitelist_entry)));

    save_config();
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

    Config *cfg = get_config();

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Umbral de CPU (%):"), 0, 1, 1, 1);
    cpu_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cpu_threshold_spin), cfg->max_cpu_usage);
    gtk_grid_attach(GTK_GRID(grid), cpu_threshold_spin, 1, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Umbral de Memoria (%):"), 0, 2, 1, 1);
    mem_threshold_spin = gtk_spin_button_new_with_range(10.0, 100.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mem_threshold_spin), cfg->max_ram_usage);
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

    Config *cfg = get_config();

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Dispositivos USB:"), 0, 2, 1, 1);
    usb_interval_spin = gtk_spin_button_new_with_range(5.0, 3600.0, 5.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(usb_interval_spin), cfg->usb_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), usb_interval_spin, 1, 2, 1, 1);
    auto_usb_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_usb_check), cfg->auto_scan_usb);
    gtk_grid_attach(GTK_GRID(grid), auto_usb_check, 2, 2, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Procesos:"), 0, 3, 1, 1);
    process_interval_spin = gtk_spin_button_new_with_range(1.0, 300.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(process_interval_spin), cfg->process_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), process_interval_spin, 1, 3, 1, 1);
    auto_process_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_process_check), cfg->auto_scan_processes);
    gtk_grid_attach(GTK_GRID(grid), auto_process_check, 2, 3, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Puertos de Red:"), 0, 4, 1, 1);
    port_interval_spin = gtk_spin_button_new_with_range(60.0, 7200.0, 60.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_interval_spin), cfg->port_scan_interval);
    gtk_grid_attach(GTK_GRID(grid), port_interval_spin, 1, 4, 1, 1);
    auto_port_check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_port_check), cfg->auto_scan_ports);
    gtk_grid_attach(GTK_GRID(grid), auto_port_check, 2, 4, 1, 1);

    return grid;
}

static GtkWidget *create_alerts_page(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(vbox), "nw-panel");

    gtk_box_pack_start(GTK_BOX(vbox), page_title("Configuracion de Alertas"), FALSE, FALSE, 0);

    Config *cfg = get_config();

    sound_alerts_check = gtk_check_button_new_with_label("Activar alertas sonoras");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound_alerts_check), cfg->enable_sound_alerts);
    gtk_box_pack_start(GTK_BOX(vbox), sound_alerts_check, FALSE, FALSE, 0);

    notifications_check = gtk_check_button_new_with_label("Mostrar notificaciones del sistema");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check), cfg->enable_notifications);
    gtk_box_pack_start(GTK_BOX(vbox), notifications_check, FALSE, FALSE, 0);

    log_file_check = gtk_check_button_new_with_label("Guardar logs en archivo");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(log_file_check), cfg->log_to_file);
    gtk_box_pack_start(GTK_BOX(vbox), log_file_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(vbox), page_title("Rango de Escaneo de Puertos"), FALSE, FALSE, 0);

    GtkWidget *ports_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(ports_box), gtk_label_new("Desde:"), FALSE, FALSE, 0);
    port_start_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_start_spin), cfg->port_scan_start);
    gtk_box_pack_start(GTK_BOX(ports_box), port_start_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ports_box), gtk_label_new("Hasta:"), FALSE, FALSE, 0);
    port_end_spin = gtk_spin_button_new_with_range(1.0, 65535.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_end_spin), cfg->port_scan_end);
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
    gchar *whitelist_text = whitelist_to_string();
    gtk_entry_set_text(GTK_ENTRY(whitelist_entry), whitelist_text);
    g_free(whitelist_text);
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
    // El Config compartido ya esta cargado desde matcomguard.conf (una vez,
    // al arrancar la app via gui_process_panel_create() -> load_config()) --
    // este dialogo solo lo lee/escribe, no necesita recargarlo del disco.

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

gdouble get_cpu_threshold(void) { return get_config()->max_cpu_usage; }
gdouble get_mem_threshold(void) { return get_config()->max_ram_usage; }
