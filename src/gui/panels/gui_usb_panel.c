// src/gui/panels/gui_usb_panel.c
#include "gui_usb_panel.h"
#include "gui_internal.h"
#include "gui.h"
#include "gui_backend_adapters.h"
#include "gui_periodic_worker.h"
#include "guard_dial.h"
#include "gui_thread_bridge.h"
#include "device_monitor.h"
#include "threadpool.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define USB_HASH_POOL_THREADS 4
#define USB_AUTO_MONITOR_INTERVAL_SECONDS 30

typedef enum { USB_SCAN_MODE_REFRESH, USB_SCAN_MODE_DEEP } UsbScanMode;

typedef struct {
    GtkWidget *dot;
    GtkWidget *primary;
    GtkWidget *secondary;
    GtkWidget *badge;
} UsbRowRefs;

static GtkWidget *usb_list_box = NULL;
static GtkWidget *usb_dial = NULL;
static GtkWidget *usb_detail_label = NULL;
static GtkWidget *usb_refresh_button = NULL;
static GtkWidget *usb_deep_scan_button = NULL;
static GtkWidget *usb_cancel_button = NULL;

static ThreadPool *usb_hash_pool = NULL;
static GuiPeriodicWorker *usb_auto_monitor_worker = NULL;

static pthread_t usb_scan_thread;
static volatile sig_atomic_t usb_cancel_flag = 0;
static volatile sig_atomic_t usb_scan_running = 0;
static UsbScanMode usb_current_mode = USB_SCAN_MODE_REFRESH;

// Protege usb_last_scan_files_changed/usb_last_scan_suspicious_devices:
// se escriben desde el hilo de escaneo manual (reset en start_usb_scan(),
// que corre en el hilo principal, e incrementos en usb_scan_thread_main())
// y se leen desde get_usb_statistics_for_gui(), que el coordinador del
// sistema invoca desde su propio hilo de fondo -- sin este mutex serian
// una carrera de datos entre tres hilos distintos.
static pthread_mutex_t usb_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static int usb_last_scan_files_changed = 0;
static int usb_last_scan_suspicious_devices = 0;

// ============================================================================
// FILA DE LISTA (patron compartido: punto + linea sans + linea mono + badge)
// ============================================================================

static void apply_status_classes(GtkWidget *dot, GtkWidget *badge, gboolean is_suspicious, const char *status) {
    GtkStyleContext *dot_ctx = gtk_widget_get_style_context(dot);
    GtkStyleContext *badge_ctx = gtk_widget_get_style_context(badge);

    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-normal");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-warning");
    gtk_style_context_remove_class(dot_ctx, "nw-row-dot-critical");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-normal");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-warning");
    gtk_style_context_remove_class(badge_ctx, "nw-row-badge-critical");
    gtk_style_context_add_class(badge_ctx, "nw-row-badge");

    const char *level;
    if (is_suspicious) {
        level = "critical";
    } else if (strcmp(status, "LIMPIO") == 0 || strcmp(status, "ACTUALIZADO") == 0) {
        level = "normal";
    } else {
        level = "warning";
    }

    char dot_class[32], badge_class[32];
    snprintf(dot_class, sizeof(dot_class), "nw-row-dot-%s", level);
    snprintf(badge_class, sizeof(badge_class), "nw-row-badge-%s", level);
    gtk_style_context_add_class(dot_ctx, dot_class);
    gtk_style_context_add_class(badge_ctx, badge_class);
}

static GtkWidget *find_row_by_device(const char *device_name) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(usb_list_box));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL; l = l->next) {
        const char *name = g_object_get_data(G_OBJECT(l->data), "device-name");
        if (name && strcmp(name, device_name) == 0) {
            found = GTK_WIDGET(l->data);
            break;
        }
    }
    g_list_free(children);
    return found;
}

