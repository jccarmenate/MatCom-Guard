#ifndef PORT_SCANNER_H
#define PORT_SCANNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>       // sig_atomic_t
#include "progress.h"
#include "threadpool.h"

// ============================================================================
// ESTRUCTURAS PÚBLICAS
// ============================================================================

/**
 * Estructura para información de puerto escaneado
 */
typedef struct {
    int port;                    // Número del puerto
    int is_open;                 // 1 si está abierto, 0 si cerrado
    char service_name[64];       // Nombre del servicio asociado
    int is_suspicious;           // 1 si es sospechoso, 0 si es normal
} PortInfo;

/**
 * Estructura para el resultado completo del escaneo
 */
typedef struct {
    PortInfo *ports;             // Array de puertos escaneados
    int total_ports;             // Total de puertos escaneados
    int open_ports;              // Cantidad de puertos abiertos
    int suspicious_ports;        // Cantidad de puertos sospechosos
} ScanResult;

/**
 * Estructura para mapear puertos con servicios conocidos
 */
typedef struct {
    int port;
    const char *service;
    int is_common;               // 1 si es común y esperado, 0 si no
} ServiceMapping;

// ============================================================================
// CLASIFICACIÓN DE PUERTOS (antes privada de port_scanner.c)
// ============================================================================

/**
 * Obtiene el nombre del servicio asociado a un puerto.
 * @return 1 si es un servicio común/esperado, 0 si es desconocido.
 */
int get_port_service_name(int port, char *service_name, size_t buffer_size);

/**
 * Determina si un puerto es sospechoso según los mismos criterios que usa
 * el escaneo de puertos internamente.
 */
int is_port_suspicious(int port, const char *service_name);

// ============================================================================
// FUNCIONES PÚBLICAS DE ESCANEO DE PUERTOS
// ============================================================================

/**
 * Escanea un rango de puertos usando `num_threads` hilos trabajadores.
 * `cb`/`user_data` son opcionales (pueden ser NULL) y se invocan a medida
 * que cada puerto termina de escanearse — pueden llegar desde cualquier
 * hilo trabajador. `cancel`, si no es NULL, permite pedir la interrupción
 * del escaneo desde otro hilo (se revisa entre puerto y puerto).
 * @return 0 si tuvo éxito, -1 si los parámetros son inválidos o falló la
 *         reserva de memoria/hilos.
 */
int scan_ports_range(int start_port, int end_port, int num_threads,
                      ProgressCallback cb, void *user_data,
                      volatile sig_atomic_t *cancel, ScanResult *out);

#endif // PORT_SCANNER_H

