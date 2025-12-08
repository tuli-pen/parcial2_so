/*
 * collector.c
 *
 * Servidor recolector para agentes CPU y MEM.
 *
 * ./collector <puerto>
 *
 * Acepta múltiples conexiones TCP, recibe líneas tipo:
 *  MEM;ip;memUsed;memFree;swapTotal;swapFree
 *  CPU;ip;cpuUsage;userPct;sysPct;idlePct
 *
 * Mantiene una tabla con la última info por IP y un hilo visualizador
 * que imprime cada 2 segundos.
 */

// Definimos esta macro para habilitar ciertas funciones POSIX (como sigaction)
// según el estándar POSIX 2008. Esto puede afectar qué funciones expone la libc.
#define _POSIX_C_SOURCE 200809L

// Includes estándar de C
#include <stdio.h>      // printf, fprintf, etc.
#include <stdlib.h>     // malloc, free, atof, exit...
#include <string.h>     // memset, strcmp, strncpy, strtok...
#include <unistd.h>     // close, sleep, read, write...
#include <errno.h>      // errno y mensajes de error
#include <signal.h>     // manejo de señales (sigaction, SIGINT)
#include <pthread.h>    // hilos POSIX (pthread_t, pthread_create, mutex...)
#include <ctype.h>      // funciones sobre caracteres (aquí casi no se usan)

// Includes para sockets
#include <sys/types.h>  // tipos como socklen_t
#include <sys/socket.h> // socket, bind, listen, accept, recv...
#include <netdb.h>      // getaddrinfo, struct addrinfo
#include <arpa/inet.h>  // funciones para direcciones IP (inet_ntoa, etc.)

// Máximo número de hosts (IPs) que vamos a almacenar simultáneamente
#define MAX_HOSTS 64

// Tamaño máximo de línea de texto que esperamos recibir por el socket
#define MAX_LINE 512

// Variable global que indica si el programa debe seguir corriendo.
// Se marca como volatile y de tipo sig_atomic_t para que sea segura
// al modificarla desde un manejador de señal.
volatile sig_atomic_t keep_running = 1;

// Estructura que almacena la información de un host (una IP).
typedef struct {
    char ip[32];                 // IP en formato texto (ej: "192.168.0.10")
    float cpu_usage;             // Porcentaje de uso total de CPU
    float cpu_user;              // Porcentaje de tiempo de CPU en modo usuario
    float cpu_sys;               // Porcentaje de tiempo de CPU en modo sistema
    float cpu_idle;              // Porcentaje de tiempo de CPU inactiva
    float mem_used;              // Memoria usada (en MB)
    float mem_free;              // Memoria libre (en MB)
    float swap_t;                // Swap total (en MB)
    float swap_f;                // Swap libre (en MB)
    int has_cpu;                 // Bandera: 1 si ya hay datos de CPU válidos
    int has_mem;                 // Bandera: 1 si ya hay datos de memoria válidos
} host_info_t;

// Tabla global que guarda la información de hasta MAX_HOSTS máquinas.
host_info_t hosts[MAX_HOSTS];

// Mutex global para proteger el acceso concurrente a la tabla 'hosts'.
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/**************** SIGNAL HANDLER ****************/
// Función que se ejecuta cuando llega una señal SIGINT (por ejemplo, Ctrl+C).
void handle_sigint(int sig) {
    (void)sig;        // Evita el warning por parámetro sin usar
    keep_running = 0; // Cambia la variable global para indicar que debemos terminar
}

/********* FIND OR CREATE HOST ENTRY *********/
// Busca una entrada de host por IP, y si no existe, crea una nueva
// en el primer espacio libre de la tabla.
host_info_t *get_host(const char *ip) {
    // Primer bucle: buscamos si la IP ya existe en la tabla
    for (int i = 0; i < MAX_HOSTS; i++) {
        // Si la IP de esa posición coincide con la IP buscada
        if (strcmp(hosts[i].ip, ip) == 0)
            return &hosts[i]; // Devolvemos un puntero a esa entrada
    }
    // Segundo bucle: si no estaba, buscamos una entrada vacía (ip == "")
    for (int i = 0; i < MAX_HOSTS; i++) {
        // Si el primer carácter es '\0', significa que está libre
        if (hosts[i].ip[0] == '\0') {
            // Copiamos la IP en la estructura (con límite de tamaño)
            strncpy(hosts[i].ip, ip, sizeof(hosts[i].ip));
            // IMPORTANTE: al ser global, el resto de campos estaban en 0 por defecto
            return &hosts[i]; // Devolvemos la nueva entrada
        }
    }
    // Si llegamos aquí, no había espacio (tabla llena)
    return NULL;
}

