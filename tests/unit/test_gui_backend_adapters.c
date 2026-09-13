// tests/unit/test_gui_backend_adapters.c
// _GNU_SOURCE se necesita para exponer strdup() bajo -std=c99 estricto --
// mismo patrón que usa tests/unit/test_hash_skip.c (glibc oculta strdup()
// si no se pide soporte extendido al compilar en modo ISO C99 puro).
#define _GNU_SOURCE
#include "gui_backend_adapters.h"
#include "gui.h"
#include "process_monitor.h"
#include "device_monitor.h"
#include "port_scanner.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void test_utf8_safe_truncate(void) {
    char dest[8];

    // Cadena corta: copia completa.
    utf8_safe_truncate("abc", dest, sizeof(dest));
    assert(strcmp(dest, "abc") == 0);

    // Cadena ASCII larga: trunca en dest_size - 1.
    utf8_safe_truncate("abcdefghij", dest, sizeof(dest));
    assert(strlen(dest) == 7);

    // "café" repetido para forzar el corte justo sobre un byte de continuación
    // UTF-8 (0xA9 en "é" = 0xC3 0xA9): un buffer de 4 bytes sólo tiene espacio
    // para "caf" + NUL si se corta ANTES de la secuencia de 2 bytes de la é.
    char small[4];
    utf8_safe_truncate("café", small, sizeof(small));
    assert(strlen(small) <= 3);
    // El resultado nunca termina con un byte de continuación 0x80-0xBF colgado.
    size_t len = strlen(small);
    if (len > 0) {
        unsigned char last = (unsigned char)small[len - 1];
        assert(!(last >= 0x80 && last <= 0xBF));
    }

    // dest_size == 0: no debe escribir nada (no hay espacio ni para el NUL).
    char untouched[1] = { 'X' };
    utf8_safe_truncate("abc", untouched, 0);
    assert(untouched[0] == 'X');
}

static void test_adapt_process_info_to_gui(void) {
    ProcessInfo info = {0};
    info.pid = 1234;
    strcpy(info.name, "firefox");
    info.cpu_usage = 96.0f;
    info.mem_usage = 10.0f;
    info.is_whitelisted = 1;
    info.alerta_activa = 1; // Debe ser ignorado: whitelist tiene prioridad absoluta.
    info.exceeds_thresholds = 1;

    GUIProcess gp;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.pid == 1234);
    assert(strcmp(gp.name, "firefox") == 0);
    assert(gp.is_whitelisted == TRUE);
    assert(gp.is_suspicious == FALSE); // whitelist gana sobre alerta_activa/exceeds_thresholds/CPU alta

    // No whitelisted + alerta activa -> sospechoso.
    info.is_whitelisted = 0;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.is_suspicious == TRUE);

    // No whitelisted, sin alerta ni exceeds_thresholds, pero CPU > 95 -> sospechoso.
    info.alerta_activa = 0;
    info.exceeds_thresholds = 0;
    info.cpu_usage = 96.0f;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.is_suspicious == TRUE);

    // Nada activado y bajo uso -> no sospechoso.
    info.cpu_usage = 10.0f;
    info.mem_usage = 10.0f;
    assert(adapt_process_info_to_gui(&info, &gp) == 0);
    assert(gp.is_suspicious == FALSE);

    assert(adapt_process_info_to_gui(NULL, &gp) == -1);
    assert(adapt_process_info_to_gui(&info, NULL) == -1);
}

static void test_evaluate_usb_suspicion(void) {
    assert(evaluate_usb_suspicion(0, 0, 0, 100) == FALSE);
    assert(evaluate_usb_suspicion(0, 0, 15, 100) == TRUE);   // >10% eliminados
    assert(evaluate_usb_suspicion(0, 25, 0, 100) == TRUE);   // >20% modificados
    assert(evaluate_usb_suspicion(20, 10, 5, 100) == TRUE);  // >30% de actividad total
    assert(evaluate_usb_suspicion(15, 0, 0, 30) == TRUE);    // dispositivo pequeño + muchos nuevos
    assert(evaluate_usb_suspicion(1, 1, 1, 100) == FALSE);
}

static FileInfo *make_file(const char *path, const char *hash) {
    FileInfo *f = malloc(sizeof(FileInfo));
    f->path = strdup(path);
    f->name = strdup(path);
    f->extension = strdup("");
    f->size = 0;
    strncpy(f->sha256_hash, hash, sizeof(f->sha256_hash) - 1);
    f->sha256_hash[sizeof(f->sha256_hash) - 1] = '\0';
    f->permissions = 0644;
    f->last_modified = 0;
    f->last_accessed = 0;
    return f;
}

static DeviceSnapshot *make_snapshot(const char *device_name, int file_count) {
    DeviceSnapshot *s = malloc(sizeof(DeviceSnapshot));
    s->device_name = strdup(device_name);
    s->files = malloc(sizeof(FileInfo *) * (file_count > 0 ? file_count : 1));
    s->file_count = 0;
    s->capacity = file_count > 0 ? file_count : 1;
    s->snapshot_time = 1000;
    return s;
}

