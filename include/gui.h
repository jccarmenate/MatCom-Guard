#ifndef GUI_H
#define GUI_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    char device_name[128];
    char mount_point[256];
    char status[64];
    int files_changed;
    int total_files;
    gboolean is_suspicious;
    time_t last_scan;
} GUIUSBDevice;

typedef struct {
    pid_t pid;
    char name[256];
    float cpu_usage;
    float mem_usage;
    gboolean is_suspicious;
    gboolean is_whitelisted;  // Nuevo campo para whitelist
} GUIProcess;

typedef struct {
    int port;
    char service[128];
    char status[64];
    gboolean is_suspicious;
} GUIPort;

typedef void (*ScanUSBCallback)(void);
typedef void (*ScanProcessesCallback)(void);
typedef void (*ScanPortsCallback)(void);
typedef void (*ExportReportCallback)(const char *filename);

// Funciones principales de la GUI
void init_gui(int argc, char **argv);
void gui_add_log_entry(const char *module, const char *level, const char *message);
void gui_update_usb_device(GUIUSBDevice *device);
void gui_update_process(GUIProcess *process);
void gui_update_port(GUIPort *port);
void gui_update_statistics(int usb_devices, int processes, int open_ports);
void gui_update_system_status(const char *status, gboolean is_healthy);
void gui_set_scanning_status(gboolean is_scanning);
void gui_set_scan_callbacks(ScanUSBCallback usb_cb, ScanProcessesCallback proc_cb,
                           ScanPortsCallback port_cb, ExportReportCallback report_cb);

#endif