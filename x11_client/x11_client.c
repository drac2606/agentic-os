#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

/* Incluimos el protocolo que acabamos de crear */
#include "../include/protocolo.h"

/* Conecta al IALearner. Devuelve el fd del socket, o -1 si falla. */
static int conectar_ia_learner(const char *host, int puerto) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in direccion;
    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family = AF_INET;
    direccion.sin_port   = htons((uint16_t)puerto);

    if (inet_pton(AF_INET, host, &direccion.sin_addr) <= 0) {
        fprintf(stderr, "Direccion invalida: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&direccion, sizeof(direccion)) < 0) {
        fprintf(stderr, "No se pudo conectar a IALearner (%s:%d): %s\n",
                host, puerto, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* Envía un mensaje del protocolo, agregando '\n' al final. */
static void enviar_mensaje(int fd_socket, const char *mensaje) {
    if (fd_socket < 0) return;

    char buffer[TAM_MAX_MSG];
    snprintf(buffer, sizeof(buffer), "%s\n", mensaje);

    ssize_t enviados = send(fd_socket, buffer, strlen(buffer), MSG_NOSIGNAL);
    if (enviados < 0) {
        fprintf(stderr, "Error enviando mensaje: %s\n", strerror(errno));
    }
}

int main(int argc, char *argv[]) {
    /* Validación defensiva de argumentos: host, puerto, id de ventana */
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <host> <puerto> <id_ventana>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *host = argv[1];
    char *fin_parse;
    
    long puerto = strtol(argv[2], &fin_parse, 10);
    if (*fin_parse != '\0' || puerto <= 0 || puerto > 65535) {
        fprintf(stderr, "Puerto invalido: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    long id_ventana = strtol(argv[3], &fin_parse, 10);
    if (*fin_parse != '\0' || id_ventana <= 0 || id_ventana > MAX_VENTANAS) {
        fprintf(stderr, "id_ventana invalido: %s\n", argv[3]);
        return EXIT_FAILURE;
    }

    /* Ignoramos SIGPIPE para que no se caiga la ventana si el servidor muere */
    signal(SIGPIPE, SIG_IGN);

    int fd_socket = conectar_ia_learner(host, (int)puerto);
    if (fd_socket < 0) {
        fprintf(stderr, "Continuando sin conexion a IALearner (modo local)\n");
    }

    /* Identificarse ante el servidor */
    char msg_id[TAM_MAX_MSG];
    snprintf(msg_id, sizeof(msg_id), "%s %ld", PROTO_ID, id_ventana);
    enviar_mensaje(fd_socket, msg_id);

    /* ── Apertura de la ventana X11 ── */
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "No se puede abrir el display X11\n");
        if (fd_socket >= 0) close(fd_socket);
        return EXIT_FAILURE;
    }

    int screen = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        10 + (int)id_ventana * 30, 10 + (int)id_ventana * 30,
        400, 200,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    char titulo[64];
    snprintf(titulo, sizeof(titulo), "Agentic-OS - Ventana %ld", id_ventana);
    XStoreName(display, window, titulo);

    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);

    XEvent event;
    while (1) {
        XNextEvent(display, &event);

        if (event.type == KeyPress) {
            KeySym keysym = XLookupKeysym(&event.xkey, 0);

            if (keysym == XK_Escape) {
                break; /* Sale del bucle para cerrar */
            }

            if (keysym == XK_Return || keysym == XK_KP_Enter) {
                enviar_mensaje(fd_socket, PROTO_RET);
                printf("[Ventana %ld] --- fin de oracion ---\n", id_ventana);
                continue;
            }

            /* Obtener el caracter imprimible de la tecla */
            char buf[8];
            int  n = XLookupString(&event.xkey, buf, sizeof(buf) - 1, NULL, NULL);

            if (n > 0 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127) {
                char msg_char[TAM_MAX_MSG];
                snprintf(msg_char, sizeof(msg_char), "%s %c", PROTO_CHAR, buf[0]);
                enviar_mensaje(fd_socket, msg_char);
                printf("[Ventana %ld] Tecla enviada: '%c'\n", id_ventana, buf[0]);
            }
        }
    }

    /* Avisar que la ventana se cierra, antes de liberar recursos */
    enviar_mensaje(fd_socket, PROTO_FIN);

    if (fd_socket >= 0) close(fd_socket);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return EXIT_SUCCESS;
}