static void on_usb_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    (void)user_data;
    if (!row) return;
    const char *name = g_object_get_data(G_OBJECT(row), "device-name");
    UsbRowRefs *refs = g_object_get_data(G_OBJECT(row), "row-refs");
    if (!name || !refs) return;

    char detail[512];
    snprintf(detail, sizeof(detail), "<b>Dispositivo:</b> %s\n<b>%s</b>\n<b>Estado:</b> %s",
             name, gtk_label_get_text(GTK_LABEL(refs->secondary)), gtk_label_get_text(GTK_LABEL(refs->badge)));
    gtk_label_set_markup(GTK_LABEL(usb_detail_label), detail);
}

// gui_update_usb_device() (gui.h) solo debe llamarse desde el hilo principal
// de GTK -- el hilo de escaneo pasa siempre por post_usb_device_update().
void gui_update_usb_device(GUIUSBDevice *device) {
    if (!usb_list_box || !device) return;

    GtkWidget *row = find_row_by_device(device->device_name);
    UsbRowRefs *refs;

    if (!row) {
        row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "nw-list-row");

        GtkWidget *dot = gtk_label_new("\xE2\x97\x8F");
        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *primary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(primary), "nw-row-primary");
        gtk_widget_set_halign(primary, GTK_ALIGN_START);
        GtkWidget *secondary = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(secondary), "nw-row-secondary");
        gtk_widget_set_halign(secondary, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(text_box), primary, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), secondary, FALSE, FALSE, 0);
        GtkWidget *badge = gtk_label_new("");

        gtk_box_pack_start(GTK_BOX(hbox), dot, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), text_box, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), badge, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);

        refs = malloc(sizeof(UsbRowRefs));
        refs->dot = dot;
        refs->primary = primary;
        refs->secondary = secondary;
        refs->badge = badge;
        g_object_set_data_full(G_OBJECT(row), "row-refs", refs, free);
        g_object_set_data_full(G_OBJECT(row), "device-name", g_strdup(device->device_name), g_free);

        gtk_list_box_insert(GTK_LIST_BOX(usb_list_box), row, -1);
        gtk_widget_show_all(row);
    } else {
        refs = g_object_get_data(G_OBJECT(row), "row-refs");
    }

    gtk_label_set_text(GTK_LABEL(refs->primary), device->device_name);

    char secondary_text[320];
    snprintf(secondary_text, sizeof(secondary_text), "%s . %d/%d archivos cambiados",
             device->mount_point, device->files_changed, device->total_files);
    gtk_label_set_text(GTK_LABEL(refs->secondary), secondary_text);
    gtk_label_set_text(GTK_LABEL(refs->badge), device->status);
    apply_status_classes(refs->dot, refs->badge, device->is_suspicious, device->status);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Dispositivo %s: %s - %d archivos modificados",
             device->device_name, device->status, device->files_changed);
    gui_add_log_entry("USB_MONITOR", device->is_suspicious ? "WARNING" : "INFO", log_msg);
}

typedef struct { GUIUSBDevice device; } UsbDeviceUpdateMsg;

static gboolean usb_device_update_idle(gpointer data) {
    UsbDeviceUpdateMsg *msg = (UsbDeviceUpdateMsg *)data;
    gui_update_usb_device(&msg->device);
    free(msg);
    return G_SOURCE_REMOVE;
}

static void post_usb_device_update(const GUIUSBDevice *device) {
    UsbDeviceUpdateMsg *msg = malloc(sizeof(UsbDeviceUpdateMsg));
    if (!msg) return;
    msg->device = *device;
    g_idle_add(usb_device_update_idle, msg);
}

// ============================================================================
// PROGRESO DE ESCANEO (dial compartido, patron gui_demo_scan)
// ============================================================================

static void on_usb_scan_progress(const ProgressUpdate *update, void *ui_user_data) {
    guard_dial_set_progress((GtkWidget *)ui_user_data, update->current, update->total, update->phase);
}

typedef struct {
    const char *device_name;
} UsbDeviceProgressContext;