/************* PARSE CPU MESSAGE *************/
// Función que parsea un mensaje de tipo CPU y actualiza la tabla de hosts.
// Formato esperado: "CPU;ip;usage;user;sys;idle"
void parse_cpu(char *msg) {
    // Primer token: "CPU" (no lo usamos directamente)
    char *tok = strtok(msg, ";");
    // Segundo token: IP
    tok = strtok(NULL, ";");
    if (!tok) return;    // Si no hay token, el mensaje está mal formado
    char *ip = tok;      // Guardamos el puntero a la cadena IP

    // Tercer token: uso total de CPU
    tok = strtok(NULL, ";");
    if (!tok) return;
    float usage = atof(tok); // Convertimos a float (porcentaje)

    // Cuarto, quinto y sexto token: user, sys, idle
    float user = atof(strtok(NULL, ";")); // Porcentaje CPU modo usuario
    float sys  = atof(strtok(NULL, ";")); // Porcentaje CPU modo sistema
    float idle = atof(strtok(NULL, ";")); // Porcentaje CPU inactiva

    // Proteger la tabla global con el mutex mientras actualizamos datos
    pthread_mutex_lock(&lock);
    host_info_t *h = get_host(ip); // Obtenemos (o creamos) la entrada de ese host
    if (h) {
        // Actualizamos los campos de CPU
        h->cpu_usage = usage;
        h->cpu_user  = user;
        h->cpu_sys   = sys;
        h->cpu_idle  = idle;
        h->has_cpu   = 1; // Marcamos que ya tenemos datos de CPU válidos
    }
    pthread_mutex_unlock(&lock); // Liberamos el mutex
}

/************* PARSE MEM MESSAGE *************/
// Función que parsea un mensaje de tipo MEM y actualiza la tabla de hosts.
// Formato esperado: "MEM;ip;used;free;swapT;swapF"
void parse_mem(char *msg) {
    // Primer token: "MEM"
    char *tok = strtok(msg, ";");
    // Segundo token: IP
    tok = strtok(NULL, ";");
    if (!tok) return;   // Si no existe, mensaje inválido
    char *ip = tok;     // Guardamos IP

    // Siguientes tokens: used, free, swapTotal, swapFree
    float used = atof(strtok(NULL, ";")); // Memoria usada
    float free = atof(strtok(NULL, ";")); // Memoria libre
    float swt  = atof(strtok(NULL, ";")); // Swap total
    float swf  = atof(strtok(NULL, ";")); // Swap libre

    // Sección crítica para actualizar la tabla global
    pthread_mutex_lock(&lock);
    host_info_t *h = get_host(ip); // Buscamos/creamos entrada de host
    if (h) {
        // Actualizamos los campos de memoria
        h->mem_used = used;
        h->mem_free = free;
        h->swap_t   = swt;
        h->swap_f   = swf;
        h->has_mem  = 1; // Marcamos que ya tenemos datos de memoria válidos
    }
    pthread_mutex_unlock(&lock); // Liberamos el mutex
}

/*********** THREAD: HANDLE CLIENT ***********/
// Función que se ejecuta en un hilo por cada cliente conectado.
// Se encarga de recibir datos por el socket y procesar líneas CPU/MEM.
void *client_thread(void *arg) {
    // arg es un puntero a int que contiene el descriptor de socket del cliente.
    int fd = *(int *)arg;
    // Liberamos la memoria que se reservó en main para este descriptor.
    free(arg);

    // Buffer temporal para almacenar los datos que llegan por el socket.
    char buf[MAX_LINE];

    // Bucle principal del hilo mientras el servidor siga activo.
    while (keep_running) {
        // recv lee datos del socket 'fd' y los guarda en 'buf'.
        // Devuelve el número de bytes leídos, o <= 0 si hay error o se cierra.
        ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
        if (n <= 0)
            break; // Si n <= 0, rompemos el bucle (cliente cerró o error)

        // Añadimos terminador de cadena para poder tratar 'buf' como string.
        buf[n] = '\0';

        // Un mismo recv puede traer varias líneas separadas por '\n'.
        // Usamos strtok para procesar cada línea individualmente.
        char *line = strtok(buf, "\n");
        while (line) {
            // Hacemos una copia de la línea porque dentro de parse_cpu/parse_mem
            // también se usa strtok, que modifica la cadena.
            char copy[MAX_LINE];
            strncpy(copy, line, sizeof(copy));
            // Nos aseguramos de que copy quede terminada en '\0'
            copy[sizeof(copy) - 1] = '\0';

            // Si la línea empieza por "CPU;", la tratamos como mensaje de CPU.
            if (strncmp(copy, "CPU;", 4) == 0)
                parse_cpu(copy);
            // Si empieza por "MEM;", la tratamos como mensaje de memoria.
            else if (strncmp(copy, "MEM;", 4) == 0)
                parse_mem(copy);

            // Pasamos a la siguiente línea dentro del mismo buffer.
            line = strtok(NULL, "\n");
        }
    }

    // Al salir del bucle, cerramos el socket del cliente.
    close(fd);
    // Terminamos el hilo.
    return NULL;
}

