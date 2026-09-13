// src/gui/panels/gui_logs_panel.c
#include "gui_logs_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

GtkWidget *log_text_view = NULL;
GtkTextBuffer *log_buffer = NULL;
GtkTextTag *info_tag = NULL;
GtkTextTag *warning_tag = NULL;
GtkTextTag *error_tag = NULL;
GtkTextTag *alert_tag = NULL;

GtkWidget *gui_logs_panel_create(void) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *title = gtk_label_new("Registro de Eventos");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(panel), title, FALSE, FALSE, 0);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled_window, TRUE);

    log_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_text_view), GTK_WRAP_WORD);
    gtk_style_context_add_class(gtk_widget_get_style_context(log_text_view), "nw-log-view");

    log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_text_view));

    info_tag = gtk_text_buffer_create_tag(log_buffer, "info", "foreground", "#4FB0A5", NULL);
    warning_tag = gtk_text_buffer_create_tag(log_buffer, "warning", "foreground", "#D98E3F",
                                              "weight", PANGO_WEIGHT_BOLD, NULL);
    error_tag = gtk_text_buffer_create_tag(log_buffer, "error", "foreground", "#C0524A",
                                            "weight", PANGO_WEIGHT_BOLD, NULL);
    alert_tag = gtk_text_buffer_create_tag(log_buffer, "alert", "foreground", "#12141C",
                                            "background", "#C0524A", "weight", PANGO_WEIGHT_BOLD, NULL);

    gtk_container_add(GTK_CONTAINER(scrolled_window), log_text_view);
    gtk_box_pack_start(GTK_BOX(panel), scrolled_window, TRUE, TRUE, 0);

    gtk_text_buffer_set_text(log_buffer,
        "MatCom Guard - Sistema de Proteccion Iniciado\n"
        "Monitoreando dispositivos USB, procesos y puertos de red...\n\n", -1);

    return panel;
}

static char *safe_strdup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, str, len);
    return copy;
}

typedef struct {
    char *module;
    char *level;
    char *message;
} LogEntryData;

static gboolean add_log_entry_main_thread(gpointer user_data) {
    LogEntryData *data = (LogEntryData *)user_data;
    if (!log_buffer || !data) {
        if (data) {
            free(data->module);
            free(data->level);
            free(data->message);
            free(data);
        }
        return FALSE;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);

    char full_message[1024];
    snprintf(full_message, sizeof(full_message), "[%s] %s | %s: %s\n",
             timestamp, data->level, data->module, data->message);

    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(log_buffer, &end_iter);

    GtkTextTag *tag = NULL;
    if (strcmp(data->level, "INFO") == 0) tag = info_tag;
    else if (strcmp(data->level, "WARNING") == 0) tag = warning_tag;
    else if (strcmp(data->level, "ERROR") == 0) tag = error_tag;
    else if (strcmp(data->level, "ALERT") == 0) tag = alert_tag;

    if (tag) {
        gtk_text_buffer_insert_with_tags(log_buffer, &end_iter, full_message, -1, tag, NULL);
    } else {
        gtk_text_buffer_insert(log_buffer, &end_iter, full_message, -1);
    }

    gtk_text_buffer_get_end_iter(log_buffer, &end_iter);
    GtkTextMark *end_mark = gtk_text_buffer_create_mark(log_buffer, NULL, &end_iter, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_text_view), end_mark);
    gtk_text_buffer_delete_mark(log_buffer, end_mark);

    printf("%s", full_message);

    free(data->module);
    free(data->level);
    free(data->message);
    free(data);
    return FALSE;
}

void gui_add_log_entry(const char *module, const char *level, const char *message) {
    if (!log_buffer || !module || !level || !message) return;

    LogEntryData *data = malloc(sizeof(LogEntryData));
    if (!data) return;

    data->module = safe_strdup(module);
    data->level = safe_strdup(level);
    data->message = safe_strdup(message);

    if (!data->module || !data->level || !data->message) {
        free(data->module);
        free(data->level);
        free(data->message);
        free(data);
        return;
    }

    g_idle_add(add_log_entry_main_thread, data);
}