// Corre en el hilo que esta escaneando (el hilo de escaneo, o un worker del
// hash_pool) -- construye el texto combinado y lo entrega de forma
// sincronica a gui_thread_bridge_post(), que copia el string internamente
// antes de retornar, asi que `ctx` (en la pila del llamador) no necesita
// sobrevivir mas alla de esta llamada.
static void usb_device_progress_trampoline(const ProgressUpdate *update, void *user_data) {
    UsbDeviceProgressContext *ctx = (UsbDeviceProgressContext *)user_data;
    char combined_phase[128];
    snprintf(combined_phase, sizeof(combined_phase), "%s: %s", ctx->device_name, update->phase ? update->phase : "");

    ProgressUpdate combined = { .phase = combined_phase, .current = update->current, .total = update->total };
    GuiThreadBridgeTarget target = { .handler = on_usb_scan_progress, .ui_user_data = usb_dial, .watch = G_OBJECT(usb_dial) };
    gui_thread_bridge_post(&combined, &target);
}

// ============================================================================
// HILO DE ESCANEO MANUAL (Actualizar / Escaneo Profundo)
// ============================================================================

typedef struct {
    int devices_processed;
    int was_cancelled;
} UsbScanDoneMessage;

static gboolean on_usb_scan_done_idle(gpointer data) {
    pthread_join(usb_scan_thread, NULL);

    UsbScanDoneMessage *msg = (UsbScanDoneMessage *)data;
    char summary[128];
    if (msg->was_cancelled) {
        snprintf(summary, sizeof(summary), "Cancelado (%d dispositivos procesados)", msg->devices_processed);
    } else {
        snprintf(summary, sizeof(summary), "Listo: %d dispositivos procesados", msg->devices_processed);
    }
    guard_dial_set_progress(usb_dial, msg->devices_processed, msg->devices_processed, summary);
    free(msg);

    gtk_widget_set_sensitive(usb_refresh_button, TRUE);
    gtk_widget_set_sensitive(usb_deep_scan_button, TRUE);
    gtk_widget_set_sensitive(usb_cancel_button, FALSE);
    usb_scan_running = 0;

    return G_SOURCE_REMOVE;
}

static void *usb_scan_thread_main(void *arg) {
    (void)arg;

    DeviceList *devices = monitor_connected_devices();
    int device_count = devices ? devices->count : 0;
    int devices_processed = 0;

    for (int i = 0; i < device_count && !usb_cancel_flag; i++) {
        const char *device_name = devices->devices[i];
        UsbDeviceProgressContext ctx = { .device_name = device_name };
        DeviceSnapshot *previous = get_cached_usb_snapshot(device_name);
        int was_cancelled = 0;

        DeviceSnapshot *snapshot = create_device_snapshot_ex(
            device_name, previous, usb_hash_pool,
            usb_device_progress_trampoline, &ctx,
            &usb_cancel_flag, &was_cancelled);

        if (!snapshot || was_cancelled) {
            if (snapshot) free_device_snapshot(snapshot);
            break;
        }

        GUIUSBDevice gui_device;

        if (usb_current_mode == USB_SCAN_MODE_REFRESH) {
            store_usb_snapshot(device_name, snapshot); // el cache toma posesion
            adapt_device_snapshot_to_gui(snapshot, NULL, &gui_device);
            utf8_safe_truncate("ACTUALIZADO", gui_device.status, sizeof(gui_device.status));
            gui_device.files_changed = 0;
            gui_device.is_suspicious = FALSE;
            post_usb_device_update(&gui_device);
        } else if (previous) {
            adapt_device_snapshot_to_gui(snapshot, previous, &gui_device);
            post_usb_device_update(&gui_device);

            pthread_mutex_lock(&usb_stats_mutex);
            if (gui_device.is_suspicious) usb_last_scan_suspicious_devices++;
            usb_last_scan_files_changed += gui_device.files_changed;
            pthread_mutex_unlock(&usb_stats_mutex);

            free_device_snapshot(snapshot); // el escaneo profundo nunca sobrescribe la referencia
        } else {
            store_usb_snapshot(device_name, snapshot); // primera vez: se vuelve la linea base
            adapt_device_snapshot_to_gui(snapshot, NULL, &gui_device);
            utf8_safe_truncate("NUEVO", gui_device.status, sizeof(gui_device.status));
            gui_device.files_changed = 0;
            gui_device.is_suspicious = FALSE;
            post_usb_device_update(&gui_device);
        }

        devices_processed++;
    }

    if (devices) free_device_list(devices);

    UsbScanDoneMessage *msg = malloc(sizeof(UsbScanDoneMessage));
    if (msg) {
        msg->devices_processed = devices_processed;
        msg->was_cancelled = usb_cancel_flag;
        g_idle_add(on_usb_scan_done_idle, msg);
    }

    return NULL;
}

