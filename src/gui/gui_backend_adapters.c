// src/gui/gui_backend_adapters.c
#define _GNU_SOURCE
#include "gui_backend_adapters.h"
#include "gui_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <cairo.h>
#include <cairo-pdf.h>

// ============================================================================
// UTILIDADES DE CONVERSIÓN
// ============================================================================

void utf8_safe_truncate(const char *src, char *dest, size_t dest_size) {
    if (!src || !dest || dest_size == 0) {
        return;
    }

    size_t src_len = strlen(src);
    size_t copy_len = (src_len < dest_size - 1) ? src_len : dest_size - 1;

    // Retroceder mientras el byte en el límite de corte sea un byte de
    // continuación UTF-8 (10xxxxxx, 0x80-0xBF) -- cortar ahí partiría un
    // codepoint multibyte a la mitad.
    while (copy_len > 0) {
        unsigned char c = (unsigned char)src[copy_len];
        if (copy_len == src_len || (c & 0xC0) != 0x80) {
            break;
        }
        copy_len--;
    }

    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

// ============================================================================
// CACHE DE SNAPSHOTS USB
// ============================================================================

typedef struct USBSnapshotCache {
    char device_name[256];
    DeviceSnapshot *snapshot;
    struct USBSnapshotCache *next;
} USBSnapshotCache;

static USBSnapshotCache *cache_head = NULL;
// Protege la lista enlazada: el escaneo USB corre en su propio hilo mientras
// otro código puede consultarla desde el hilo principal.
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================================
// ADAPTADORES DE ESTRUCTURAS DE DATOS
// ============================================================================

int adapt_process_info_to_gui(const ProcessInfo *backend_info, GUIProcess *gui_process) {
    if (!backend_info || !gui_process) {
        return -1;
    }

    gui_process->pid = backend_info->pid;
    utf8_safe_truncate(backend_info->name, gui_process->name, sizeof(gui_process->name));
    gui_process->cpu_usage = backend_info->cpu_usage;
    gui_process->mem_usage = backend_info->mem_usage;
    gui_process->is_whitelisted = backend_info->is_whitelisted;

    // Un proceso whitelisted nunca se marca sospechoso, sin importar alertas
    // activas o uso de recursos -- evita falsos positivos en procesos de confianza.
    if (backend_info->is_whitelisted) {
        gui_process->is_suspicious = FALSE;
    } else if (backend_info->alerta_activa || backend_info->exceeds_thresholds ||
               backend_info->cpu_usage > 95.0 || backend_info->mem_usage > 90.0) {
        gui_process->is_suspicious = TRUE;
    } else {
        gui_process->is_suspicious = FALSE;
    }

    return 0;
}

int adapt_device_snapshot_to_gui(const DeviceSnapshot *snapshot,
                                 const DeviceSnapshot *previous_snapshot,
                                 GUIUSBDevice *gui_device) {
    if (!snapshot || !gui_device || !snapshot->device_name) {
        return -1;
    }

    memset(gui_device, 0, sizeof(GUIUSBDevice));

    utf8_safe_truncate(snapshot->device_name, gui_device->device_name, sizeof(gui_device->device_name));
    snprintf(gui_device->mount_point, sizeof(gui_device->mount_point), "/media/%s", snapshot->device_name);
    gui_device->total_files = snapshot->file_count;
    gui_device->last_scan = snapshot->snapshot_time;

    int files_added = 0, files_modified = 0, files_deleted = 0;
    if (previous_snapshot) {
        detect_usb_changes(previous_snapshot, snapshot, &files_added, &files_modified, &files_deleted);
        gui_device->files_changed = files_added + files_modified + files_deleted;
    } else {
        gui_device->files_changed = 0;
    }

    gui_device->is_suspicious = evaluate_usb_suspicion(files_added, files_modified,
                                                        files_deleted, snapshot->file_count);
    generate_usb_status_string(gui_device->files_changed, gui_device->is_suspicious,
                                FALSE, gui_device->status, sizeof(gui_device->status));

    return 0;
}

int adapt_port_info_to_gui(const PortInfo *backend_port, GUIPort *gui_port) {
    if (!backend_port || !gui_port) {
        return -1;
    }

    gui_port->port = backend_port->port;
    gui_port->is_suspicious = backend_port->is_suspicious;
    utf8_safe_truncate(backend_port->service_name, gui_port->service, sizeof(gui_port->service));
    generate_port_status_string(backend_port->is_open, gui_port->status, sizeof(gui_port->status));

    return 0;
}

int aggregate_port_statistics(const PortInfo *ports, int port_count,
                             int *total_open, int *total_suspicious) {
    if (!ports || !total_open || !total_suspicious) {
        return -1;
    }

    *total_open = 0;
    *total_suspicious = 0;
    for (int i = 0; i < port_count; i++) {
        if (ports[i].is_open) (*total_open)++;
        if (ports[i].is_suspicious) (*total_suspicious)++;
    }

    return 0;
}

// ============================================================================
// FUNCIONES DE DETECCIÓN DE CAMBIOS
// ============================================================================

int detect_usb_changes(const DeviceSnapshot *old_snapshot,
                      const DeviceSnapshot *new_snapshot,
                      int *files_added, int *files_modified, int *files_deleted) {
    if (!old_snapshot || !new_snapshot || !files_added || !files_modified || !files_deleted) {
        return -1;
    }

    *files_added = 0;
    *files_modified = 0;
    *files_deleted = 0;

    for (int i = 0; i < new_snapshot->file_count; i++) {
        FileInfo *new_file = new_snapshot->files[i];
        int found = 0;
        for (int j = 0; j < old_snapshot->file_count; j++) {
            FileInfo *old_file = old_snapshot->files[j];
            if (strcmp(new_file->path, old_file->path) == 0) {
                found = 1;
                if (strcmp(new_file->sha256_hash, old_file->sha256_hash) != 0) {
                    (*files_modified)++;
                }
                break;
            }
        }
        if (!found) {
            (*files_added)++;
        }
    }

    for (int i = 0; i < old_snapshot->file_count; i++) {
        FileInfo *old_file = old_snapshot->files[i];
        int found = 0;
        for (int j = 0; j < new_snapshot->file_count; j++) {
            if (strcmp(old_file->path, new_snapshot->files[j]->path) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            (*files_deleted)++;
        }
    }

    return 0;
}

gboolean evaluate_usb_suspicion(int files_added, int files_modified,
                                int files_deleted, int total_files) {
    if (files_deleted > (total_files * 0.1)) return TRUE;        // >10% eliminados
    if (files_modified > (total_files * 0.2)) return TRUE;       // >20% modificados
    int total_changes = files_added + files_modified + files_deleted;
    if (total_changes > (total_files * 0.3)) return TRUE;        // >30% de actividad
    if (total_files < 50 && files_added > 10) return TRUE;       // inyección en dispositivo pequeño
    return FALSE;
}

// ============================================================================
// GESTIÓN DE ESTADO Y CACHE
// ============================================================================

int init_usb_snapshot_cache(void) {
    return 0; // cache_head arranca en NULL estáticamente.
}

int store_usb_snapshot(const char *device_name, const DeviceSnapshot *snapshot) {
    if (!device_name || !snapshot) {
        return -1;
    }

    pthread_mutex_lock(&cache_mutex);

    USBSnapshotCache *current = cache_head;
    while (current) {
        if (strcmp(current->device_name, device_name) == 0) {
            if (current->snapshot) {
                free_device_snapshot(current->snapshot);
            }
            current->snapshot = (DeviceSnapshot *)snapshot;
            pthread_mutex_unlock(&cache_mutex);
            return 0;
        }
        current = current->next;
    }

    USBSnapshotCache *new_entry = malloc(sizeof(USBSnapshotCache));
    if (!new_entry) {
        pthread_mutex_unlock(&cache_mutex);
        return -1;
    }

    utf8_safe_truncate(device_name, new_entry->device_name, sizeof(new_entry->device_name));
    new_entry->snapshot = (DeviceSnapshot *)snapshot;
    new_entry->next = cache_head;
    cache_head = new_entry;

    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

DeviceSnapshot *get_cached_usb_snapshot(const char *device_name) {
    if (!device_name) {
        return NULL;
    }

    pthread_mutex_lock(&cache_mutex);
    USBSnapshotCache *current = cache_head;
    while (current) {
        if (strcmp(current->device_name, device_name) == 0) {
            DeviceSnapshot *snapshot = current->snapshot;
            pthread_mutex_unlock(&cache_mutex);
            return snapshot;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&cache_mutex);
    return NULL;
}

void cleanup_usb_snapshot_cache(void) {
    pthread_mutex_lock(&cache_mutex);
    USBSnapshotCache *current = cache_head;
    while (current) {
        USBSnapshotCache *next = current->next;
        if (current->snapshot) {
            free_device_snapshot(current->snapshot);
        }
        free(current);
        current = next;
    }
    cache_head = NULL;
    pthread_mutex_unlock(&cache_mutex);
}

// ============================================================================
// UTILIDADES DE FORMATEO
// ============================================================================

int format_timestamp_for_gui(time_t timestamp, char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return -1;
    }

    if (timestamp == 0) {
        utf8_safe_truncate("Nunca", buffer, buffer_size);
        return 0;
    }

    time_t now = time(NULL);
    int diff = (int)(now - timestamp);

    if (diff < 60) {
        snprintf(buffer, buffer_size, "Hace %d seg", diff);
    } else if (diff < 3600) {
        snprintf(buffer, buffer_size, "Hace %d min", diff / 60);
    } else if (diff < 86400) {
        snprintf(buffer, buffer_size, "Hace %d horas", diff / 3600);
    } else {
        snprintf(buffer, buffer_size, "Hace %d dias", diff / 86400);
    }

    return 0;
}

int generate_usb_status_string(int files_changed, gboolean is_suspicious,
                              gboolean is_scanning, char *status_buffer,
                              size_t buffer_size) {
    if (!status_buffer || buffer_size == 0) {
        return -1;
    }

    const char *text;
    if (is_scanning) text = "ESCANEANDO";
    else if (is_suspicious) text = "SOSPECHOSO";
    else if (files_changed > 0) text = "CAMBIOS DETECTADOS";
    else text = "LIMPIO";

    utf8_safe_truncate(text, status_buffer, buffer_size);
    return 0;
}

int generate_port_status_string(int is_open, char *status_buffer, size_t buffer_size) {
    if (!status_buffer || buffer_size == 0) {
        return -1;
    }
    utf8_safe_truncate(is_open ? "Abierto" : "Cerrado", status_buffer, buffer_size);
    return 0;
}

// ============================================================================
// EXPORTACIÓN DE LOGS Y REPORTES
// ============================================================================

static char *get_log_content(void) {
    extern GtkTextBuffer *log_buffer;
    if (!log_buffer) {
        return NULL;
    }
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(log_buffer, &start);
    gtk_text_buffer_get_end_iter(log_buffer, &end);
    return gtk_text_buffer_get_text(log_buffer, &start, &end, FALSE);
}

static void generate_system_stats(char *stats_buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    extern GtkWidget *stats_usb_count;
    extern GtkWidget *stats_process_count;
    extern GtkWidget *stats_ports_open;
    extern GtkWidget *stats_system_status;

    const char *usb_count = stats_usb_count ? gtk_label_get_text(GTK_LABEL(stats_usb_count)) : "N/A";
    const char *process_count = stats_process_count ? gtk_label_get_text(GTK_LABEL(stats_process_count)) : "N/A";
    const char *ports_count = stats_ports_open ? gtk_label_get_text(GTK_LABEL(stats_ports_open)) : "N/A";
    const char *system_status = stats_system_status ? gtk_label_get_text(GTK_LABEL(stats_system_status)) : "N/A";

    snprintf(stats_buffer, buffer_size,
        "=== REPORTE DE SEGURIDAD MATCOM GUARD ===\n"
        "Fecha y Hora: %s\n"
        "Estado del Sistema: %s\n"
        "Dispositivos USB Conectados: %s\n"
        "Procesos Monitoreados: %s\n"
        "Puertos Abiertos: %s\n"
        "Generado por: MatCom Guard v1.0\n\n",
        timestamp, system_status, usb_count, process_count, ports_count);
}

void gui_export_report_to_pdf(const char *filename) {
    if (!filename) {
        return;
    }

    const char *ext = strrchr(filename, '.');
    if (!ext || (strcmp(ext, ".pdf") != 0 && strcmp(ext, ".txt") != 0)) {
        gui_add_log_entry("EXPORT", "ERROR", "Formato de archivo no soportado. Use .pdf o .txt");
        return;
    }

    char *log_content = get_log_content();
    if (!log_content) {
        gui_add_log_entry("EXPORT", "ERROR", "No se pudo obtener el contenido del log");
        return;
    }

    char stats_buffer[2048];
    generate_system_stats(stats_buffer, sizeof(stats_buffer));

    if (strcmp(ext, ".txt") == 0) {
        FILE *file = fopen(filename, "w");
        if (!file) {
            gui_add_log_entry("EXPORT", "ERROR", "No se pudo crear el archivo de reporte");
            g_free(log_content);
            return;
        }

        char filtered_stats[4096];
        char filtered_log[32768];
        filter_emoji_and_special_chars(stats_buffer, filtered_stats, sizeof(filtered_stats));
        filter_emoji_and_special_chars(log_content, filtered_log, sizeof(filtered_log));

        fprintf(file, "%s", filtered_stats);
        fprintf(file, "=== LOG DE EVENTOS ===\n\n");
        fprintf(file, "%s", filtered_log);
        fclose(file);
    } else {
        cairo_surface_t *surface = cairo_pdf_surface_create(filename, 595, 842);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);
            gui_add_log_entry("EXPORT", "ERROR", "No se pudo crear la superficie PDF");
            g_free(log_content);
            return;
        }
        cairo_t *cr = cairo_create(surface);
        if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
            gui_add_log_entry("EXPORT", "ERROR", "No se pudo crear el contexto Cairo");
            g_free(log_content);
            return;
        }

        cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_source_rgb(cr, 0, 0, 0);

        double x = 50, y = 50;
        double line_height = 12;
        double page_height = 792;
        double margin_bottom = 50;
        int max_chars_per_line = 85;

        cairo_set_font_size(cr, 16);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, "REPORTE DE SEGURIDAD MATCOM GUARD");
        y += line_height * 2;
        cairo_set_font_size(cr, 10);

        char filtered_stats[4096];
        char filtered_log[32768];
        filter_emoji_and_special_chars(stats_buffer, filtered_stats, sizeof(filtered_stats));
        filter_emoji_and_special_chars(log_content, filtered_log, sizeof(filtered_log));

        char wrapped_stats[8192];
        wrap_text_for_pdf(filtered_stats, wrapped_stats, sizeof(wrapped_stats), max_chars_per_line);

        char *stats_copy = strdup(wrapped_stats);
        if (stats_copy) {
            char *line = strtok(stats_copy, "\n");
            while (line != NULL) {
                if (y > page_height - margin_bottom) {
                    cairo_show_page(cr);
                    y = 50;
                }
                cairo_move_to(cr, x, y);
                cairo_show_text(cr, line);
                y += line_height;
                line = strtok(NULL, "\n");
            }
            free(stats_copy);
        }
        y += line_height;

        if (y > page_height - margin_bottom) {
            cairo_show_page(cr);
            y = 50;
        }
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, "=== LOG DE EVENTOS ===");
        y += line_height * 1.5;

        char wrapped_log[65536];
        wrap_text_for_pdf(filtered_log, wrapped_log, sizeof(wrapped_log), max_chars_per_line);

        char *log_copy = strdup(wrapped_log);
        if (log_copy) {
            char *line = strtok(log_copy, "\n");
            while (line != NULL) {
                if (y > page_height - margin_bottom) {
                    cairo_show_page(cr);
                    y = 50;
                }
                cairo_move_to(cr, x, y);
                cairo_show_text(cr, line);
                y += line_height;
                line = strtok(NULL, "\n");
            }
            free(log_copy);
        }

        cairo_destroy(cr);
        cairo_surface_finish(surface);
        cairo_surface_destroy(surface);
    }

    g_free(log_content);

    char export_message[512];
    snprintf(export_message, sizeof(export_message), "Reporte de seguridad exportado a: %s", filename);
    gui_add_log_entry("EXPORT", "INFO", export_message);
}

