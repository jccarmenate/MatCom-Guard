#ifndef GUI_SHELL_H
#define GUI_SHELL_H

#include <gtk/gtk.h>

/**
 * Construye el shell nuevo: barra de estado/insignia arriba, riel de
 * iconos angosto a la izquierda, y un GtkStack de 5 páginas a la derecha
 * (4 son placeholders "próximamente"; la página 0, Dashboard, se entrega
 * vacía para que el llamador la llene). Devuelve un contenedor listo para
 * empacar en cualquier GtkBox/ventana existente -- no crea su propia
 * ventana. Carga y aplica el stylesheet Night Watch la primera vez que se
 * llama.
 *
 * Solo puede existir un shell por proceso (el estado interno es estático)
 * -- llamarla más de una vez sobrescribe el shell anterior y agrega un
 * segundo GtkCssProvider a la pantalla.
 */
GtkWidget *gui_shell_create(void);

/**
 * Devuelve el contenedor de la página `page_index` (0=Dashboard,
 * 1=USB, 2=Procesos, 3=Puertos, 4=Registros) para que el llamador le
 * agregue contenido real. NULL si `page_index` está fuera de [0, 4] o si
 * gui_shell_create() todavía no fue llamado.
 */
GtkWidget *gui_shell_get_page_container(int page_index);

#endif // GUI_SHELL_H
