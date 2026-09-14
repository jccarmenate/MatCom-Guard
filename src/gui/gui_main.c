#include "gui_internal.h"
#include "gui.h"
#include "gui_process_panel.h"
#include "gui_usb_panel.h"
#include "gui_ports_panel.h"
#include "gui_system_coordinator.h"
#include "gui_backend_adapters.h"
#include "gui_shell.h"
#include "gui_dashboard_panel.h"
#include "gui_logs_panel.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

GtkWidget *main_window = NULL;
GtkWidget *main_container = NULL;

// Variables de callbacks del backend
ScanUSBCallback usb_callback = NULL;
ScanProcessesCallback processes_callback = NULL;
ScanPortsCallback ports_callback = NULL;
ExportReportCallback report_callback = NULL;

// Declaraciones de funciones
static void on_window_destroy(GtkWidget *widget, gpointer data);
static int initialize_complete_backend_system(void);
static void cleanup_complete_backend_system(void);
static void intelligent_system_sync(void);
static gboolean gui_set_scanning_status_timeout(gpointer user_data);
static gboolean intelligent_system_sync_timeout(gpointer user_data);

static void on_window_destroy(GtkWidget *widget __attribute__((unused)), gpointer data __attribute__((unused))) {
    gui_add_log_entry("SISTEMA", "INFO", "Cerrando MatCom Guard - iniciando secuencia de apagado seguro...");

    // Detener el coordinador PRIMERO: su hilo de fondo lee el cache de
    // snapshots USB y otro estado que los paneles liberan a continuacion --
    // si el coordinador siguiera activo mientras gui_usb_panel_shutdown()
    // libera ese cache, un tick concurrente del coordinador leeria memoria
    // ya liberada. stop_system_coordinator() hace join de su hilo antes de
    // retornar, asi que despues de esta linea no hay ningun lector activo.
    stop_system_coordinator();

    // Detener el panel USB (monitoreo automatico + escaneo manual en curso)
    // antes de tocar el resto del sistema backend.
    gui_usb_panel_shutdown();
    gui_process_panel_shutdown();
    gui_ports_panel_shutdown();

    // Realizar limpieza completa del sistema backend
    cleanup_complete_backend_system();
    
    gui_add_log_entry("SISTEMA", "INFO", "MatCom Guard cerrado exitosamente - todos los recursos liberados");
    
    printf("\n🛡️ MatCom Guard cerrado de manera segura\n");
    printf("   ✅ Todos los hilos terminados correctamente\n");
    printf("   ✅ Todos los recursos liberados\n");
    printf("   ✅ Estado del sistema guardado\n");
    printf("   Hasta la próxima protección! 👋\n\n");
    
    gtk_main_quit();
}

// Callbacks para los botones del header
static void on_scan_all_clicked(GtkMenuItem *item __attribute__((unused)), gpointer data __attribute__((unused))) {
    gui_add_log_entry("SCANNER", "INFO", "Iniciando escaneo completo del sistema");
    gui_set_scanning_status(TRUE);

    // Ejecutar todos los escaneos
    if (usb_callback) usb_callback();
    if (processes_callback) processes_callback();
    if (ports_callback) ports_callback();
    
    g_timeout_add_seconds(5, gui_set_scanning_status_timeout, GINT_TO_POINTER(FALSE));
}

// Wrapper con firma gboolean(gpointer) requerida por g_timeout_add_seconds;
// gui_set_scanning_status() devuelve void, así que un cast directo a
// GSourceFunc dejaría el valor de retorno leído por GLib indefinido.
static gboolean gui_set_scanning_status_timeout(gpointer user_data) {
    gui_set_scanning_status(GPOINTER_TO_INT(user_data));
    return G_SOURCE_REMOVE;
}

static void on_scan_usb_menu_clicked(GtkMenuItem *item __attribute__((unused)), gpointer data __attribute__((unused))) {
    if (usb_callback) {
        gui_add_log_entry("USB_SCANNER", "INFO", "Escaneo manual de USB iniciado desde menu");
        usb_callback();
    }
}

static void on_scan_processes_menu_clicked(GtkMenuItem *item __attribute__((unused)), gpointer data __attribute__((unused))) {
    if (processes_callback) {
        gui_add_log_entry("PROCESS_SCANNER", "INFO", "Escaneo manual de procesos iniciado desde menu");
        processes_callback();
    }
}

