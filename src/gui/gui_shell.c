#include "gui_shell.h"
#include "gui_icons.h"
#include <stdio.h>

#define GUI_SHELL_PAGE_COUNT 5
#define GUI_SHELL_CSS_PATH "resources/style/night-watch.css"

static GtkWidget *rail_buttons[GUI_SHELL_PAGE_COUNT];
static GtkWidget *page_containers[GUI_SHELL_PAGE_COUNT];
static GtkWidget *content_stack = NULL;
static GtkWidget *action_bar_container = NULL;
static GtkWidget *status_badge = NULL;

static const char *page_names[GUI_SHELL_PAGE_COUNT] = {
    "page0", "page1", "page2", "page3", "page4"
};
static const GuiIconType page_icon_types[GUI_SHELL_PAGE_COUNT] = {
    GUI_ICON_DASHBOARD, GUI_ICON_USB, GUI_ICON_PROCESSES, GUI_ICON_PORTS, GUI_ICON_LOGS
};
static const char *page_labels[GUI_SHELL_PAGE_COUNT] = {
    "Dashboard", "Dispositivos USB", "Procesos", "Puertos", "Registros"
};

static void load_night_watch_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    gtk_css_provider_load_from_path(provider, GUI_SHELL_CSS_PATH, &error);
    if (error != NULL) {
        fprintf(stderr, "[gui_shell] No se pudo cargar %s: %s\n", GUI_SHELL_CSS_PATH, error->message);
        g_error_free(error);
    } else {
        GdkScreen *screen = gdk_screen_get_default();
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

static void on_rail_button_toggled(GtkToggleButton *button, gpointer user_data) {
    int index = GPOINTER_TO_INT(user_data);
    if (!gtk_toggle_button_get_active(button)) {
        return; // solo actuamos cuando un botón se prende, no cuando se apaga
    }
    for (int i = 0; i < GUI_SHELL_PAGE_COUNT; i++) {
        if (i != index) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rail_buttons[i]), FALSE);
        }
    }
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack), page_names[index]);
}

static GtkWidget *create_status_badge_bar(void) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "nw-status-bar");

    GtkWidget *title = gtk_label_new("\xF0\x9F\x9B\xA1\xEF\xB8\x8F MatCom Guard");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(bar), title, TRUE, TRUE, 0);

    // Vacío: el llamador agrega aquí sus propios botones de acción
    // (Escanear/Pausar/Configuración/Exportar) via gui_shell_get_action_bar().
    action_bar_container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(bar), action_bar_container, FALSE, FALSE, 0);

    status_badge = gtk_label_new("\xE2\x97\x8F SISTEMA SEGURO");
    gtk_style_context_add_class(gtk_widget_get_style_context(status_badge), "nw-badge");
    gtk_box_pack_end(GTK_BOX(bar), status_badge, FALSE, FALSE, 0);

    return bar;
}

static GtkWidget *create_icon_rail(void) {
    GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(rail), "nw-rail");
    gtk_widget_set_size_request(rail, 44, -1);

    for (int i = 0; i < GUI_SHELL_PAGE_COUNT; i++) {
        GtkWidget *btn = gtk_toggle_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "nw-rail-button");
        gtk_widget_set_tooltip_text(btn, page_labels[i]);

        GtkWidget *icon = gui_icon_widget_new(page_icon_types[i], GTK_TOGGLE_BUTTON(btn));
        gtk_container_add(GTK_CONTAINER(btn), icon);

        g_signal_connect(btn, "toggled", G_CALLBACK(on_rail_button_toggled), GINT_TO_POINTER(i));
        rail_buttons[i] = btn;
        gtk_box_pack_start(GTK_BOX(rail), btn, FALSE, FALSE, 0);
    }

    return rail;
}

static GtkWidget *create_placeholder_page(const char *label_text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *label = gtk_label_new(label_text);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "nw-placeholder");
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);

    return box;
}

static void create_content_stack(void) {
    content_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(content_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    for (int i = 0; i < GUI_SHELL_PAGE_COUNT; i++) {
        if (i == 0) {
            // El Dashboard lo llena el llamador (ver gui_shell_get_page_container);
            // acá solo se crea un contenedor vacío.
            page_containers[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        } else {
            char message[128];
            snprintf(message, sizeof(message), "%s \xE2\x80\x94 pr\xC3\xB3ximamente", page_labels[i]);
            page_containers[i] = create_placeholder_page(message);
        }
        gtk_stack_add_named(GTK_STACK(content_stack), page_containers[i], page_names[i]);
    }
}

GtkWidget *gui_shell_create(void) {
    load_night_watch_css();

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(root), "nw-shell");

    gtk_box_pack_start(GTK_BOX(root), create_status_badge_bar(), FALSE, FALSE, 0);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(body), create_icon_rail(), FALSE, FALSE, 0);

    create_content_stack();
    gtk_box_pack_start(GTK_BOX(body), content_stack, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rail_buttons[0]), TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack), page_names[0]);

    return root;
}

GtkWidget *gui_shell_get_page_container(int page_index) {
    if (page_index < 0 || page_index >= GUI_SHELL_PAGE_COUNT) {
        return NULL;
    }
    return page_containers[page_index];
}

GtkWidget *gui_shell_get_action_bar(void) {
    return action_bar_container;
}

void gui_shell_set_active_page(int page_index) {
    if (page_index < 0 || page_index >= GUI_SHELL_PAGE_COUNT || !rail_buttons[page_index]) {
        return;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rail_buttons[page_index]), TRUE);
}

void gui_shell_set_status_badge(const char *text, gboolean is_healthy) {
    if (!status_badge || !text) {
        return;
    }

    char badge_text[160];
    snprintf(badge_text, sizeof(badge_text), "\xE2\x97\x8F %s", text);
    gtk_label_set_text(GTK_LABEL(status_badge), badge_text);

    GtkStyleContext *ctx = gtk_widget_get_style_context(status_badge);
    if (is_healthy) {
        gtk_style_context_remove_class(ctx, "nw-badge-critical");
        gtk_style_context_add_class(ctx, "nw-badge");
    } else {
        gtk_style_context_remove_class(ctx, "nw-badge");
        gtk_style_context_add_class(ctx, "nw-badge-critical");
    }
}
