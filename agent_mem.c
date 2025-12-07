/*
 * agent_mem.c
 *
 * Agente de memoria para el práctico:
 * ./agent_mem <ip_recolector> <puerto> <ip_logica_agente>
 *
 * Lee /proc/meminfo periódicamente y envía:
 * MEM;<ip_logica_agente>;<mem_used_MB>;<MemFree_MB>;<SwapTotal_MB>;<SwapFree_MB>\n
 *
 * Compilar:
 * gcc -std=c11 -Wall -Wextra -o agent_mem agent_mem.c
 *
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

volatile sig_atomic_t keep_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

/* Estructura para guardar métricas leídas */
typedef struct {
    long mem_total_kb;
    long mem_available_kb;
    long mem_free_kb;
    long swap_total_kb;
    long swap_free_kb;
} meminfo_t;

/* Lee /proc/meminfo y extrae los campos requeridos.
 * Devuelve 0 si tuvo éxito, -1 en caso de error.
 */
int read_meminfo(meminfo_t *m) {
    if (!m) return -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        perror("fopen(/proc/meminfo)");
        return -1;
    }

    char line[256];
    /* inicializar con -1 para detectar ausencia */
    m->mem_total_kb = -1;
    m->mem_available_kb = -1;
    m->mem_free_kb = -1;
    m->swap_total_kb = -1;
    m->swap_free_kb = -1;

    while (fgets(line, sizeof(line), f)) {
        long val;
        if (sscanf(line, "MemTotal: %ld kB", &val) == 1) {
            m->mem_total_kb = val;
            continue;
        }
        if (sscanf(line, "MemAvailable: %ld kB", &val) == 1) {
            m->mem_available_kb = val;
            continue;
        }
        if (sscanf(line, "MemFree: %ld kB", &val) == 1) {
            m->mem_free_kb = val;
            continue;
        }
        if (sscanf(line, "SwapTotal: %ld kB", &val) == 1) {
            m->swap_total_kb = val;
            continue;
        }
        if (sscanf(line, "SwapFree: %ld kB", &val) == 1) {
            m->swap_free_kb = val;
            continue;
        }
    }

    fclose(f);

    /* Verificar que tenemos al menos los campos obligatorios */
    if (m->mem_total_kb < 0 || m->mem_available_kb < 0 || m->mem_free_kb < 0) {
        fprintf(stderr, "No se pudieron leer todos los campos requeridos en /proc/meminfo\n");
        return -1;
    }
    /* Swap puede ser 0 - aceptable */
    if (m->swap_total_kb < 0) m->swap_total_kb = 0;
    if (m->swap_free_kb < 0) m->swap_free_kb = 0;

    return 0;
}

/* Conecta TCP al recolector. Devuelve fd del socket o -1 en error.
 * ip_recolector: IP o hostname, puerto_str: puerto como cadena.
 */
int connect_to_collector(const char *ip_recolector, const char *puerto_str) {
    struct addrinfo hints, *res, *rp;
    int sfd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;    /* IPv4 o IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(ip_recolector, puerto_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) continue;

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            /* conectado */
            break;
        }

        close(sfd);
        sfd = -1;
    }

    freeaddrinfo(res);

    if (sfd == -1) {
        fprintf(stderr, "No se pudo conectar a %s:%s\n", ip_recolector, puerto_str);
        return -1;
    }

    return sfd;
}

/* Envia todo el buffer (sendall). Devuelve 0 si todo enviado, -1 en error. */
int send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(fd, buf + total, len - total, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        if (sent == 0) {
            /* conexión cerrada inesperadamente */
            return -1;
        }
        total += (size_t)sent;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <ip_recolector> <puerto> <ip_logica_agente>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *ip_recolector = argv[1];
    const char *puerto_str = argv[2];
    const char *ip_logica_agente = argv[3];

    /* Capturar SIGINT para terminar ordenadamente */
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    int sockfd = -1;

    /* Intentar conectar inicialmente (si falla, se reintentará en loop) */
    sockfd = connect_to_collector(ip_recolector, puerto_str);
    if (sockfd != -1) {
        fprintf(stderr, "Conectado a %s:%s\n", ip_recolector, puerto_str);
    } else {
        fprintf(stderr, "Intentando reconectar periódicamente...\n");
    }

    const int interval_sec = 2; /* intervalo de envío (2s) */

    while (keep_running) {
        meminfo_t m;
        if (read_meminfo(&m) != 0) {
            /* Error leyendo /proc/meminfo; esperar y reintentar */
            sleep(interval_sec);
            continue;
        }

        double mem_used_mb = (m.mem_total_kb - m.mem_available_kb) / 1024.0;
        double mem_free_mb = m.mem_free_kb / 1024.0;
        double swap_total_mb = m.swap_total_kb / 1024.0;
        double swap_free_mb = m.swap_free_kb / 1024.0;

        /* Formatear línea a enviar */
        char msg[256];
        int n = snprintf(msg, sizeof(msg), "MEM;%s;%.2f;%.2f;%.2f;%.2f\n",
                         ip_logica_agente,
                         mem_used_mb,
                         mem_free_mb,
                         swap_total_mb,
                         swap_free_mb);
        if (n < 0 || n >= (int)sizeof(msg)) {
            fprintf(stderr, "Error construyendo el mensaje\n");
            /* No intentamos enviar; esperar y continuar */
            sleep(interval_sec);
            continue;
        }

        if (sockfd == -1) {
            /* intentar reconectar */
            sockfd = connect_to_collector(ip_recolector, puerto_str);
            if (sockfd != -1) {
                fprintf(stderr, "Reconectado a %s:%s\n", ip_recolector, puerto_str);
            } else {
                /* Esperar e intentar luego */
                sleep(interval_sec);
                continue;
            }
        }

        /* Enviar la línea */
        if (send_all(sockfd, msg, (size_t)n) != 0) {
            fprintf(stderr, "Fallo al enviar. Cerrando socket y reintentando.\n");
            close(sockfd);
            sockfd = -1;
            /* En el siguiente ciclo se intentará reconectar */
        } else {
            /* Envío OK */
            /* Opcional: imprimir en stderr un log local */
            fprintf(stderr, "Enviado: %s", msg);
        }

        /* Esperar intervalo (permite salir con SIGINT) */
        for (int i = 0; i < interval_sec && keep_running; ++i) {
            sleep(1);
        }
    }

    if (sockfd != -1) close(sockfd);
    fprintf(stderr, "agent_mem terminado.\n");
    return EXIT_SUCCESS;
}