static void on_scan_ports_menu_clicked(GtkMenuItem *item __attribute__((unused)), gpointer data __attribute__((unused))) {
    if (ports_callback) {
        gui_add_log_entry("PORT_SCANNER", "INFO", "Escaneo manual de puertos iniciado desde menu");
        ports_callback();
    }
}

static void on_monitor_toggle_clicked(GtkToggleButton *button, gpointer data __attribute__((unused))) {
    gboolean is_active = gtk_toggle_button_get_active(button);
    
    if (is_active) {
        gtk_button_set_label(GTK_BUTTON(button), "⏸️ Pausar");
        gui_add_log_entry("SISTEMA", "INFO", "Monitoreo automático reanudado");
        gui_update_system_status("Sistema Operativo", TRUE);
    } else {
        gtk_button_set_label(GTK_BUTTON(button), "▶️ Reanudar");
        gui_add_log_entry("SISTEMA", "WARNING", "Monitoreo automático pausado");
        gui_update_system_status("Monitoreo Pausado", FALSE);
    }
}

static void on_config_clicked(GtkButton *button __attribute__((unused)), gpointer data __attribute__((unused))) {
    gui_add_log_entry("CONFIG", "INFO", "Abriendo ventana de configuración");
    show_config_dialog(GTK_WINDOW(main_window));
}

static void on_export_clicked(GtkButton *button __attribute__((unused)), gpointer data __attribute__((unused))) {
    // Crear diálogo de guardar archivo
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Exportar Reporte de Seguridad",
                                                    GTK_WINDOW(main_window),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Cancelar", GTK_RESPONSE_CANCEL,
                                                    "_Guardar", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    
    // Configurar nombre predeterminado
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char filename[256];
    strftime(filename, sizeof(filename), "MatComGuard_Report_%Y%m%d_%H%M%S.pdf", tm);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), filename);
    
    // Agregar filtros
    GtkFileFilter *pdf_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(pdf_filter, "Documentos PDF");
    gtk_file_filter_add_pattern(pdf_filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), pdf_filter);
    
    GtkFileFilter *txt_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(txt_filter, "Archivos de texto");
    gtk_file_filter_add_pattern(txt_filter, "*.txt");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), txt_filter);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        
        if (report_callback) {
            report_callback(filename);
        }
        
        g_free(filename);
    }
    
    gtk_widget_destroy(dialog);
}