/******** THREAD: VISUALIZER ********/
// Hilo que se encarga de imprimir periódicamente el estado de todos los hosts.
void *visualizer_thread(void *arg) {
    (void)arg; // No usamos el argumento, se castea para evitar warning.

    // Mientras el servidor siga activo.
    while (keep_running) {
        // Dormimos 2 segundos entre cada refresco de pantalla.
        sleep(2);
        
        // Secuencia de escape ANSI para limpiar la pantalla y mover el cursor
        // a la esquina superior izquierda (simula un "pantallazo" tipo top).
        printf("\033[2J\033[H");
        // Imprimimos encabezado de la tabla.
        printf("IP           CPU    usr   sys   idle   MemUsed  MemFree\n");
        printf("----------------------------------------------------------\n");

        // Bloqueamos el mutex mientras recorremos la tabla de hosts.
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_HOSTS; i++) {
            // Si la IP está vacía, significa que esta entrada no se usa.
            if (hosts[i].ip[0] == '\0') continue;

            host_info_t *h = &hosts[i];
            // Imprimimos la IP alineada a la izquierda en un ancho de 12 caracteres.
            printf("%-12s ", h->ip);

            // Si tenemos datos de CPU, los mostramos.
            if (h->has_cpu)
                printf("%5.1f %5.1f %5.1f %6.1f   ",
                       h->cpu_usage, h->cpu_user, h->cpu_sys, h->cpu_idle);
            else
                // Si no hay datos de CPU, mostramos "--" para indicar ausencia.
                printf(" --    --    --    --     ");

            // Si tenemos datos de memoria, los mostramos.
            if (h->has_mem)
                printf("%7.1f %7.1f", h->mem_used, h->mem_free);
            else
                // Si no hay datos de memoria, mostramos "--".
                printf("   --       --");

            // Fin de la línea para ese host.
            printf("\n");
        }
        // Liberamos el mutex después de leer toda la tabla.
        pthread_mutex_unlock(&lock);
    }

    // Cuando keep_running sea 0, salimos del bucle y terminamos el hilo.
    return NULL;
}

/************ MAIN ************/
// Función principal del programa: configura el servidor y acepta conexiones.
int main(int argc, char *argv[]) {
    // Comprobamos que se haya pasado exactamente un argumento (el puerto).
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return 1; // Salimos con código de error.
    }

    // Configuración del manejo de la señal SIGINT (Ctrl+C).
    struct sigaction sa;
    sa.sa_handler = handle_sigint; // Función que se llamará al recibir SIGINT.
    sigemptyset(&sa.sa_mask);      // No se bloquean señales adicionales.
    sa.sa_flags = 0;               // Sin flags especiales.
    sigaction(SIGINT, &sa, NULL);  // Registramos el manejador.

    // Guardamos el puerto pasado por la línea de comandos.
    const char *port = argv[1];

    int sfd;                // Descriptor de socket del servidor (socket de escucha).
    struct addrinfo hints;  // Estructura para indicar preferencias a getaddrinfo.
    struct addrinfo *res;   // Resultado de getaddrinfo (dirección(es) disponibles).

    // Inicializamos la estructura hints a 0.
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // Queremos IPv4.
    hints.ai_socktype = SOCK_STREAM;  // Tipo de socket: TCP.
    hints.ai_flags = AI_PASSIVE;      // Para indicar que vamos a hacer bind (servidor).

    // getaddrinfo resuelve la dirección local (servidor) en base a hints.
    // NULL indica "todas las interfaces" (0.0.0.0).
    getaddrinfo(NULL, port, &hints, &res);

    // Creamos el socket servidor usando los parámetros devueltos por getaddrinfo.
    sfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    // Configuramos el socket para permitir reusar la dirección rápidamente
    // (evita el error "Address already in use" al reiniciar el servidor).
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Asociamos el socket a la dirección IP y puerto obtenidos.
    bind(sfd, res->ai_addr, res->ai_addrlen);
    // Ponemos el socket en modo escucha, con una cola de hasta 16 conexiones.
    listen(sfd, 16);

    // Ya no necesitamos la estructura de direcciones, la liberamos.
    freeaddrinfo(res);

    // Creamos el hilo visualizador que mostrará la tabla cada 2 segundos.
    pthread_t viz;
    pthread_create(&viz, NULL, visualizer_thread, NULL);

    // Mensaje informativo para el usuario.
    printf("Collector escuchando en puerto %s\n", port);

    // Bucle principal del servidor: aceptar nuevas conexiones mientras siga activo.
    while (keep_running) {
        struct sockaddr_in cli;     // Estructura para información del cliente.
        socklen_t clilen = sizeof(cli); // Tamaño de la estructura cli.

        // Reservamos memoria dinámica para guardar el descriptor del cliente.
        // Esto se pasa al hilo y luego el hilo lo libera.
        int *cfd = malloc(sizeof(int));
        // accept bloquea hasta que llegue una nueva conexión.
        *cfd = accept(sfd, (struct sockaddr *)&cli, &clilen);
        // Si hubo error en accept, liberamos y seguimos con la siguiente iteración.
        if (*cfd < 0) { free(cfd); continue; }

        // Creamos un hilo nuevo para manejar a este cliente.
        pthread_t th;
        pthread_create(&th, NULL, client_thread, cfd);
        // Detach para que el hilo se limpie solo al terminar, sin necesidad de join.
        pthread_detach(th);
    }

    // Cuando keep_running sea 0, salimos del bucle, cerramos el socket de escucha.
    close(sfd);
    // Terminamos el programa correctamente.
    return 0;
}
