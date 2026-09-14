#ifndef GUI_BACKEND_ADAPTERS_H
#define GUI_BACKEND_ADAPTERS_H

#include "gui.h"
#include "process_monitor.h"
#include "device_monitor.h"
#include "port_scanner.h"
#include <time.h>

// ============================================================================
// ADAPTADORES DE ESTRUCTURAS DE DATOS
// ============================================================================

/**
 * @brief Convierte ProcessInfo del backend a GUIProcess para el frontend
 * 
 * Esta función actúa como puente entre la estructura rica en datos del backend
 * de procesos y la estructura simplificada que maneja la interfaz gráfica.
 * 
 * @param backend_info Información completa del proceso desde el backend
 * @param gui_process Estructura GUI donde se almacenará la conversión
 * @return int 0 si es exitoso, -1 si hay error
 */
int adapt_process_info_to_gui(const ProcessInfo *backend_info, GUIProcess *gui_process);

/**
 * @brief Convierte DeviceSnapshot del backend a GUIUSBDevice para el frontend
 * 
 * Esta función toma el snapshot detallado de archivos de un dispositivo USB
 * y lo convierte en un resumen estadístico que puede mostrar la GUI. Calcula
 * automáticamente el número de archivos modificados comparando con snapshots
 * anteriores si están disponibles.
 * 
 * @param snapshot Snapshot completo del dispositivo desde el backend
 * @param previous_snapshot Snapshot anterior para detectar cambios (puede ser NULL)
 * @param gui_device Estructura GUI donde se almacenará el resumen
 * @return int 0 si es exitoso, -1 si hay error
 */
int adapt_device_snapshot_to_gui(const DeviceSnapshot *snapshot, 
                                 const DeviceSnapshot *previous_snapshot,
                                 GUIUSBDevice *gui_device);

/**
 * @brief Convierte PortInfo del backend a GUIPort para el frontend
 * 
 * Esta función traduce la información binaria de puertos (abierto/cerrado)
 * del backend a las cadenas de texto descriptivas que espera la GUI.
 * 
 * @param backend_port Información del puerto desde el backend
 * @param gui_port Estructura GUI donde se almacenará la conversión
 * @return int 0 si es exitoso, -1 si hay error
 */
int adapt_port_info_to_gui(const PortInfo *backend_port, GUIPort *gui_port);

/**
 * @brief Convierte un array de PortInfo a estadísticas agregadas
 * 
 * El backend de puertos retorna arrays de puertos individuales, pero la GUI
 * a menudo necesita estadísticas agregadas (total de puertos abiertos,
 * cantidad de puertos sospechosos, etc.) para mostrar en paneles de resumen.
 * 
 * @param ports Array de información de puertos del backend
 * @param port_count Número de puertos en el array
 * @param total_open Puntero donde almacenar el total de puertos abiertos
 * @param total_suspicious Puntero donde almacenar el total de puertos sospechosos
 * @return int 0 si es exitoso, -1 si hay error
 */
int aggregate_port_statistics(const PortInfo *ports, int port_count,
                             int *total_open, int *total_suspicious);

// ============================================================================
// FUNCIONES DE DETECCIÓN DE CAMBIOS
// ============================================================================

/**
 * @brief Detecta cambios entre dos snapshots de dispositivo USB
 * 
 * Compara dos snapshots del mismo dispositivo para identificar archivos
 * que han sido añadidos, modificados o eliminados. Esta función es crucial
 * para el sistema de detección de actividad maliciosa en dispositivos USB.
 * 
 * @param old_snapshot Snapshot anterior del dispositivo
 * @param new_snapshot Snapshot actual del dispositivo
 * @param files_added Puntero donde almacenar cantidad de archivos añadidos
 * @param files_modified Puntero donde almacenar cantidad de archivos modificados
 * @param files_deleted Puntero donde almacenar cantidad de archivos eliminados
 * @return int 0 si es exitoso, -1 si hay error
 */
int detect_usb_changes(const DeviceSnapshot *old_snapshot,
                      const DeviceSnapshot *new_snapshot,
                      int *files_added, int *files_modified, int *files_deleted);

/**
 * @brief Determina si un dispositivo USB debe considerarse sospechoso
 * 
 * Aplica heurísticas para evaluar si un dispositivo muestra comportamiento
 * sospechoso basándose en la cantidad y tipo de cambios detectados.
 * 
 * @param files_added Número de archivos añadidos
 * @param files_modified Número de archivos modificados
 * @param files_deleted Número de archivos eliminados
 * @param total_files Total de archivos en el dispositivo
 * @return gboolean TRUE si el dispositivo es sospechoso, FALSE si no
 */
gboolean evaluate_usb_suspicion(int files_added, int files_modified, 
                                int files_deleted, int total_files);

