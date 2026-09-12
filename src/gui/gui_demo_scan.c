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
    ScanDoneMessage *msg = (ScanDoneMessage *)data;

    char summary[128];
    snprintf(summary, sizeof(summary), "Listo: %d abiertos de %d",
              msg->result.open_ports, msg->result.total_ports);
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
        scan_running = 0;
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
    pthread_create(&scan_thread, NULL, demo_scan_thread_main, NULL);
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
    pthread_join(scan_thread, NULL);
    scan_running = 0;
}
