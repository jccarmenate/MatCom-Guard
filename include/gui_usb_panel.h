// include/gui_usb_panel.h
#ifndef GUI_USB_PANEL_H
#define GUI_USB_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Dispositivos USB: lista de dispositivos, dial de escaneo, botones. */
GtkWidget *gui_usb_panel_create(void);

/** Detiene el monitoreo automático y cualquier escaneo en curso; hace join de los hilos. */
void gui_usb_panel_shutdown(void);

/** Detiene el monitoreo automático de USB sin liberar el cache de snapshots ni
 *  el pool de hashes (a diferencia de gui_usb_panel_shutdown). Usado por el
 *  boton Pausar/Reanudar de la barra superior. */
void gui_usb_panel_pause_auto_monitor(void);

/** Reinicia el monitoreo automático de USB detenido por
 *  gui_usb_panel_pause_auto_monitor(). No-op si ya esta activo. */
void gui_usb_panel_resume_auto_monitor(void);

/** Callback compatible con ScanUSBCallback (gui.h) para el menu "Escanear" del header bar. */
void gui_compatible_scan_usb(void);

int is_usb_monitoring_active(void);
int is_gui_usb_scan_in_progress(void);
int get_usb_statistics_for_gui(int *total_devices, int *suspicious_devices, int *total_files, int *files_with_changes);

#endif // GUI_USB_PANEL_H
