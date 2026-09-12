#ifndef GUI_DEMO_SCAN_H
#define GUI_DEMO_SCAN_H

#include <gtk/gtk.h>

/**
 * Crea el widget de demo (dial + botones "Probar escaneo"/"Cancelar") para
 * embeber en la página Dashboard del shell. Solo puede existir una
 * instancia por proceso (el estado del escaneo demo es global) -- llamarla
 * más de una vez sobrescribe los widgets de la instancia anterior.
 */
GtkWidget *gui_demo_scan_create_widget(void);

/**
 * Si hay un escaneo demo en curso, pide su cancelación y une (pthread_join)
 * el hilo antes de devolver el control. Debe llamarse antes de liberar
 * cualquier recurso compartido durante el apagado de la aplicación (p.ej.
 * antes de cleanup_complete_backend_system() en gui_main.c). Si no hay
 * ningún escaneo en curso, no hace nada.
 */
void gui_demo_scan_shutdown(void);

#endif // GUI_DEMO_SCAN_H
