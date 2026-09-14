// include/gui_config_dialog.h
#ifndef GUI_CONFIG_DIALOG_H
#define GUI_CONFIG_DIALOG_H

#include <gtk/gtk.h>

void show_config_dialog(GtkWindow *parent);

gdouble get_cpu_threshold(void);
gdouble get_mem_threshold(void);
gint get_usb_scan_interval(void);
gint get_process_scan_interval(void);
gint get_port_scan_interval(void);
gboolean is_auto_scan_usb_enabled(void);
gboolean is_auto_scan_processes_enabled(void);
gboolean is_auto_scan_ports_enabled(void);
gboolean is_sound_alerts_enabled(void);
gboolean is_notifications_enabled(void);

#endif // GUI_CONFIG_DIALOG_H
