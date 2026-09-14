#include "../include/port_scanner.h"
#include <pthread.h>

// ============================================================================
// MAPEO DE SERVICIOS COMUNES (DATOS ESTÁTICOS)
// ============================================================================

/**
 * Tabla de servicios comunes y sus puertos asociados
 */
static const ServiceMapping common_services[] = {
    {21, "FTP", 1},
    {22, "SSH", 1},
    {23, "Telnet", 0},           // Sospechoso por inseguro
    {25, "SMTP", 1},
    {53, "DNS", 1},
    {80, "HTTP", 1},
    {110, "POP3", 1},
    {143, "IMAP", 1},
    {443, "HTTPS", 1},
    {993, "IMAPS", 1},
    {995, "POP3S", 1},
    {3389, "RDP", 0},            // Sospechoso si está expuesto
    {4444, "Metasploit", 0},     // Puerto común para backdoors
    {5900, "VNC", 0},            // Sospechoso si está expuesto
    {6667, "IRC", 0},            // Sospechoso para backdoors
    {8080, "HTTP-Alt", 1},       // Común para servidores web alternativos
    {31337, "Elite/Backdoor", 0}, // Puerto típico de backdoors
    {0, NULL, 0}                 // Terminador
};

// ============================================================================
// FUNCIONES DE DETECCIÓN DE SERVICIOS
// ============================================================================

/**
 * Obtiene el nombre del servicio asociado a un puerto
 *
 * @param port: Número del puerto
 * @param service_name: Buffer donde se almacenará el nombre del servicio
 * @param buffer_size: Tamaño del buffer
 * @return int: 1 si es un servicio común, 0 si es sospechoso o desconocido
 */
int get_port_service_name(int port, char *service_name, size_t buffer_size) {
    for (int i = 0; common_services[i].service != NULL; i++) {
        if (common_services[i].port == port) {
            snprintf(service_name, buffer_size, "%s", common_services[i].service);
            return common_services[i].is_common;
        }
    }
    
    // Puerto desconocido
    snprintf(service_name, buffer_size, "Unknown");
    return 0;
}

/**
 * Determina si un puerto es sospechoso basado en criterios de seguridad
 *
 * @param port: Número del puerto
 * @param service_name: Nombre del servicio asociado
 * @return int: 1 si es sospechoso, 0 si es normal
 */
int is_port_suspicious(int port, const char *service_name) {
    // Puertos conocidos como backdoors
    if (port == 31337 || port == 4444 || port == 6667) {
        return 1;
    }
    
    // Servicios inseguros
    if (strcmp(service_name, "Telnet") == 0 || 
        strcmp(service_name, "RDP") == 0 || 
        strcmp(service_name, "VNC") == 0) {
        return 1;
    }
    
    // Puertos altos sin justificación común
    if (port > 1024 && strcmp(service_name, "Unknown") == 0) {
        return 1;
    }
    
    return 0;
}

// ============================================================================
// FUNCIONES DE ESCANEO DE PUERTOS
// ============================================================================

/**
 * Intenta establecer conexión TCP a un puerto específico
 * 
 * @param port: Puerto a escanear
 * @return int: 1 si el puerto está abierto, 0 si está cerrado
 */
static int scan_single_port(int port) {
    int sock;
    struct sockaddr_in target;
    int result;
    
    // Crear socket TCP
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return 0;
    }
    
    // Configurar timeout para evitar bloqueos largos
    struct timeval timeout;
    timeout.tv_sec = 1;  // 1 segundo timeout
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
    
    // Configurar dirección de destino (localhost)
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // Intentar conexión
    result = connect(sock, (struct sockaddr *)&target, sizeof(target));
    
    close(sock);
    
    return (result == 0) ? 1 : 0;
}

// ============================================================================
// ESCANEO CONCURRENTE DE RANGOS DE PUERTOS
// ============================================================================

