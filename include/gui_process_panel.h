// include/gui_process_panel.h
#ifndef GUI_PROCESS_PANEL_H
#define GUI_PROCESS_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Procesos: lista de procesos, dial de monitoreo, umbrales, terminar. */
GtkWidget *gui_process_panel_create(void);

/** Detiene el monitoreo de procesos si esta activo y libera sus recursos. */
void gui_process_panel_shutdown(void);

/** Detiene el monitoreo de procesos sin liberar recursos del panel (a diferencia
 *  de gui_process_panel_shutdown). Usado por el boton Pausar/Reanudar. */
void gui_process_panel_pause_monitoring(void);

/** Reinicia el monitoreo de procesos detenido por gui_process_panel_pause_monitoring().
 *  No-op si ya esta activo. */
void gui_process_panel_resume_monitoring(void);

/** Callback compatible con ScanProcessesCallback (gui.h) para el menu "Escanear". */
void gui_compatible_scan_processes(void);

int is_process_monitoring_active(void);
int get_process_statistics_for_gui(int *total_processes, int *high_cpu_count, int *high_memory_count, int *suspicious_count);
int sync_gui_with_backend_processes(void);

#endif // GUI_PROCESS_PANEL_H