static void test_detect_usb_changes(void) {
    DeviceSnapshot *old_snap = make_snapshot("USB1", 2);
    old_snap->files[old_snap->file_count++] = make_file("/media/USB1/a.txt", "hash_a");
    old_snap->files[old_snap->file_count++] = make_file("/media/USB1/b.txt", "hash_b");

    DeviceSnapshot *new_snap = make_snapshot("USB1", 2);
    new_snap->files[new_snap->file_count++] = make_file("/media/USB1/a.txt", "hash_a_CHANGED");
    new_snap->files[new_snap->file_count++] = make_file("/media/USB1/c.txt", "hash_c");
    // b.txt ausente en new_snap -> eliminado.

    int added, modified, deleted;
    assert(detect_usb_changes(old_snap, new_snap, &added, &modified, &deleted) == 0);
    assert(added == 1);    // c.txt
    assert(modified == 1); // a.txt (hash distinto)
    assert(deleted == 1);  // b.txt

    free_device_snapshot(old_snap);
    free_device_snapshot(new_snap);
}

static void test_generate_status_strings(void) {
    char buf[64];
    assert(generate_usb_status_string(0, FALSE, TRUE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "ESCANEANDO") == 0);
    assert(generate_usb_status_string(0, TRUE, FALSE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "SOSPECHOSO") == 0);
    assert(generate_usb_status_string(3, FALSE, FALSE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "CAMBIOS DETECTADOS") == 0);
    assert(generate_usb_status_string(0, FALSE, FALSE, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "LIMPIO") == 0);

    assert(generate_port_status_string(1, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "Abierto") == 0);
    assert(generate_port_status_string(0, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "Cerrado") == 0);
}

static void test_adapt_port_info_to_gui(void) {
    PortInfo port = {0};
    port.port = 443;
    port.is_open = 1;
    port.is_suspicious = 0;
    strcpy(port.service_name, "HTTPS");

    GUIPort gp;
    assert(adapt_port_info_to_gui(&port, &gp) == 0);
    assert(gp.port == 443);
    assert(strcmp(gp.service, "HTTPS") == 0);
    assert(strcmp(gp.status, "Abierto") == 0);
    assert(gp.is_suspicious == 0);
}

static void test_aggregate_port_statistics(void) {
    PortInfo ports[3] = {
        { .port = 22, .is_open = 1, .is_suspicious = 0 },
        { .port = 4444, .is_open = 1, .is_suspicious = 1 },
        { .port = 25, .is_open = 0, .is_suspicious = 0 },
    };
    int total_open, total_suspicious;
    assert(aggregate_port_statistics(ports, 3, &total_open, &total_suspicious) == 0);
    assert(total_open == 2);
    assert(total_suspicious == 1);
}

static void test_usb_snapshot_cache(void) {
    assert(init_usb_snapshot_cache() == 0);

    DeviceSnapshot *snap = make_snapshot("USB_TEST", 1);
    snap->files[snap->file_count++] = make_file("/media/USB_TEST/f.txt", "hash1");

    assert(store_usb_snapshot("USB_TEST", snap) == 0);
    DeviceSnapshot *cached = get_cached_usb_snapshot("USB_TEST");
    assert(cached == snap);
    assert(get_cached_usb_snapshot("NONEXISTENT") == NULL);

    cleanup_usb_snapshot_cache(); // Libera `snap` internamente vía free_device_snapshot.
}

static void test_format_timestamp_for_gui(void) {
    char buf[64];
    assert(format_timestamp_for_gui(0, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "Nunca") == 0);

    time_t now = time(NULL);
    assert(format_timestamp_for_gui(now - 30, buf, sizeof(buf)) == 0);
    assert(strstr(buf, "seg") != NULL);
}

static void test_filter_and_wrap(void) {
    char out[64];
    filter_emoji_and_special_chars("Hola \xF0\x9F\x91\x8B mundo", out, sizeof(out));
    assert(strstr(out, "Hola") != NULL);
    assert(strstr(out, "mundo") != NULL);

    char wrapped[128];
    wrap_text_for_pdf("una linea muy larga que debe partirse en pedazos mas chicos", wrapped, sizeof(wrapped), 10);
    assert(strchr(wrapped, '\n') != NULL);

    int lines = count_wrapped_lines("corta\nuna linea muy muy muy larga que no cabe", 10);
    assert(lines >= 3);
}

int main(void) {
    test_utf8_safe_truncate();
    test_adapt_process_info_to_gui();
    test_evaluate_usb_suspicion();
    test_detect_usb_changes();
    test_generate_status_strings();
    test_adapt_port_info_to_gui();
    test_aggregate_port_statistics();
    test_usb_snapshot_cache();
    test_format_timestamp_for_gui();
    test_filter_and_wrap();
    printf("test_gui_backend_adapters: OK\n");
    return 0;
}
