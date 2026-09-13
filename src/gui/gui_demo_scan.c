#include "gui_demo_scan.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "port_scanner.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static GtkWidget *dial = NULL;
static GtkWidget *scan_button = NULL;
static GtkWidget *cancel_button = NULL;
static pthread_t scan_thread;
static volatile sig_atomic_t cancel_flag = 0;
static volatile sig_atomic_t scan_running = 0;

typedef struct {
    ScanResult result;
} ScanDoneMessage;

static void on_demo_scan_progress(const ProgressUpdate *update, void *ui_user_data) {
    GtkWidget *dial_widget = (GtkWidget *)ui_user_data;
    guard_dial_set_progress(dial_widget, update->current, update->total, update->phase);
}

static gboolean on_scan_done_idle(gpointer data) {
    pthread_join(scan_thread, NULL);

    ScanDoneMessage *msg = (ScanDoneMessage *)data;

    char summary[128];
    if (cancel_flag) {
        snprintf(summary, sizeof(summary), "Cancelado (%d abiertos detectados)",
                  msg->result.open_ports);
    } else {
        snprintf(summary, sizeof(summary), "Listo: %d abiertos de %d",
                  msg->result.open_ports, msg->result.total_ports);
    }
    guard_dial_set_progress(dial, msg->result.total_ports, msg->result.total_ports, summary);

    if (msg->result.ports != NULL) {
        free(msg->result.ports);
    }
    free(msg);

    gtk_widget_set_sensitive(scan_button, TRUE);
    gtk_widget_set_sensitive(cancel_button, FALSE);
    scan_running = 0;

    return G_SOURCE_REMOVE;
}

static gboolean on_scan_start_failed_idle(gpointer data) {
    (void)data;
    pthread_join(scan_thread, NULL);
    gtk_widget_set_sensitive(scan_button, TRUE);
    gtk_widget_set_sensitive(cancel_button, FALSE);
    guard_dial_set_progress(dial, 0, 0, "No se pudo iniciar el escaneo");
    scan_running = 0;
    return G_SOURCE_REMOVE;
}

static void *demo_scan_thread_main(void *arg) {
    (void)arg;

    GuiThreadBridgeTarget target = {
        .handler = on_demo_scan_progress,
        .ui_user_data = dial,
        .watch = G_OBJECT(dial)
    };

    ScanDoneMessage *msg = malloc(sizeof(ScanDoneMessage));
    if (msg == NULL) {
        fprintf(stderr, "[gui_demo_scan] No se pudo asignar memoria para el resultado del escaneo\n");
        g_idle_add(on_scan_start_failed_idle, NULL);
        return NULL;
    }

    int rc = scan_ports_range(1, 100, 4, gui_thread_bridge_post, &target, &cancel_flag, &msg->result);
    if (rc != 0) {
        msg->result.ports = NULL;
        msg->result.total_ports = 0;
        msg->result.open_ports = 0;
        msg->result.suspicious_ports = 0;
    }

    g_idle_add(on_scan_done_idle, msg);
    return NULL;
}

static void on_scan_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (scan_running) {
        return;
    }
    cancel_flag = 0;
    scan_running = 1;
    gtk_widget_set_sensitive(scan_button, FALSE);
    gtk_widget_set_sensitive(cancel_button, TRUE);
    guard_dial_set_progress(dial, 0, 100, "Iniciando escaneo de prueba...");

    if (pthread_create(&scan_thread, NULL, demo_scan_thread_main, NULL) != 0) {
        fprintf(stderr, "[gui_demo_scan] No se pudo crear el hilo de escaneo\n");
        scan_running = 0;
        gtk_widget_set_sensitive(scan_button, TRUE);
        gtk_widget_set_sensitive(cancel_button, FALSE);
        guard_dial_set_progress(dial, 0, 0, "No se pudo iniciar el escaneo");
    }
}

static void on_cancel_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    cancel_flag = 1;
}

GtkWidget *gui_demo_scan_create_widget(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(box), dial, FALSE, FALSE, 0);

    GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(button_row, GTK_ALIGN_CENTER);

    scan_button = gtk_button_new_with_label("Probar escaneo (127.0.0.1:1-100)");
    g_signal_connect(scan_button, "clicked", G_CALLBACK(on_scan_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(button_row), scan_button, FALSE, FALSE, 0);

    cancel_button = gtk_button_new_with_label("Cancelar");
    gtk_widget_set_sensitive(cancel_button, FALSE);
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(on_cancel_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(button_row), cancel_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), button_row, FALSE, FALSE, 0);

    return box;
}

void gui_demo_scan_shutdown(void) {
    if (!scan_running) {
        return;
    }
    cancel_flag = 1;
    // Antes de que este join retorne, el hilo ya llamó a g_idle_add() con
    // on_scan_done_idle (o on_scan_start_failed_idle) como su último acto
    // -- esa idle queda encolada en el GMainContext por defecto, y ESA
    // función también hace su propio pthread_join(scan_thread, ...), lo
    // cual sería un doble join (comportamiento indefinido) si alguna vez
    // llegara a despacharse. Hoy es inalcanzable: on_window_destroy llama
    // gtk_main_quit() justo después de esta función sin bombear el loop
    // principal en el medio (no hay ningún gtk_main_iteration()/
    // g_main_context_iteration() en todo el árbol de fuentes), así que esa
    // idle nunca se despacha -- queda abandonada (fuga menor al cierre del
    // proceso, sin doble join). Si alguna vez se bombea el loop entre esta
    // llamada y gtk_main_quit(), o se llama a esta función desde otro
    // lugar, hay que invalidar `scan_thread` acá para que las callbacks de
    // idle detecten que ya fue unido y no lo intenten de nuevo.
    pthread_join(scan_thread, NULL);
    scan_running = 0;
}