static void start_usb_scan(UsbScanMode mode) {
    if (usb_scan_running) {
        gui_add_log_entry("USB_SCANNER", "WARNING", "Escaneo USB ya en progreso");
        return;
    }

    usb_current_mode = mode;
    usb_cancel_flag = 0;
    usb_scan_running = 1;

    // Un escaneo profundo nuevo reemplaza por completo las estadisticas del
    // escaneo profundo anterior (ver el comentario en
    // get_usb_statistics_for_gui) -- sin este reinicio, los contadores
    // acumularian indefinidamente a traves de multiples escaneos en vez de
    // reflejar solo el mas reciente.
    if (mode == USB_SCAN_MODE_DEEP) {
        pthread_mutex_lock(&usb_stats_mutex);
        usb_last_scan_suspicious_devices = 0;
        usb_last_scan_files_changed = 0;
        pthread_mutex_unlock(&usb_stats_mutex);
    }

    gtk_widget_set_sensitive(usb_refresh_button, FALSE);
    gtk_widget_set_sensitive(usb_deep_scan_button, FALSE);
    gtk_widget_set_sensitive(usb_cancel_button, TRUE);
    guard_dial_set_progress(usb_dial, 0, 0, mode == USB_SCAN_MODE_REFRESH ? "Actualizando snapshots..." : "Escaneando en profundidad...");

    gui_add_log_entry("USB_SCANNER", "INFO",
        mode == USB_SCAN_MODE_REFRESH ? "Actualizando snapshots de dispositivos USB"
                                       : "Iniciando escaneo profundo de dispositivos USB");

    if (pthread_create(&usb_scan_thread, NULL, usb_scan_thread_main, NULL) != 0) {
        gui_add_log_entry("USB_SCANNER", "ERROR", "No se pudo crear el hilo de escaneo USB");
        usb_scan_running = 0;
        gtk_widget_set_sensitive(usb_refresh_button, TRUE);
        gtk_widget_set_sensitive(usb_deep_scan_button, TRUE);
        gtk_widget_set_sensitive(usb_cancel_button, FALSE);
        guard_dial_set_progress(usb_dial, 0, 0, "No se pudo iniciar el escaneo");
    }
}

static void on_refresh_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    start_usb_scan(USB_SCAN_MODE_REFRESH);
}

static void on_deep_scan_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    start_usb_scan(USB_SCAN_MODE_DEEP);
}

static void on_cancel_button_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    usb_cancel_flag = 1;
}

// ============================================================================
// MONITOREO AUTOMATICO (gui_periodic_worker)
// ============================================================================

static void usb_handle_new_device(const char *device_name, volatile sig_atomic_t *cancel) {
    GUIUSBDevice initial = {0};
    utf8_safe_truncate(device_name, initial.device_name, sizeof(initial.device_name));
    // device_name viene del backend (nombre real de directorio bajo /media)
    // -- se arma la ruta completa en un buffer de scratch y se trunca de
    // forma UTF-8-segura al copiarla al campo final, en vez de dejar que
    // snprintf corte "%s" a la mitad de una secuencia multibyte.
    char full_mount_path[512];
    snprintf(full_mount_path, sizeof(full_mount_path), "/media/%s", device_name);
    utf8_safe_truncate(full_mount_path, initial.mount_point, sizeof(initial.mount_point));
    utf8_safe_truncate("DETECTADO", initial.status, sizeof(initial.status));
    initial.last_scan = time(NULL);
    post_usb_device_update(&initial);

    int was_cancelled = 0;
    DeviceSnapshot *snapshot = create_device_snapshot_ex(device_name, NULL, usb_hash_pool, NULL, NULL, cancel, &was_cancelled);
    if (!snapshot || was_cancelled) {
        if (snapshot) free_device_snapshot(snapshot);
        return;
    }

    store_usb_snapshot(device_name, snapshot);
    GUIUSBDevice gui_device;
    adapt_device_snapshot_to_gui(snapshot, NULL, &gui_device);
    utf8_safe_truncate("NUEVO", gui_device.status, sizeof(gui_device.status));
    gui_device.files_changed = 0;
    gui_device.is_suspicious = FALSE;
    post_usb_device_update(&gui_device);
}

