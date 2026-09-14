// include/gui_config_dialog.h
#ifndef GUI_CONFIG_DIALOG_H
#define GUI_CONFIG_DIALOG_H

#include <gtk/gtk.h>

void show_config_dialog(GtkWindow *parent);

gdouble get_cpu_threshold(void);
gdouble get_mem_threshold(void);

#endif // GUI_CONFIG_DIALOG_H
