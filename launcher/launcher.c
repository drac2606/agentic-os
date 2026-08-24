#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "../include/protocolo.h"

static ProcesoHijo g_procesos[MAX_VENTANAS];

static volatile sig_atomic_t g_cantidad_procesos = 0;
static volatile sig_atomic_t g_terminados = 0;

/*
 * El Launcher solamente administra procesos locales.
 * La comunicación con IALearner la realizan los x11_client.
 */

static void manejar_sigchld(int sig) {
    (void)sig;

    int estado;
    pid_t pid;

    while ((pid = waitpid(
                -1,
                &estado,
                WNOHANG
            )) > 0) {

        for (int i = 0;
             i < g_cantidad_procesos;
             i++) {

            if (g_procesos[i].pid == pid &&
                g_procesos[i].estado == PROC_ACTIVO) {

                g_procesos[i].estado =
                    PROC_TERMINADO;

                g_procesos[i].codigo_salida =
                    WIFEXITED(estado)
                    ? WEXITSTATUS(estado)
                    : -1;

                g_terminados++;

                break;
            }
        }
    }
}

static pid_t lanzar_cliente(
    int id_ventana,
    const char *host,
    int puerto
) {

    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {

        char str_id[16];
        char str_puerto[16];

        snprintf(
            str_id,
            sizeof(str_id),
            "%d",
            id_ventana
        );

        snprintf(
            str_puerto,
            sizeof(str_puerto),
            "%d",
            puerto
        );

        char *args[] = {
            (char *)"./x11_client/x11_client",
            (char *)host,
            str_puerto,
            str_id,
            NULL
        };

        execv(
            "./x11_client/x11_client",
            args
        );

        fprintf(
            stderr,
            "[Launcher] Error execv: %s\n",
            strerror(errno)
        );

        _exit(EXIT_FAILURE);
    }

    return pid;
}

static void cerrar_todos_los_procesos(void) {

    for (int i = 0;
         i < g_cantidad_procesos;
         i++) {

        if (g_procesos[i].estado ==
            PROC_ACTIVO) {

            kill(
                g_procesos[i].pid,
                SIGTERM
            );
        }
    }
}

static int convertir_entero(
    const char *texto,
    long minimo,
    long maximo,
    long *resultado
) {

    char *fin = NULL;

    errno = 0;

    long valor =
        strtol(
            texto,
            &fin,
            10
        );

    if (errno != 0 ||
        fin == texto ||
        *fin != '\0' ||
        valor < minimo ||
        valor > maximo) {

        return -1;
    }

    *resultado = valor;

    return 0;
}

int main(
    int argc,
    char *argv[]
) {

    if (argc < 2 || argc > 4) {

        fprintf(
            stderr,
            "Uso: %s <n_ventanas> "
            "[host_ialearner] [puerto]\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    long n_ventanas_long;

    if (convertir_entero(
            argv[1],
            1,
            MAX_VENTANAS,
            &n_ventanas_long
        ) < 0) {

        fprintf(
            stderr,
            "Error: n_ventanas debe ser "
            "un entero entre 1 y %d.\n",
            MAX_VENTANAS
        );

        return EXIT_FAILURE;
    }

    const char *host =
        (argc >= 3)
        ? argv[2]
        : HOST_DEFECTO;

    long puerto_long =
        PUERTO_DEFECTO;

    if (argc == 4 &&
        convertir_entero(
            argv[3],
            1,
            65535,
            &puerto_long
        ) < 0) {

        fprintf(
            stderr,
            "Error: puerto invalido.\n"
        );

        return EXIT_FAILURE;
    }

    int n_ventanas =
        (int)n_ventanas_long;

    int puerto =
        (int)puerto_long;

    if (access(
            "./x11_client/x11_client",
            X_OK
        ) != 0) {

        fprintf(
            stderr,
            "[Launcher] Ejecutable de ventana "
            "no encontrado. Compilalo primero.\n"
        );

        return EXIT_FAILURE;
    }

    struct sigaction sa;

    memset(
        &sa,
        0,
        sizeof(sa)
    );

    sa.sa_handler =
        manejar_sigchld;

    sigemptyset(
        &sa.sa_mask
    );

    sa.sa_flags =
        SA_RESTART |
        SA_NOCLDSTOP;

    if (sigaction(
            SIGCHLD,
            &sa,
            NULL
        ) < 0) {

        perror(
            "sigaction(SIGCHLD)"
        );

        return EXIT_FAILURE;
    }

    signal(
        SIGPIPE,
        SIG_IGN
    );

    memset(
        g_procesos,
        0,
        sizeof(g_procesos)
    );

    for (int i = 0;
         i < n_ventanas;
         i++) {

        int id = i + 1;

        pid_t pid =
            lanzar_cliente(
                id,
                host,
                puerto
            );

        if (pid < 0) {

            fprintf(
                stderr,
                "[Launcher] No se pudo "
                "lanzar la ventana %d.\n",
                id
            );

            continue;
        }

        g_procesos[
            g_cantidad_procesos
        ].pid = pid;

        g_procesos[
            g_cantidad_procesos
        ].id_ventana = id;

        g_procesos[
            g_cantidad_procesos
        ].estado = PROC_ACTIVO;

        g_procesos[
            g_cantidad_procesos
        ].codigo_salida = -1;

        g_cantidad_procesos++;

        printf(
            "[Launcher] Ventana %d "
            "lanzada (PID %d)\n",
            id,
            pid
        );

        usleep(100000);
    }

    if (g_cantidad_procesos == 0) {

        fprintf(
            stderr,
            "[Launcher] No se pudo "
            "lanzar ninguna ventana.\n"
        );

        return EXIT_FAILURE;
    }

    char entrada[64];

    while (
        g_cantidad_procesos > 0 &&
        g_terminados < g_cantidad_procesos
    ) {

        printf(
            "\n==================================\n"
        );

        printf(
            " 1. Ver estado | "
            "2. Cerrar todo | "
            "3. Salir\n"
        );

        printf(
            "==================================\n"
            "Opcion: "
        );

        fflush(stdout);

        if (fgets(
                entrada,
                sizeof(entrada),
                stdin
            ) == NULL) {

            break;
        }

        char *fin = NULL;

        errno = 0;

        long opcion =
            strtol(
                entrada,
                &fin,
                10
            );

        if (errno != 0 ||
            fin == entrada) {

            printf(
                "[Launcher] Opcion invalida.\n"
            );

            continue;
        }

        if (opcion == 1) {

            printf(
                "\n--- ESTADO DE PROCESOS ---\n"
            );

            for (int i = 0;
                 i < g_cantidad_procesos;
                 i++) {

                printf(
                    "Ventana %d (PID %d) - %s\n",
                    g_procesos[i].id_ventana,
                    g_procesos[i].pid,
                    g_procesos[i].estado ==
                        PROC_ACTIVO
                        ? "ACTIVA"
                        : "TERMINADA"
                );
            }

        } else if (opcion == 2) {

            cerrar_todos_los_procesos();

        } else if (opcion == 3) {

            break;

        } else {

            printf(
                "[Launcher] Opcion invalida.\n"
            );
        }
    }

    cerrar_todos_los_procesos();

    /*
     * Esperar a todos los hijos para garantizar
     * que no queden procesos zombie.
     */
    while (
        g_terminados <
        g_cantidad_procesos
    ) {

        pause();
    }

    printf(
        "[Launcher] Terminando...\n"
    );

    return EXIT_SUCCESS;
}
