/*
 * agent_cpu.c
 *
 * Agente de CPU para el práctico:
 * ./agent_cpu <ip_recolector> <puerto> <ip_logica_agente>
 *
 * Lee /proc/stat periódicamente y envía:
 * CPU;<ip_logica_agente>;<cpu_usage>;<user_pct>;<system_pct>;<idle_pct>\n
 *
 * Compilar:
 * gcc -std=c11 -Wall -Wextra -o agent_cpu agent_cpu.c
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

/* ------------------- ESTRUCTURA CPU -------------------- */

typedef struct {
    unsigned long user;
    unsigned long nice;
    unsigned long system;
    unsigned long idle;
} cpu_stats_t;

/* ----------- LECTURA /proc/stat ---------------- */

int read_cpu_info(cpu_stats_t *cpu) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) {
        perror("fopen");
        return -1;
    }

    char label[5];
    if (fscanf(f, "%4s %lu %lu %lu %lu",
               label,
               &cpu->user,
               &cpu->nice,
               &cpu->system,
               &cpu->idle) != 5) {
        fprintf(stderr, "No se pudo leer la línea de cpu\n");
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* ------------- CALCULAR DELTAS Y PORCENTAJES -------------- */

void calcular_deltas(
    const cpu_stats_t *prev,
    const cpu_stats_t *curr,
    double *cpu_usage,
    double *user_pct,
    double *system_pct,
    double *idle_pct
) {
    unsigned long delta_user   = curr->user   - prev->user;
    unsigned long delta_nice   = curr->nice   - prev->nice;
    unsigned long delta_system = curr->system - prev->system;
    unsigned long delta_idle   = curr->idle   - prev->idle;

    unsigned long total = delta_user + delta_nice + delta_system + delta_idle;

    if (total == 0) total = 1;

    *cpu_usage  = 100.0 * (total - delta_idle) / total;
    *user_pct   = 100.0 * delta_user   / total;
    *system_pct = 100.0 * delta_system / total;
    *idle_pct   = 100.0 * delta_idle   / total;
}

/* ------------ SOCKETS (IGUAL QUE agent_mem) ------------- */

int connect_to_collector(const char *ip_recolector, const char *puerto_str) {
    struct addrinfo hints, *res, *rp;
    int sfd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(ip_recolector, puerto_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) continue;

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) break;

        close(sfd);
        sfd = -1;
    }

    freeaddrinfo(res);

    if (sfd == -1) {
        fprintf(stderr, "No se pudo conectar a %s:%s\n",
                ip_recolector, puerto_str);
        return -1;
    }

    return sfd;
}

int send_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(fd, buf + total, len - total, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        if (sent == 0) return -1;

        total += sent;
    }
    return 0;
}

/* ---------------------- MAIN ------------------------ */

int main(int argc, char *argv[]) {

    if (argc != 4) {
        fprintf(stderr,
            "Uso: %s <ip_recolector> <puerto> <ip_logica_agente>\n",
            argv[0]);
        return EXIT_FAILURE;
    }

    const char *ip_recolector = argv[1];
    const char *puerto        = argv[2];
    const char *ip_logica     = argv[3];

    /* SIGINT */
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    int sockfd = connect_to_collector(ip_recolector, puerto);
    if (sockfd != -1)
        fprintf(stderr, "Conectado a %s:%s\n", ip_recolector, puerto);
    else
        fprintf(stderr, "Intentando reconectar...\n");

    const int interval_sec = 2;

    while (keep_running) {

        cpu_stats_t prev, curr;

        /* Leer primera muestra */
        if (read_cpu_info(&prev) != 0) {
            sleep(interval_sec);
            continue;
        }

        sleep(1); // diferencia entre muestras

        /* Leer segunda muestra */
        if (read_cpu_info(&curr) != 0) {
            sleep(interval_sec);
            continue;
        }

        /* Calcular porcentajes */
        double cpu_usage, user_pct, system_pct, idle_pct;
        calcular_deltas(&prev, &curr,
                        &cpu_usage, &user_pct, &system_pct, &idle_pct);

        /* Formar mensaje */
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
            "CPU;%s;%.2f;%.2f;%.2f;%.2f\n",
            ip_logica, cpu_usage, user_pct, system_pct, idle_pct);

        if (n < 0 || n >= (int)sizeof(msg)) {
            fprintf(stderr, "Error generando mensaje\n");
            sleep(interval_sec);
            continue;
        }

        if (sockfd == -1) {
            sockfd = connect_to_collector(ip_recolector, puerto);
            if (sockfd != -1)
                fprintf(stderr, "Reconectado.\n");
            else {
                sleep(interval_sec);
                continue;
            }
        }

        if (send_all(sockfd, msg, n) != 0) {
            fprintf(stderr, "Error enviando, cerrando socket.\n");
            close(sockfd);
            sockfd = -1;
        } else {
            fprintf(stderr, "Enviado: %s", msg);
        }

        for (int i = 0; i < interval_sec && keep_running; i++)
            sleep(1);
    }

    if (sockfd != -1) close(sockfd);

    fprintf(stderr, "agent_cpu terminado.\n");
    return EXIT_SUCCESS;
}