static void usb_auto_monitor_work(volatile sig_atomic_t *cancel, void *user_data) {
    (void)user_data;
    // Estatico: solo el hilo del periodic worker toca `previous`, un unico
    // consumidor siempre conocido -- no necesita mutex propio.
    static DeviceList *previous = NULL;

    DeviceList *current = monitor_connected_devices();

    if (current && previous) {
        for (int i = 0; i < current->count && !*cancel; i++) {
            int found = 0;
            for (int j = 0; j < previous->count; j++) {
                if (strcmp(current->devices[i], previous->devices[j]) == 0) { found = 1; break; }
            }
            if (!found) usb_handle_new_device(current->devices[i], cancel);
        }
        for (int i = 0; i < previous->count; i++) {
            int found = 0;
            for (int j = 0; j < current->count; j++) {
                if (strcmp(previous->devices[i], current->devices[j]) == 0) { found = 1; break; }
            }
            if (!found) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Dispositivo USB desconectado: %s", previous->devices[i]);
                gui_add_log_entry("USB_MONITOR", "INFO", msg);
            }
        }
    } else if (current && !previous) {
        for (int i = 0; i < current->count && !*cancel; i++) {
            usb_handle_new_device(current->devices[i], cancel);
        }
    }

    if (previous) free_device_list(previous);
    previous = current;
}

// ============================================================================
// CICLO DE VIDA DEL PANEL
// ============================================================================

GtkWidget *gui_usb_panel_create(void) {
    init_usb_snapshot_cache();
    usb_hash_pool = threadpool_create(USB_HASH_POOL_THREADS);

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(panel), "nw-panel");

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new("Dispositivos USB");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "nw-panel-title");
    gtk_box_pack_start(GTK_BOX(toolbar), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new(""), TRUE, TRUE, 0);

    usb_cancel_button = gtk_button_new_with_label("Cancelar");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_cancel_button), "nw-button-secondary");
    gtk_widget_set_sensitive(usb_cancel_button, FALSE);
    g_signal_connect(usb_cancel_button, "clicked", G_CALLBACK(on_cancel_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), usb_cancel_button, FALSE, FALSE, 0);

    usb_deep_scan_button = gtk_button_new_with_label("Escaneo Profundo");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_deep_scan_button), "nw-button-secondary");
    gtk_widget_set_tooltip_text(usb_deep_scan_button, "Comparar con el snapshot de referencia sin modificarlo");
    g_signal_connect(usb_deep_scan_button, "clicked", G_CALLBACK(on_deep_scan_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), usb_deep_scan_button, FALSE, FALSE, 0);

    usb_refresh_button = gtk_button_new_with_label("Actualizar");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_refresh_button), "nw-button-primary");
    gtk_widget_set_tooltip_text(usb_refresh_button, "Tomar nuevos snapshots de referencia");
    g_signal_connect(usb_refresh_button, "clicked", G_CALLBACK(on_refresh_button_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), usb_refresh_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), toolbar, FALSE, FALSE, 0);

    usb_dial = guard_dial_new();
    gtk_box_pack_start(GTK_BOX(panel), usb_dial, FALSE, FALSE, 0);

    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);

    usb_list_box = gtk_list_box_new();
    g_signal_connect(usb_list_box, "row-selected", G_CALLBACK(on_usb_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), usb_list_box);
    gtk_box_pack_start(GTK_BOX(content_box), scrolled, TRUE, TRUE, 0);

    usb_detail_label = gtk_label_new("Seleccione un dispositivo para ver detalles");
    gtk_style_context_add_class(gtk_widget_get_style_context(usb_detail_label), "nw-row-secondary");
    gtk_label_set_line_wrap(GTK_LABEL(usb_detail_label), TRUE);
    gtk_widget_set_size_request(usb_detail_label, 220, -1);
    gtk_widget_set_valign(usb_detail_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content_box), usb_detail_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(panel), content_box, TRUE, TRUE, 0);

    usb_auto_monitor_worker = gui_periodic_worker_start(USB_AUTO_MONITOR_INTERVAL_SECONDS, usb_auto_monitor_work, NULL);

    return panel;
}

