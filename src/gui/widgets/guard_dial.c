#include "guard_dial.h"
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double guard_dial_compute_arc_fraction(int current, int total) {
    if (total <= 0) {
        return 0.0;
    }
    if (current <= 0) {
        return 0.0;
    }
    if (current >= total) {
        return 1.0;
    }
    return (double)current / (double)total;
}

#define GUARD_DIAL_PHASE_BUFFER 128
#define GUARD_DIAL_STATE_KEY "guard-dial-state"

typedef struct {
    int current;
    int total;
    char phase[GUARD_DIAL_PHASE_BUFFER];
} GuardDialState;

static void guard_dial_state_free(gpointer data) {
    free(data);
}

static gboolean guard_dial_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)user_data;
    GuardDialState *state = g_object_get_data(G_OBJECT(widget), GUARD_DIAL_STATE_KEY);

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    double cx = allocation.width / 2.0;
    double cy = allocation.height / 2.0;
    double smaller_side = allocation.width < allocation.height ? allocation.width : allocation.height;
    double radius = smaller_side / 2.0 - 12.0;
    if (radius < 1.0) {
        radius = 1.0;
    }

    // Anillo de fondo.
    cairo_set_source_rgb(cr, 0x2A / 255.0, 0x2E / 255.0, 0x3D / 255.0);
    cairo_set_line_width(cr, 10);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_stroke(cr);

    if (state != NULL) {
        double fraction = guard_dial_compute_arc_fraction(state->current, state->total);
        double start_angle = -G_PI / 2.0;
        double end_angle = start_angle + fraction * 2 * G_PI;

        if (fraction > 0.0) {
            cairo_set_source_rgb(cr, 0xD9 / 255.0, 0x8E / 255.0, 0x3F / 255.0);
            cairo_set_line_width(cr, 10);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_arc(cr, cx, cy, radius, start_angle, end_angle);
            cairo_stroke(cr);
        }
    }

    // Anillo interior punteado, puramente decorativo (aspecto de instrumento).
    cairo_set_source_rgb(cr, 0x23 / 255.0, 0x28 / 255.0, 0x38 / 255.0);
    cairo_set_line_width(cr, 1);
    double dashes[] = {2.0, 4.0};
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_arc(cr, cx, cy, radius - 16 > 1.0 ? radius - 16 : 1.0, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);

    if (state != NULL) {
        char percent_text[16];
        int percent = (state->total > 0)
            ? (int)(guard_dial_compute_arc_fraction(state->current, state->total) * 100)
            : 0;
        snprintf(percent_text, sizeof(percent_text), "%d%%", percent);

        int text_w, text_h;

        PangoLayout *percent_layout = pango_cairo_create_layout(cr);
        PangoFontDescription *percent_font = pango_font_description_from_string("Georgia Bold 16");
        pango_layout_set_font_description(percent_layout, percent_font);
        pango_layout_set_text(percent_layout, percent_text, -1);
        pango_layout_get_pixel_size(percent_layout, &text_w, &text_h);
        cairo_set_source_rgb(cr, 0xE8 / 255.0, 0xE6 / 255.0, 0xDE / 255.0);
        cairo_move_to(cr, cx - text_w / 2.0, cy - text_h - 2);
        pango_cairo_show_layout(cr, percent_layout);
        pango_font_description_free(percent_font);
        g_object_unref(percent_layout);

        PangoLayout *phase_layout = pango_cairo_create_layout(cr);
        PangoFontDescription *phase_font = pango_font_description_from_string("Monospace 8");
        pango_layout_set_font_description(phase_layout, phase_font);
        pango_layout_set_text(phase_layout, state->phase, -1);
        pango_layout_get_pixel_size(phase_layout, &text_w, &text_h);
        cairo_set_source_rgb(cr, 0x9a / 255.0, 0xa1 / 255.0, 0xad / 255.0);
        cairo_move_to(cr, cx - text_w / 2.0, cy + 4);
        pango_cairo_show_layout(cr, phase_layout);
        pango_font_description_free(phase_font);
        g_object_unref(phase_layout);
    }

    return FALSE;
}

GtkWidget *guard_dial_new(void) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 140, 140);

    GuardDialState *state = malloc(sizeof(GuardDialState));
    if (state == NULL) {
        fprintf(stderr, "[guard_dial] No se pudo asignar memoria para el estado del dial\n");
    } else {
        state->current = 0;
        state->total = 0;
        state->phase[0] = '\0';
        g_object_set_data_full(G_OBJECT(area), GUARD_DIAL_STATE_KEY, state, guard_dial_state_free);
    }

    g_signal_connect(area, "draw", G_CALLBACK(guard_dial_draw), NULL);
    return area;
}

void guard_dial_set_progress(GtkWidget *dial, int current, int total, const char *phase) {
    GuardDialState *state = g_object_get_data(G_OBJECT(dial), GUARD_DIAL_STATE_KEY);
    if (state == NULL) {
        return;
    }

    state->current = current;
    state->total = total;
    if (phase != NULL) {
        strncpy(state->phase, phase, sizeof(state->phase) - 1);
        state->phase[sizeof(state->phase) - 1] = '\0';
    } else {
        state->phase[0] = '\0';
    }

    gtk_widget_queue_draw(dial);
}