// ============================================================================
// GESTIÓN DE ESTADO Y CACHE
// ============================================================================

/**
 * @brief Inicializa el sistema de cache de snapshots USB
 * 
 * El adaptador necesita mantener snapshots anteriores para poder detectar
 * cambios. Esta función inicializa las estructuras de datos necesarias.
 * 
 * @return int 0 si es exitoso, -1 si hay error
 */
int init_usb_snapshot_cache(void);

/**
 * @brief Almacena un snapshot en el cache para futuras comparaciones
 * 
 * @param device_name Nombre del dispositivo
 * @param snapshot Snapshot a almacenar
 * @return int 0 si es exitoso, -1 si hay error
 */
int store_usb_snapshot(const char *device_name, const DeviceSnapshot *snapshot);

/**
 * @brief Recupera un snapshot anterior del cache
 * 
 * @param device_name Nombre del dispositivo
 * @return DeviceSnapshot* Snapshot anterior o NULL si no existe
 */
DeviceSnapshot* get_cached_usb_snapshot(const char *device_name);

/**
 * @brief Limpia el cache de snapshots USB y libera memoria
 */
void cleanup_usb_snapshot_cache(void);

// ============================================================================
// UTILIDADES DE CONVERSIÓN
// ============================================================================

/**
 * @brief Genera una cadena de estado para dispositivos USB
 * 
 * Basándose en los cambios detectados y el estado del dispositivo,
 * genera una cadena descriptiva apropiada para mostrar en la GUI.
 * 
 * @param files_changed Número de archivos que han cambiado
 * @param is_suspicious Si el dispositivo es considerado sospechoso
 * @param is_scanning Si actualmente se está escaneando el dispositivo
 * @param status_buffer Buffer donde almacenar la cadena de estado
 * @param buffer_size Tamaño del buffer
 * @return int 0 si es exitoso, -1 si hay error
 */
int generate_usb_status_string(int files_changed, gboolean is_suspicious, 
                              gboolean is_scanning, char *status_buffer, 
                              size_t buffer_size);

/**
 * @brief Convierte estado binario de puerto a cadena descriptiva
 * 
 * @param is_open Si el puerto está abierto (1) o cerrado (0)
 * @param status_buffer Buffer donde almacenar la cadena de estado
 * @param buffer_size Tamaño del buffer
 * @return int 0 si es exitoso, -1 si hay error
 */
int generate_port_status_string(int is_open, char *status_buffer, size_t buffer_size);

/**
 * @brief Copia `src` a `dest` truncando a lo sumo en `dest_size - 1` bytes,
 * sin partir una secuencia UTF-8 multibyte a la mitad. `dest` siempre queda
 * terminado en '\0'. Seguro con dest_size == 0 (no escribe nada).
 */
void utf8_safe_truncate(const char *src, char *dest, size_t dest_size);

// ============================================================================
// EXPORTACIÓN DE REPORTES
// ============================================================================

/**
 * @brief Exporta un reporte completo del sistema en formato PDF o TXT
 * 
 * Esta función genera un reporte completo que incluye:
 * - Estadísticas actuales del sistema
 * - Estado de dispositivos USB conectados
 * - Información de procesos monitoreados
 * - Historial completo del log de eventos
 * 
 * @param filename Ruta completa del archivo donde guardar el reporte (.pdf o .txt)
 */
void gui_export_report_to_pdf(const char *filename);

// ============================================================================
// UTILIDADES DE FORMATEO DE TEXTO
// ============================================================================

/**
 * @brief Filtra caracteres emoji y no ASCII de una cadena de texto
 * 
 * Esta función remueve todos los caracteres que no son ASCII básico,
 * incluyendo emojis, para asegurar compatibilidad con PDF y sistemas legacy.
 * 
 * @param input Cadena de entrada que puede contener emojis
 * @param output Buffer donde almacenar la cadena filtrada
 * @param output_size Tamaño del buffer de salida
 */
void filter_emoji_and_special_chars(const char *input, char *output, size_t output_size);

/**
 * @brief Divide una línea larga en múltiples líneas para ajustar al ancho de página
 * 
 * Esta función toma una línea que puede ser más larga que el ancho de página
 * y la divide en líneas más cortas, respetando espacios para evitar cortar palabras.
 * 
 * @param input Línea de entrada (posiblemente larga)
 * @param output Buffer donde almacenar las líneas divididas (separadas por \n)
 * @param output_size Tamaño del buffer de salida
 * @param max_width Ancho máximo permitido por línea
 */
void wrap_text_for_pdf(const char *input, char *output, size_t output_size, int max_width);

#endif // GUI_BACKEND_ADAPTERS_H