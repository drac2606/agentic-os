#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

#include "../include/protocolo.h"

static ProcesoHijo g_procesos[MAX_VENTANAS];
static volatile sig_atomic_t g_cantidad_procesos = 0;
static volatile sig_atomic_t g_terminados = 0;

static const char *g_host = NULL;
static int g_puerto = 0;

/* ─── MANEJADOR DE SEÑALES PARA EVITAR ZOMBIES ─── */
static void manejar_sigchld(int sig) {
    (void)sig;
    int estado;
    pid_t pid;
    while ((pid = waitpid(-1, &estado, WNOHANG)) > 0) {
        for (int i = 0; i < g_cantidad_procesos; i++) {
            if (g_procesos[i].pid == pid) {
                g_procesos[i].estado = PROC_TERMINADO;
                g_procesos[i].codigo_salida = WIFEXITED(estado) ? WEXITSTATUS(estado) : -1;
                g_terminados++;
                break;
            }
        }
    }
}

/* ─── NOTIFICAR TOTAL AL SERVIDOR ─── */
static int notificar_total(const char *host, int puerto, int n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in direccion;
    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family = AF_INET;
    direccion.sin_port = htons((uint16_t)puerto);
    inet_pton(AF_INET, host, &direccion.sin_addr);

    if (connect(fd, (struct sockaddr *)&direccion, sizeof(direccion)) < 0) {
        close(fd);
        return -1;
    }

    char mensaje[TAM_MAX_MSG];
    snprintf(mensaje, sizeof(mensaje), "%s %d\n", PROTO_TOTAL, n);
    send(fd, mensaje, strlen(mensaje), MSG_NOSIGNAL);
    close(fd);
    return 0;
}

/* ─── LANZAR VENTANA CON FORK Y EXECV ─── */
static pid_t lanzar_cliente(int id_ventana, const char *host, int puerto) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) { /* Proceso Hijo */
        char str_id[16], str_puerto[16];
        snprintf(str_id, sizeof(str_id), "%d", id_ventana);
        snprintf(str_puerto, sizeof(str_puerto), "%d", puerto);

        char *args[] = {
            (char *)"./x11_client/x11_client",
            (char *)host,
            str_puerto,
            str_id,
            NULL
        };
        execv("./x11_client/x11_client", args);
        fprintf(stderr, "[Launcher] Error execv: %s\n", strerror(errno));
        _exit(EXIT_FAILURE);
    }
    return pid; /* Proceso Padre retorna el PID */
}

static void cerrar_todos_los_procesos(void) {
    for (int i = 0; i < g_cantidad_procesos; i++) {
        if (g_procesos[i].estado == PROC_ACTIVO) {
            kill(g_procesos[i].pid, SIGTERM);
        }
    }
}

/* ─── FUNCIÓN PRINCIPAL MAIN ─── */
int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Uso: %s <n_ventanas> [host_ialearner] [puerto]\n", argv[0]);
        return EXIT_FAILURE;
    }

    long n_ventanas = strtol(argv[1], NULL, 10);
    g_host = (argc >= 3) ? argv[2] : HOST_DEFECTO;
    g_puerto = (argc == 4) ? atoi(argv[3]) : PUERTO_DEFECTO;

    if (access("./x11_client/x11_client", X_OK) != 0) {
        fprintf(stderr, "[Launcher] Ejecutable de ventana no encontrado. Compilalo primero.\n");
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejar_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (notificar_total(g_host, g_puerto, (int)n_ventanas) < 0) {
        fprintf(stderr, "[Launcher] ADVERTENCIA: sin conexion al servidor.\n");
    }

    memset(g_procesos, 0, sizeof(g_procesos));
    for (int i = 0; i < n_ventanas; i++) {
        int id = i + 1;
        pid_t pid = lanzar_cliente(id, g_host, g_puerto);
        if (pid > 0) {
            g_procesos[i].pid = pid;
            g_procesos[i].id_ventana = id;
            g_procesos[i].estado = PROC_ACTIVO;
            g_cantidad_procesos++;
            printf("[Launcher] Ventana %d lanzada (PID %d)\n", id, pid);
        }
        usleep(100000); // 100ms
    }

    /* Menú simplificado */
    char entrada[64];
    while (g_cantidad_procesos > 0 && g_terminados < g_cantidad_procesos) {
        printf("\n==================================\n");
        printf(" 1. Ver estado | 2. Cerrar todo | 3. Salir\n");
        printf("==================================\nOpcion: ");
        fflush(stdout);
        
        if (fgets(entrada, sizeof(entrada), stdin) == NULL) break;
        int opcion = atoi(entrada);

        if (opcion == 1) {
            printf("\n--- ESTADO DE PROCESOS ---\n");
            for (int i = 0; i < g_cantidad_procesos; i++) {
                printf("Ventana %d (PID %d) - %s\n", g_procesos[i].id_ventana, g_procesos[i].pid,
                       g_procesos[i].estado == PROC_ACTIVO ? "ACTIVA" : "TERMINADA");
            }
        } else if (opcion == 2) {
            cerrar_todos_los_procesos();
        } else if (opcion == 3) {
            break;
        }
    }

    cerrar_todos_los_procesos();
    printf("[Launcher] Terminando...\n");
    return EXIT_SUCCESS;
}
