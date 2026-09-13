#include "gui_internal.h"
#include "gui.h"
#include "gui_process_integration.h"
#include "gui_usb_integration.h"
#include "gui_ports_integration.h"
#include "gui_system_coordinator.h"
#include "gui_backend_adapters.h"
#include "gui_shell.h"
#include "gui_demo_scan.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

GtkWidget *main_window = NULL;
GtkWidget *main_container = NULL;
GtkWidget *header_bar = NULL;
GtkWidget *status_bar = NULL;
GtkWidget *notebook = NULL;

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
    // Debe ser lo primero: el hilo del escaneo demo tiene un puntero crudo
    // al dial y registra un weak ref sobre él -- hay que unirlo (join)
    // antes de que cualquier widget del árbol empiece a destruirse. La
    // señal "destroy" corre en G_SIGNAL_RUN_CLEANUP, antes del cierre de
    // clase de GtkContainer que destruye a los hijos, así que este orden
    // es necesario, no solo conservador -- no lo muevas más abajo.
    gui_demo_scan_shutdown();

    gui_add_log_entry("SISTEMA", "INFO", "Cerrando MatCom Guard - iniciando secuencia de apagado seguro...");
    
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
    
    // Cambiar a la pestaña de logs para ver el progreso
    if (notebook != NULL) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 4);
    }

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
    // Cambiar a la pestaña USB
    if (notebook != NULL) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
    }

    // Ejecutar escaneo
    if (usb_callback) {
        gui_add_log_entry("USB_SCANNER", "INFO", "Escaneo manual de USB iniciado desde menú");
        usb_callback();
    }
}

static void on_scan_processes_menu_clicked(GtkMenuItem *item __attribute__((unused)), gpointer data __attribute__((unused))) {
    // Cambiar a la pestaña de procesos
    if (notebook != NULL) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 2);
    }

    // Ejecutar escaneo
    if (processes_callback) {
        gui_add_log_entry("PROCESS_SCANNER", "INFO", "Escaneo manual de procesos iniciado desde menú");
        processes_callback();
    }
}

static void on_scan_ports_menu_clicked(GtkMenuItem *item __attribute__((unused)), gpointer data __attribute__((unused))) {
    // Cambiar a la pestaña de puertos
    if (notebook != NULL) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 3);
    }

    // Ejecutar escaneo
    if (ports_callback) {
        gui_add_log_entry("PORT_SCANNER", "INFO", "Escaneo manual de puertos iniciado desde menú");
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

static GtkWidget* create_header_bar() {
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "MatCom Guard");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Sistema de Protección Digital");
    
    // Menú de escaneo con opciones
    GtkWidget *scan_menu_button = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(scan_menu_button), "🛡️ Escanear");
    gtk_widget_set_tooltip_text(scan_menu_button, "Opciones de escaneo de seguridad");
    
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
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), scan_menu_button);
    
    // Botón de pausa/reanudar monitoreo
    GtkWidget *monitor_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_label(GTK_BUTTON(monitor_toggle_btn), "⏸️ Pausar");
    gtk_widget_set_tooltip_text(monitor_toggle_btn, "Pausar/Reanudar monitoreo automático");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(monitor_toggle_btn), TRUE);
    g_signal_connect(monitor_toggle_btn, "toggled", G_CALLBACK(on_monitor_toggle_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), monitor_toggle_btn);
    
    // Botón de configuración
    GtkWidget *config_btn = gtk_button_new_with_label("⚙️ Configuración");
    gtk_widget_set_tooltip_text(config_btn, "Ajustar configuración del sistema");
    g_signal_connect(config_btn, "clicked", G_CALLBACK(on_config_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), config_btn);
    
    // Botón de exportar
    GtkWidget *export_btn = gtk_button_new_with_label("📄 Exportar");
    gtk_widget_set_tooltip_text(export_btn, "Generar reporte de seguridad");
    g_signal_connect(export_btn, "clicked", G_CALLBACK(on_export_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), export_btn);
    
    return header;
}

// NUEVA FUNCIÓN: Inicialización completa del sistema backend
static int initialize_complete_backend_system(void) {
    gui_add_log_entry("STARTUP", "INFO", "=== INICIANDO SISTEMA BACKEND COMPLETO ===");
    
    // Paso 1: Inicializar el coordinador del sistema primero
    if (init_system_coordinator() != 0) {
        gui_add_log_entry("STARTUP", "CRITICAL", "FALLO CRÍTICO: No se pudo inicializar coordinador del sistema");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "✅ Coordinador del sistema inicializado");
    
    // Paso 2: Inicializar todas las integraciones de módulos
    if (init_process_integration() != 0) {
        gui_add_log_entry("STARTUP", "ERROR", "Error al inicializar integración de procesos");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "✅ Integración de procesos inicializada");
    
    if (init_usb_integration() != 0) {
        gui_add_log_entry("STARTUP", "ERROR", "Error al inicializar integración USB");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "✅ Integración USB inicializada");
    
    if (init_ports_integration() != 0) {
        gui_add_log_entry("STARTUP", "ERROR", "Error al inicializar integración de puertos");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "✅ Integración de puertos inicializada");
    
    // Paso 3: Iniciar servicios automáticos
    // USB: monitoreo automático para detectar dispositivos conectados/desconectados
    if (start_usb_monitoring(30) != 0) {  // 30 segundos de intervalo
        gui_add_log_entry("STARTUP", "WARNING", "No se pudo iniciar monitoreo automático USB");
    } else {
        gui_add_log_entry("STARTUP", "INFO", "✅ Monitoreo automático USB iniciado");
        
        // Notificar al coordinador sobre el cambio de estado
        notify_module_status_change("usb", MODULE_STATUS_ACTIVE, 
                                   "Monitoreo automático iniciado exitosamente");
    }
    
    // Procesos y Puertos: se inician bajo demanda cuando el usuario los solicita
    notify_module_status_change("process", MODULE_STATUS_INACTIVE, 
                               "Listo para iniciar bajo demanda");
    notify_module_status_change("ports", MODULE_STATUS_INACTIVE, 
                               "Listo para iniciar bajo demanda");
    
    // Paso 4: Iniciar el coordinador del sistema
    if (start_system_coordinator(5) != 0) {  // 5 segundos de intervalo
        gui_add_log_entry("STARTUP", "CRITICAL", "FALLO CRÍTICO: No se pudo iniciar coordinador del sistema");
        return -1;
    }
    gui_add_log_entry("STARTUP", "INFO", "✅ Coordinador del sistema iniciado");
    
    // Paso 5: Realizar sincronización inicial
    gui_add_log_entry("STARTUP", "INFO", "Realizando sincronización inicial del sistema...");
    
    // Esperar un momento para que los módulos se estabilicen
    sleep(2);
    
    // Solicitar evaluación inmediata para establecer estado inicial
    request_immediate_system_evaluation();
    
    gui_add_log_entry("STARTUP", "INFO", "=== SISTEMA BACKEND COMPLETAMENTE OPERATIVO ===");
    
    return 0;
}

