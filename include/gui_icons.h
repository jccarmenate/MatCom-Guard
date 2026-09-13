// include/gui_icons.h
#ifndef GUI_ICONS_H
#define GUI_ICONS_H

#include <gtk/gtk.h>
#include <cairo.h>

typedef enum {
    GUI_ICON_DASHBOARD,
    GUI_ICON_USB,
    GUI_ICON_PROCESSES,
    GUI_ICON_PORTS,
    GUI_ICON_LOGS
} GuiIconType;

/**
 * Dibuja el icono `type` centrado en un cuadrado de `size` x `size` puntos,
 * con el origen de `cr` ya trasladado a la esquina superior izquierda de
 * ese cuadrado (mismo estilo que guard_dial_draw: primitivas Cairo básicas,
 * sin cargar assets externos). Color uniforme (r,g,b) en [0,1].
 */
void gui_icon_draw(cairo_t *cr, GuiIconType type, double size, double r, double g, double b);

/**
 * Crea un GtkDrawingArea que dibuja `type`, conectado a la señal "toggled"
 * de `state_source` para repintarse en ámbar (#D98E3F) cuando está activo o
 * gris apagado (#7d8496) cuando no -- state_source es el botón del riel que
 * contendrá este widget.
 */
GtkWidget *gui_icon_widget_new(GuiIconType type, GtkToggleButton *state_source);

#endif // GUI_ICONS_H
