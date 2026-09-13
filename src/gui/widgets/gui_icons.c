#include <stdlib.h>
#include "gui_icons.h"

#define GUI_ICON_AMBER_R 0.851
#define GUI_ICON_AMBER_G 0.557
#define GUI_ICON_AMBER_B 0.247
#define GUI_ICON_MUTED_R 0.490
#define GUI_ICON_MUTED_G 0.518
#define GUI_ICON_MUTED_B 0.588

static void draw_dashboard(cairo_t *cr, double size) {
    // Gráfico de barras: tres barras de distinta altura.
    double bar_w = size * 0.18;
    double gap = size * 0.12;
    double base_y = size * 0.82;
    double heights[3] = { size * 0.35, size * 0.6, size * 0.45 };
    double x = size * 0.15;
    for (int i = 0; i < 3; i++) {
        cairo_rectangle(cr, x, base_y - heights[i], bar_w, heights[i]);
        cairo_fill(cr);
        x += bar_w + gap;
    }
}

static void draw_usb(cairo_t *cr, double size) {
    // Forma de memoria USB: cuerpo rectangular + conector angosto arriba.
    double body_w = size * 0.5;
    double body_h = size * 0.45;
    double body_x = (size - body_w) / 2.0;
    double body_y = size * 0.4;
    cairo_rectangle(cr, body_x, body_y, body_w, body_h);
    cairo_fill(cr);

    double connector_w = size * 0.2;
    double connector_h = size * 0.25;
    double connector_x = (size - connector_w) / 2.0;
    double connector_y = body_y - connector_h;
    cairo_rectangle(cr, connector_x, connector_y, connector_w, connector_h);
    cairo_fill(cr);
}

static void draw_processes(cairo_t *cr, double size) {
    // Rayo (lightning bolt) con un path simple de 6 puntos.
    cairo_move_to(cr, size * 0.55, size * 0.1);
    cairo_line_to(cr, size * 0.25, size * 0.58);
    cairo_line_to(cr, size * 0.45, size * 0.58);
    cairo_line_to(cr, size * 0.4, size * 0.9);
    cairo_line_to(cr, size * 0.75, size * 0.4);
    cairo_line_to(cr, size * 0.52, size * 0.4);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_ports(cairo_t *cr, double size) {
    // Enchufe: cuerpo redondeado + dos clavijas.
    double body_r = size * 0.28;
    cairo_arc(cr, size * 0.5, size * 0.55, body_r, 0, 2 * G_PI);
    cairo_fill(cr);

    double prong_w = size * 0.08;
    double prong_h = size * 0.22;
    cairo_rectangle(cr, size * 0.35, size * 0.1, prong_w, prong_h);
    cairo_fill(cr);
    cairo_rectangle(cr, size * 0.57, size * 0.1, prong_w, prong_h);
    cairo_fill(cr);
}

static void draw_logs(cairo_t *cr, double size) {
    // Documento con líneas de texto.
    double line_h = size * 0.1;
    double x = size * 0.2;
    double w = size * 0.6;
    double y = size * 0.22;
    for (int i = 0; i < 4; i++) {
        double this_w = (i == 3) ? w * 0.6 : w;
        cairo_rectangle(cr, x, y, this_w, line_h * 0.5);
        cairo_fill(cr);
        y += line_h;
    }
}

void gui_icon_draw(cairo_t *cr, GuiIconType type, double size, double r, double g, double b) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, r, g, b);

    switch (type) {
        case GUI_ICON_DASHBOARD: draw_dashboard(cr, size); break;
        case GUI_ICON_USB: draw_usb(cr, size); break;
        case GUI_ICON_PROCESSES: draw_processes(cr, size); break;
        case GUI_ICON_PORTS: draw_ports(cr, size); break;
        case GUI_ICON_LOGS: draw_logs(cr, size); break;
    }

    cairo_restore(cr);
}

typedef struct {
    GuiIconType type;
    GtkToggleButton *state_source;
} GuiIconWidgetData;

static gboolean on_icon_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    GuiIconWidgetData *data = (GuiIconWidgetData *)user_data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    double size = (width < height) ? width : height;

    gboolean active = gtk_toggle_button_get_active(data->state_source);
    double r = active ? GUI_ICON_AMBER_R : GUI_ICON_MUTED_R;
    double g = active ? GUI_ICON_AMBER_G : GUI_ICON_MUTED_G;
    double b = active ? GUI_ICON_AMBER_B : GUI_ICON_MUTED_B;

    gui_icon_draw(cr, data->type, size, r, g, b);
    return FALSE;
}

static void on_state_source_toggled(GtkToggleButton *button, gpointer user_data) {
    (void)button;
    gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static void on_icon_widget_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    free(user_data);
}

GtkWidget *gui_icon_widget_new(GuiIconType type, GtkToggleButton *state_source) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 20, 20);

    GuiIconWidgetData *data = malloc(sizeof(GuiIconWidgetData));
    if (data) {
        data->type = type;
        data->state_source = state_source;
        g_signal_connect(area, "draw", G_CALLBACK(on_icon_draw), data);
        g_signal_connect(area, "destroy", G_CALLBACK(on_icon_widget_destroy), data);
        g_signal_connect(state_source, "toggled", G_CALLBACK(on_state_source_toggled), area);
    }

    return area;
}