typedef struct {
    int start_port;
    int end_port;
    int next_port;             // próximo puerto a tomar; protegido por lock
    pthread_mutex_t lock;

    PortInfo *ports;            // un slot por puerto del rango
    int open_ports;              // protegido por lock
    int suspicious_ports;        // protegido por lock
    int ports_completed;         // protegido por lock

    ProgressCallback cb;
    void *user_data;
    volatile sig_atomic_t *cancel;
} PortScanJob;

static void scan_port_task(void *arg) {
    PortScanJob *job = (PortScanJob *)arg;

    for (;;) {
        if (job->cancel && *job->cancel) {
            return;
        }

        pthread_mutex_lock(&job->lock);
        if (job->next_port > job->end_port) {
            pthread_mutex_unlock(&job->lock);
            return;
        }
        int port = job->next_port++;
        pthread_mutex_unlock(&job->lock);

        int index = port - job->start_port;
        PortInfo *info = &job->ports[index];
        info->port = port;
        info->is_open = scan_single_port(port);
        info->is_suspicious = 0;
        info->service_name[0] = '\0';

        if (info->is_open) {
            get_port_service_name(port, info->service_name, sizeof(info->service_name));
            info->is_suspicious = is_port_suspicious(port, info->service_name);
        }

        pthread_mutex_lock(&job->lock);
        if (info->is_open) {
            job->open_ports++;
            if (info->is_suspicious) {
                job->suspicious_ports++;
            }
        }
        job->ports_completed++;
        int completed = job->ports_completed;
        pthread_mutex_unlock(&job->lock);

        if (job->cb) {
            ProgressUpdate update = {
                .phase = "Escaneando puertos",
                .current = completed,
                .total = job->end_port - job->start_port + 1
            };
            job->cb(&update, job->user_data);
        }
    }
}

int scan_ports_range(int start_port, int end_port, int num_threads,
                      ProgressCallback cb, void *user_data,
                      volatile sig_atomic_t *cancel, ScanResult *out) {
    if (!out || start_port < 1 || end_port > 65535 || start_port > end_port || num_threads < 1) {
        return -1;
    }

    int total_ports = end_port - start_port + 1;
    // calloc (no malloc): si la cancelación corta el escaneo antes de
    // tiempo, los slots de puertos nunca tomados quedan en cero
    // (port=0, is_open=0, is_suspicious=0, service_name="") en vez de
    // memoria indeterminada — seguro de iterar igual.
    PortInfo *ports = calloc(total_ports, sizeof(PortInfo));
    if (!ports) {
        return -1;
    }

    PortScanJob job;
    job.start_port = start_port;
    job.end_port = end_port;
    job.next_port = start_port;
    pthread_mutex_init(&job.lock, NULL);
    job.ports = ports;
    job.open_ports = 0;
    job.suspicious_ports = 0;
    job.ports_completed = 0;
    job.cb = cb;
    job.user_data = user_data;
    job.cancel = cancel;

    ThreadPool *pool = threadpool_create(num_threads);
    if (!pool) {
        pthread_mutex_destroy(&job.lock);
        free(ports);
        return -1;
    }

    // Cada tarea recorre la cola compartida de puertos hasta agotarla —
    // alcanza con una tarea por hilo, no una por puerto.
    int submitted = 0;
    for (int i = 0; i < num_threads; i++) {
        if (threadpool_submit(pool, scan_port_task, &job) == 0) {
            submitted++;
        }
    }
    if (submitted == 0) {
        // Ninguna tarea pudo encolarse (p.ej. fallo de reserva de memoria
        // dentro del pool): no hay quién escanee, así que es un fallo real,
        // no un "éxito" con cero puertos abiertos.
        threadpool_destroy(pool);
        pthread_mutex_destroy(&job.lock);
        free(ports);
        return -1;
    }

    threadpool_wait(pool);
    threadpool_destroy(pool);
    pthread_mutex_destroy(&job.lock);

    out->ports = ports;
    out->total_ports = total_ports;
    out->open_ports = job.open_ports;
    out->suspicious_ports = job.suspicious_ports;

    return 0;
}

