#include "gui_internal.h"
#include "gui.h"
#include "gui_shell.h"
#include <gtk/gtk.h>
#include <stdio.h>

void gui_set_scanning_status(gboolean scanning) {
    if (scanning) {
        gui_add_log_entry("SCANNER", "INFO", "Iniciando escaneo completo del sistema");
    } else {
        gui_add_log_entry("SCANNER", "INFO", "Escaneo completo finalizado");
    }
}

void gui_update_system_status(const char *status, gboolean is_healthy) {
    gui_shell_set_status_badge(status, is_healthy);

    if (!is_healthy) {
        char log_message[512];
        snprintf(log_message, sizeof(log_message), "Estado del sistema cambió: %s", status);
        gui_add_log_entry("SISTEMA", "WARNING", log_message);
    }
}