void filter_emoji_and_special_chars(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return;
    }

    size_t input_len = strlen(input);
    size_t j = 0;
    for (size_t i = 0; input[i] != '\0' && j < output_size - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        if ((c >= 32 && c <= 126) || c == '\n' || c == '\t') {
            output[j++] = input[i];
        } else if (c == 0xC3 && i + 1 < input_len) {
            unsigned char next = (unsigned char)input[i + 1];
            char replacement = '?';
            switch (next) {
                case 0xA1: replacement = 'a'; break; // á
                case 0xA9: replacement = 'e'; break; // é
                case 0xAD: replacement = 'i'; break; // í
                case 0xB3: replacement = 'o'; break; // ó
                case 0xBA: replacement = 'u'; break; // ú
                case 0xB1: replacement = 'n'; break; // ñ
                case 0x81: replacement = 'A'; break; // Á
                case 0x89: replacement = 'E'; break; // É
                case 0x8D: replacement = 'I'; break; // Í
                case 0x93: replacement = 'O'; break; // Ó
                case 0x9A: replacement = 'U'; break; // Ú
                case 0x91: replacement = 'N'; break; // Ñ
                default: break;
            }
            output[j++] = replacement;
            i++; // saltar el segundo byte de la secuencia de 2 bytes
        }
        // Cualquier otro byte (emojis, UTF-8 de 3-4 bytes) se descarta.
    }
    output[j] = '\0';
}

