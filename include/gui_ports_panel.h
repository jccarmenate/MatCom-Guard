// include/gui_ports_panel.h
#ifndef GUI_PORTS_PANEL_H
#define GUI_PORTS_PANEL_H

#include <gtk/gtk.h>
#include <time.h>

/** Crea el panel de Puertos: rango configurable, presets, dial, resultados. */
GtkWidget *gui_ports_panel_create(void);

/** Cancela cualquier escaneo en curso y hace join del hilo. */
void gui_ports_panel_shutdown(void);

/** Callback compatible con ScanPortsCallback (gui.h) para el menu "Escanear". */
void gui_compatible_scan_ports(void);

int is_port_scan_active(void);
int get_port_statistics_for_gui(int *total_open, int *total_suspicious, time_t *last_scan_time);

#endif // GUI_PORTS_PANEL_H