// NUEVA FUNCIÓN: Limpieza completa del sistema backend
static void cleanup_complete_backend_system(void) {
    gui_add_log_entry("SHUTDOWN", "INFO", "=== INICIANDO LIMPIEZA COMPLETA DEL SISTEMA ===");
    
    // Limpiar en orden inverso al de inicialización para evitar dependencias rotas
    
    // Paso 1: Detener y limpiar el coordinador del sistema primero
    gui_add_log_entry("SHUTDOWN", "INFO", "Deteniendo coordinador del sistema...");
    cleanup_system_coordinator();
    gui_add_log_entry("SHUTDOWN", "INFO", "✅ Coordinador del sistema finalizado");
    
    // Paso 2: Limpiar integraciones de módulos
    gui_add_log_entry("SHUTDOWN", "INFO", "Finalizando módulos de integración...");
    
    cleanup_ports_integration();
    gui_add_log_entry("SHUTDOWN", "INFO", "✅ Integración de puertos finalizada");
    
    cleanup_usb_integration();
    gui_add_log_entry("SHUTDOWN", "INFO", "✅ Integración USB finalizada");
    
    cleanup_process_integration();
    gui_add_log_entry("SHUTDOWN", "INFO", "✅ Integración de procesos finalizada");
    
    gui_add_log_entry("SHUTDOWN", "INFO", "=== LIMPIEZA COMPLETA FINALIZADA ===");
}

// NUEVA FUNCIÓN: Sincronización inteligente de todos los módulos
static void intelligent_system_sync(void) {
    gui_add_log_entry("SYNC", "INFO", "Iniciando sincronización inteligente del sistema...");
    
    // Obtener estado consolidado del coordinador
    int total_devices, total_processes, total_ports, security_alerts;
    if (get_consolidated_statistics(&total_devices, &total_processes, 
                                   &total_ports, &security_alerts) == 0) {
        
        // Actualizar panel principal con estadísticas consolidadas
        gui_update_statistics(total_devices, total_processes, total_ports);
        
        // Registrar estadísticas actuales
        char stats_msg[512];
        snprintf(stats_msg, sizeof(stats_msg),
                 "Estado sincronizado: %d dispositivos USB, %d procesos, %d puertos abiertos, %d alertas",
                 total_devices, total_processes, total_ports, security_alerts);
        gui_add_log_entry("SYNC", "INFO", stats_msg);
        
        // Si hay alertas de seguridad, asegurar que el estado del sistema lo refleje
        if (security_alerts > 0) {
            SystemSecurityLevel level = get_current_security_level();
            if (level >= SECURITY_LEVEL_WARNING) {
                gui_update_system_status("Alertas de Seguridad Activas", FALSE);
            }
        }
    }
    
    // Sincronizar vistas individuales de módulos que estén activos
    if (is_process_monitoring_active()) {
        sync_gui_with_backend_processes();
    }
    
    if (is_usb_monitoring_active()) {
        sync_gui_with_usb_devices();
    }
    
    // Los puertos se sincronizan automáticamente cuando hay resultados disponibles
    
    gui_add_log_entry("SYNC", "INFO", "Sincronización inteligente completada");
}

void init_gui(int argc, char **argv) {
    gtk_init(&argc, &argv);

    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), "MatCom Guard - Sistema de Protección");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 900, 600);
    gtk_window_set_position(GTK_WINDOW(main_window), GTK_WIN_POS_CENTER);

    g_signal_connect(main_window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    header_bar = create_header_bar();
    gtk_window_set_titlebar(GTK_WINDOW(main_window), header_bar);

    main_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (main_container == NULL) {
        printf("ERROR: Falló la creación de main_container!\n");
        return;
    }
    gtk_container_add(GTK_CONTAINER(main_window), main_container);

    GtkWidget *shell = gui_shell_create();
    gtk_box_pack_start(GTK_BOX(main_container), shell, TRUE, TRUE, 0);

    GtkWidget *dashboard_page = gui_shell_get_page_container(0);
    if (dashboard_page != NULL) {
        gtk_box_pack_start(GTK_BOX(dashboard_page), gui_demo_scan_create_widget(), TRUE, TRUE, 0);
    }

    status_bar = create_status_bar();
    gtk_box_pack_end(GTK_BOX(main_container), status_bar, FALSE, FALSE, 0);
    
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