void wrap_text_for_pdf(const char *input, char *output, size_t output_size, int max_width) {
    if (!input || !output || output_size == 0 || max_width <= 0) {
        return;
    }

    size_t input_len = strlen(input);
    size_t output_pos = 0;
    size_t line_start = 0;

    for (size_t i = 0; i <= input_len && output_pos < output_size - 1; i++) {
        if (input[i] == '\n' || input[i] == '\0') {
            size_t line_len = i - line_start;

            if (line_len <= (size_t)max_width) {
                if (output_pos + line_len < output_size - 1) {
                    memcpy(output + output_pos, input + line_start, line_len);
                    output_pos += line_len;
                    if (input[i] == '\n' && output_pos < output_size - 1) {
                        output[output_pos++] = '\n';
                    }
                }
            } else {
                size_t pos = line_start;
                while (pos < i && output_pos < output_size - 1) {
                    size_t chunk_end = pos + max_width;
                    if (chunk_end > i) chunk_end = i;

                    if (chunk_end < i) {
                        size_t last_space = chunk_end;
                        while (last_space > pos && input[last_space] != ' ') {
                            last_space--;
                        }
                        if (last_space > pos) {
                            chunk_end = last_space;
                        }
                    }

                    size_t chunk_len = chunk_end - pos;
                    if (output_pos + chunk_len < output_size - 1) {
                        memcpy(output + output_pos, input + pos, chunk_len);
                        output_pos += chunk_len;
                        if (output_pos < output_size - 1) {
                            output[output_pos++] = '\n';
                        }
                    }

                    pos = chunk_end;
                    if (pos < i && input[pos] == ' ') pos++;
                }
            }
            line_start = i + 1;
        }
    }

    output[output_pos] = '\0';
}

int count_wrapped_lines(const char *text, int max_width) {
    if (!text || max_width <= 0) {
        return 0;
    }

    int line_count = 0;
    size_t text_len = strlen(text);
    size_t line_start = 0;

    for (size_t i = 0; i <= text_len; i++) {
        if (text[i] == '\n' || text[i] == '\0') {
            size_t line_len = i - line_start;
            if (line_len <= (size_t)max_width) {
                line_count++;
            } else {
                line_count += (line_len + max_width - 1) / max_width;
            }
            line_start = i + 1;
        }
    }

    return line_count;
}
