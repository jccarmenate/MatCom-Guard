// include/gui_usb_panel.h
#ifndef GUI_USB_PANEL_H
#define GUI_USB_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel de Dispositivos USB: lista de dispositivos, dial de escaneo, botones. */
GtkWidget *gui_usb_panel_create(void);

/** Detiene el monitoreo automático y cualquier escaneo en curso; hace join de los hilos. */
void gui_usb_panel_shutdown(void);

/** Callback compatible con ScanUSBCallback (gui.h) para el menu "Escanear" del header bar. */
void gui_compatible_scan_usb(void);

#endif // GUI_USB_PANEL_H
