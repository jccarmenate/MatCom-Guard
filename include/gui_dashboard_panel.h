// include/gui_dashboard_panel.h
#ifndef GUI_DASHBOARD_PANEL_H
#define GUI_DASHBOARD_PANEL_H

#include <gtk/gtk.h>

/** Crea el panel del Dashboard: tarjetas de estadísticas + estado general. */
GtkWidget *gui_dashboard_panel_create(void);

/** Actualiza la tarjeta "USB Sospechosos" con el conteo agregado del coordinador. */
void gui_update_usb_suspicious_count(int suspicious_count);

#endif // GUI_DASHBOARD_PANEL_H