// Empaqueta los botones de acción (antes en el GtkHeaderBar nativo) dentro
// del contenedor vacío que expone gui_shell_get_action_bar(), en la barra
// superior del shell Night Watch. Debe llamarse después de gui_shell_create().
static void create_action_bar_buttons(void) {
    GtkWidget *action_bar = gui_shell_get_action_bar();
    if (action_bar == NULL) {
        return;
    }

    // Menú de escaneo con opciones
    GtkWidget *scan_menu_button = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(scan_menu_button), "🛡️ Escanear");
    gtk_widget_set_tooltip_text(scan_menu_button, "Opciones de escaneo de seguridad");
    gtk_style_context_add_class(gtk_widget_get_style_context(scan_menu_button), "nw-button-primary");

    // Crear el menú
    GtkWidget *scan_menu = gtk_menu_new();

    // Opción: Escaneo completo
    GtkWidget *scan_all_item = gtk_menu_item_new_with_label("🛡️ Escaneo Completo");
    g_signal_connect(scan_all_item, "activate", G_CALLBACK(on_scan_all_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(scan_menu), scan_all_item);

    // Separador
    gtk_menu_shell_append(GTK_MENU_SHELL(scan_menu), gtk_separator_menu_item_new());

    // Opciones individuales
    GtkWidget *scan_usb_item = gtk_menu_item_new_with_label("💾 Escanear Dispositivos USB");
    g_signal_connect(scan_usb_item, "activate", G_CALLBACK(on_scan_usb_menu_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(scan_menu), scan_usb_item);

    GtkWidget *scan_proc_item = gtk_menu_item_new_with_label("⚡ Escanear Procesos");
    g_signal_connect(scan_proc_item, "activate", G_CALLBACK(on_scan_processes_menu_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(scan_menu), scan_proc_item);

    GtkWidget *scan_ports_item = gtk_menu_item_new_with_label("🔌 Escanear Puertos");
    g_signal_connect(scan_ports_item, "activate", G_CALLBACK(on_scan_ports_menu_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(scan_menu), scan_ports_item);

    gtk_widget_show_all(scan_menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(scan_menu_button), scan_menu);
    gtk_box_pack_start(GTK_BOX(action_bar), scan_menu_button, FALSE, FALSE, 0);

    // Botón de pausa/reanudar monitoreo
    GtkWidget *monitor_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_label(GTK_BUTTON(monitor_toggle_btn), "⏸️ Pausar");
    gtk_widget_set_tooltip_text(monitor_toggle_btn, "Pausar/Reanudar monitoreo automático");
    gtk_style_context_add_class(gtk_widget_get_style_context(monitor_toggle_btn), "nw-button-secondary");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(monitor_toggle_btn), TRUE);
    g_signal_connect(monitor_toggle_btn, "toggled", G_CALLBACK(on_monitor_toggle_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), monitor_toggle_btn, FALSE, FALSE, 0);

    // Botón de configuración
    GtkWidget *config_btn = gtk_button_new_with_label("⚙️ Configuración");
    gtk_widget_set_tooltip_text(config_btn, "Ajustar configuración del sistema");
    gtk_style_context_add_class(gtk_widget_get_style_context(config_btn), "nw-button-secondary");
    g_signal_connect(config_btn, "clicked", G_CALLBACK(on_config_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), config_btn, FALSE, FALSE, 0);

    // Botón de exportar
    GtkWidget *export_btn = gtk_button_new_with_label("📄 Exportar");
    gtk_widget_set_tooltip_text(export_btn, "Generar reporte de seguridad");
    gtk_style_context_add_class(gtk_widget_get_style_context(export_btn), "nw-button-secondary");
    g_signal_connect(export_btn, "clicked", G_CALLBACK(on_export_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(action_bar), export_btn, FALSE, FALSE, 0);
}

static int initialize_complete_backend_system(void) {
    gui_add_log_entry("STARTUP", "INFO", "=== INICIANDO SISTEMA BACKEND COMPLETO ===");

    if (init_system_coordinator() != 0) {
        gui_add_log_entry("STARTUP", "CRITICAL", "FALLO CRITICO: No se pudo inicializar coordinador del sistema");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "Coordinador del sistema inicializado");

    // USB, Procesos y Puertos ya se inicializaron cuando gui_usb_panel_create()/
    // gui_process_panel_create()/gui_ports_panel_create() se llamaron mas
    // arriba en init_gui() -- cada panel administra su propio ciclo de vida,
    // no hay un paso de "init" separado por modulo como antes.
    notify_module_status_change("process", MODULE_STATUS_INACTIVE, "Listo para iniciar bajo demanda");
    notify_module_status_change("ports", MODULE_STATUS_INACTIVE, "Listo para iniciar bajo demanda");

    if (start_system_coordinator(5) != 0) {
        gui_add_log_entry("STARTUP", "CRITICAL", "FALLO CRITICO: No se pudo iniciar coordinador del sistema");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "Coordinador del sistema iniciado");

    gui_add_log_entry("STARTUP", "INFO", "Realizando sincronizacion inicial del sistema...");
    sleep(2);
    request_immediate_system_evaluation();

    gui_add_log_entry("STARTUP", "INFO", "=== SISTEMA BACKEND COMPLETAMENTE OPERATIVO ===");
    return 0;
}

static void cleanup_complete_backend_system(void) {
    gui_add_log_entry("SHUTDOWN", "INFO", "=== INICIANDO LIMPIEZA COMPLETA DEL SISTEMA ===");
    gui_add_log_entry("SHUTDOWN", "INFO", "Deteniendo coordinador del sistema...");
    cleanup_system_coordinator();
    gui_add_log_entry("SHUTDOWN", "INFO", "Coordinador del sistema finalizado");
    gui_add_log_entry("SHUTDOWN", "INFO", "=== LIMPIEZA COMPLETA FINALIZADA ===");
}

static void intelligent_system_sync(void) {
    gui_add_log_entry("SYNC", "INFO", "Iniciando sincronizacion inteligente del sistema...");

    int total_devices, total_processes, total_ports, security_alerts;
    if (get_consolidated_statistics(&total_devices, &total_processes, &total_ports, &security_alerts) == 0) {
        gui_update_statistics(total_devices, total_processes, total_ports);

        char stats_msg[512];
        snprintf(stats_msg, sizeof(stats_msg),
                 "Estado sincronizado: %d dispositivos USB, %d procesos, %d puertos abiertos, %d alertas",
                 total_devices, total_processes, total_ports, security_alerts);
        gui_add_log_entry("SYNC", "INFO", stats_msg);

        if (security_alerts > 0) {
            SystemSecurityLevel level = get_current_security_level();
            if (level >= SECURITY_LEVEL_WARNING) {
                gui_update_system_status("Alertas de Seguridad Activas", FALSE);
            }
        }
    }

    // USB ya se mantiene sincronizado por su propio gui_periodic_worker
    // (con una primera invocacion inmediata al crear el panel en init_gui) --
    // no hace falta un empujon manual aca como en el codigo anterior.
    if (is_process_monitoring_active()) {
        sync_gui_with_backend_processes();
    }

    gui_add_log_entry("SYNC", "INFO", "Sincronizacion inteligente completada");
}

void init_gui(int argc, char **argv) {
    gtk_init(&argc, &argv);

    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), "MatCom Guard - Sistema de Protección");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 900, 600);
    gtk_window_set_position(GTK_WINDOW(main_window), GTK_WIN_POS_CENTER);

    g_signal_connect(main_window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    main_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (main_container == NULL) {
        printf("ERROR: Falló la creación de main_container!\n");
        return;
    }
    gtk_container_add(GTK_CONTAINER(main_window), main_container);

    GtkWidget *shell = gui_shell_create();
    gtk_box_pack_start(GTK_BOX(main_container), shell, TRUE, TRUE, 0);
    create_action_bar_buttons();

    GtkWidget *dashboard_page = gui_shell_get_page_container(0);
    if (dashboard_page != NULL) {
        gtk_box_pack_start(GTK_BOX(dashboard_page), gui_dashboard_panel_create(), TRUE, TRUE, 0);
    }

    GtkWidget *usb_page = gui_shell_get_page_container(1);
    if (usb_page != NULL) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(usb_page));
        for (GList *l = children; l != NULL; l = l->next) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children);
        gtk_widget_set_halign(usb_page, GTK_ALIGN_FILL);
        gtk_widget_set_valign(usb_page, GTK_ALIGN_FILL);
        gtk_box_pack_start(GTK_BOX(usb_page), gui_usb_panel_create(), TRUE, TRUE, 0);
    }

    GtkWidget *process_page = gui_shell_get_page_container(2);
    if (process_page != NULL) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(process_page));
        for (GList *l = children; l != NULL; l = l->next) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children);
        gtk_widget_set_halign(process_page, GTK_ALIGN_FILL);
        gtk_widget_set_valign(process_page, GTK_ALIGN_FILL);
        gtk_box_pack_start(GTK_BOX(process_page), gui_process_panel_create(), TRUE, TRUE, 0);
    }

    GtkWidget *ports_page = gui_shell_get_page_container(3);
    if (ports_page != NULL) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(ports_page));
        for (GList *l = children; l != NULL; l = l->next) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children);
        gtk_widget_set_halign(ports_page, GTK_ALIGN_FILL);
        gtk_widget_set_valign(ports_page, GTK_ALIGN_FILL);
        gtk_box_pack_start(GTK_BOX(ports_page), gui_ports_panel_create(), TRUE, TRUE, 0);
    }

    GtkWidget *logs_page = gui_shell_get_page_container(4);
    if (logs_page != NULL) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(logs_page));
        for (GList *l = children; l != NULL; l = l->next) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children);
        gtk_widget_set_halign(logs_page, GTK_ALIGN_FILL);
        gtk_widget_set_valign(logs_page, GTK_ALIGN_FILL);
        gtk_box_pack_start(GTK_BOX(logs_page), gui_logs_panel_create(), TRUE, TRUE, 0);
    }

    // ========================================================================
    // INTEGRACIÓN COMPLETA DEL SISTEMA BACKEND
    // ========================================================================
    
    // Configurar TODOS los callbacks con el sistema integrado completo
    gui_set_scan_callbacks(
        gui_compatible_scan_usb,        // USB totalmente integrado
        gui_compatible_scan_processes,  // Procesos totalmente integrado
        gui_compatible_scan_ports,      // Puertos totalmente integrado
        gui_export_report_to_pdf        // Export callback
    );
    
    gtk_widget_show_all(main_window);
    
    // Mensajes de consola para el desarrollador
    printf("\n");
    printf("🛡️  MatCom Guard - Sistema de Protección Digital\n");
    printf("===============================================\n");
    printf("   ✅ Frontend GUI: Completamente funcional\n");
    printf("   ✅ Backend Integrado: Procesos, USB, Puertos\n");
    printf("   ✅ Coordinador del Sistema: Activo\n");
    printf("   ✅ Monitoreo Automático: USB en tiempo real\n");
    printf("   ✅ Threading: Multi-hilo coordinado\n");
    printf("   ✅ Estado Global: Sincronizado\n");
    printf("   🔄 Sistema completamente operativo\n");
    printf("===============================================\n");
    printf("   Ventana principal: %dx%d píxeles\n", 900, 600);
    printf("   Navegación: 5 secciones (Dashboard, USB, Procesos, Puertos, Logs)\n");
    printf("   Backend real: Totalmente integrado\n");
    printf("   Estado: LISTO PARA PROTECCIÓN EN TIEMPO REAL\n\n");
    
    // Mensajes de inicio para el log de la aplicación
    gui_add_log_entry("SISTEMA", "INFO", "MatCom Guard iniciado con backend completo integrado");
    gui_add_log_entry("SISTEMA", "INFO", "Todos los módulos (Procesos, USB, Puertos) están operativos");
    gui_add_log_entry("SISTEMA", "INFO", "Monitoreo automático USB activo - detectará dispositivos automáticamente");
    gui_add_log_entry("SISTEMA", "INFO", "Sistema listo para protección en tiempo real");
    
    // Configurar el estado inicial del sistema como operativo
    gui_update_system_status("Sistema Operativo", TRUE);

    // Inicializar el sistema backend completo
    if (initialize_complete_backend_system() != 0) {
        gui_add_log_entry("STARTUP", "CRITICAL", 
                         "FALLO CRÍTICO: No se pudo inicializar sistema backend");
        
        // Mostrar dialog de error crítico al usuario
        GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(main_window),
                                                        GTK_DIALOG_MODAL,
                                                        GTK_MESSAGE_ERROR,
                                                        GTK_BUTTONS_OK,
                                                        "Error crítico al inicializar MatCom Guard.\n"
                                                        "Revise los logs para más detalles.");
        gtk_dialog_run(GTK_DIALOG(error_dialog));
        gtk_widget_destroy(error_dialog);
        
        // Continuar con GUI limitada
        gui_update_system_status("Error de Inicialización", FALSE);
    } else {
        // Inicialización exitosa
        gui_add_log_entry("STARTUP", "INFO", "🎉 MatCom Guard completamente inicializado y operativo");
        
        // Realizar sincronización inicial después de un breve delay
        g_timeout_add_seconds(3, intelligent_system_sync_timeout, NULL);
    }

    // Iniciar el bucle principal de GTK
    gtk_main();
}

// Wrapper con firma gboolean(gpointer) requerida por g_timeout_add_seconds;
// ver gui_set_scanning_status_timeout() más arriba para la misma razón.
static gboolean intelligent_system_sync_timeout(gpointer user_data) {
    (void)user_data;
    intelligent_system_sync();
    return G_SOURCE_REMOVE;
}

void gui_set_scan_callbacks(
    ScanUSBCallback usb_cb,
    ScanProcessesCallback proc_cb,
    ScanPortsCallback port_cb,
    ExportReportCallback report_cb
) {
    usb_callback = usb_cb;
    processes_callback = proc_cb;
    ports_callback = port_cb;
    report_callback = report_cb;
    
    printf("✅ Callbacks del backend configurados correctamente\n");
}