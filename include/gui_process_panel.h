// include/gui_process_panel.h
#ifndef GUI_PROCESS_PANEL_H
#define GUI_PROCESS_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Procesos: lista de procesos, dial de monitoreo, umbrales, terminar. */
GtkWidget *gui_process_panel_create(void);

/** Detiene el monitoreo de procesos si esta activo y libera sus recursos. */
void gui_process_panel_shutdown(void);

/** Callback compatible con ScanProcessesCallback (gui.h) para el menu "Escanear". */
void gui_compatible_scan_processes(void);

#endif // GUI_PROCESS_PANEL_H