void gui_usb_panel_shutdown(void) {
    // Orden: (1) detener el monitor automatico -- gui_periodic_worker_stop()
    // marca su propio `cancel` (compartido con create_device_snapshot_ex
    // dentro de usb_auto_monitor_work/usb_handle_new_device) y hace join
    // antes de retornar, asi que al llegar aca el hilo del monitor ya no
    // toca usb_hash_pool ni el cache de snapshots. (2) si hay un escaneo
    // manual en curso, cancelarlo y unirlo tambien -- igual que
    // gui_demo_scan_shutdown(), el hilo de escaneo llama g_idle_add() con
    // on_usb_scan_done_idle justo antes de retornar, y esa idle hace su
    // propio pthread_join(usb_scan_thread, ...); despachar esa idle
    // *despues* de este join seria un doble join (comportamiento
    // indefinido). Hoy es inalcanzable: on_window_destroy llama a
    // gui_usb_panel_shutdown() y luego gtk_main_quit() sin bombear el loop
    // principal en el medio, asi que esa idle nunca llega a despacharse --
    // queda abandonada (fuga menor al cierre del proceso). (3)/(4) recien
    // entonces es seguro destruir el pool de hashes y limpiar el cache,
    // porque ningun hilo puede seguir usandolos.
    gui_periodic_worker_stop(usb_auto_monitor_worker);
    usb_auto_monitor_worker = NULL;

    if (usb_scan_running) {
        usb_cancel_flag = 1;
        pthread_join(usb_scan_thread, NULL);
        usb_scan_running = 0;
    }

    if (usb_hash_pool) {
        threadpool_destroy(usb_hash_pool);
        usb_hash_pool = NULL;
    }

    cleanup_usb_snapshot_cache();
}

// ============================================================================
// COMPATIBILIDAD CON EL COORDINADOR Y EL MENU "ESCANEAR" DEL HEADER BAR
// ============================================================================

int is_usb_monitoring_active(void) {
    return usb_auto_monitor_worker != NULL;
}

int is_gui_usb_scan_in_progress(void) {
    return usb_scan_running;
}

int get_usb_statistics_for_gui(int *total_devices, int *suspicious_devices,
                               int *total_files, int *files_with_changes) {
    if (!total_devices || !suspicious_devices || !total_files || !files_with_changes) {
        return -1;
    }

    *total_devices = 0;
    *total_files = 0;

    DeviceList *devices = monitor_connected_devices();
    if (devices) {
        *total_devices = devices->count;
        for (int i = 0; i < devices->count; i++) {
            DeviceSnapshot *snapshot = get_cached_usb_snapshot(devices->devices[i]);
            if (snapshot) *total_files += snapshot->file_count;
        }
        free_device_list(devices);
    }

    // Igual que el codigo anterior: suspicious/changed reflejan el ultimo
    // escaneo profundo -- un simple "Actualizar" no los toca. Se lee bajo
    // el mismo mutex que protege los incrementos/reinicios, porque esta
    // funcion puede ser invocada desde el hilo del coordinador del sistema
    // mientras un escaneo profundo esta en curso en el hilo de escaneo USB.
    pthread_mutex_lock(&usb_stats_mutex);
    *suspicious_devices = usb_last_scan_suspicious_devices;
    *files_with_changes = usb_last_scan_files_changed;
    pthread_mutex_unlock(&usb_stats_mutex);

    return 0;
}

void gui_compatible_scan_usb(void) {
    if (usb_scan_running) {
        gui_add_log_entry("USB_SCANNER", "INFO", "Escaneo USB ya en progreso");
        return;
    }
    start_usb_scan(USB_SCAN_MODE_REFRESH);
}
