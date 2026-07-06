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

#include "../include/protocolo.h"

static int conectar_ia_learner(const char *host, int puerto) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in direccion;
    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family = AF_INET;
    direccion.sin_port   = htons((uint16_t)puerto);
    if (inet_pton(AF_INET, host, &direccion.sin_addr) <= 0) {
        close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&direccion, sizeof(direccion)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static void enviar_mensaje(int fd_socket, const char *mensaje) {
    if (fd_socket < 0) return;
    char buffer[TAM_MAX_MSG];
    snprintf(buffer, sizeof(buffer), "%s\n", mensaje);
    send(fd_socket, buffer, strlen(buffer), MSG_NOSIGNAL);
}

int main(int argc, char *argv[]) {
    if (argc != 4) return EXIT_FAILURE;
    const char *host = argv[1];
    long puerto = strtol(argv[2], NULL, 10);
    long id_ventana = strtol(argv[3], NULL, 10);
    
    signal(SIGPIPE, SIG_IGN);
    int fd_socket = conectar_ia_learner(host, (int)puerto);

    char msg_id[TAM_MAX_MSG];
    snprintf(msg_id, sizeof(msg_id), "%s %ld", PROTO_ID, id_ventana);
    enviar_mensaje(fd_socket, msg_id);

    Display *display = XOpenDisplay(NULL);
    if (!display) return EXIT_FAILURE;

    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        10 + (int)id_ventana * 30, 10 + (int)id_ventana * 30,
        450, 200, 1, BlackPixel(display, screen), WhitePixel(display, screen)
    );

    char titulo[64];
    snprintf(titulo, sizeof(titulo), "Agentic-OS - Ventana %ld", id_ventana);
    XStoreName(display, window, titulo);

    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);

    /* --- SOLUCION GRAFICA: Contexto y Búfer --- */
    GC gc = XCreateGC(display, window, 0, NULL);
    char texto_pantalla[TAM_MAX_ORACION];
    memset(texto_pantalla, 0, sizeof(texto_pantalla));
    int len_texto = 0;

    XEvent event;
    while (1) {
        XNextEvent(display, &event);

        if (event.type == Expose) {
            /* Repintar el texto cuando la ventana se refresca o mueve */
            XClearWindow(display, window);
            XDrawString(display, window, gc, 20, 30, "Escribe y presiona Enter para enviar la oracion.", 48);
            XDrawString(display, window, gc, 20, 50, "(Presiona Escape para salir)", 28);
            if (len_texto > 0) {
                XDrawString(display, window, gc, 20, 100, texto_pantalla, len_texto);
            }
        }
        else if (event.type == KeyPress) {
            KeySym keysym = XLookupKeysym(&event.xkey, 0);

            if (keysym == XK_Escape) break;

            if (keysym == XK_Return || keysym == XK_KP_Enter) {
                enviar_mensaje(fd_socket, PROTO_RET);
                printf("[Ventana %ld] --- fin de oracion ---\n", id_ventana);
                
                /* Limpiar la pantalla para la siguiente oracion */
                memset(texto_pantalla, 0, sizeof(texto_pantalla));
                len_texto = 0;
                XClearWindow(display, window);
                XDrawString(display, window, gc, 20, 30, "Escribe y presiona Enter para enviar la oracion.", 48);
                XDrawString(display, window, gc, 20, 50, "(Presiona Escape para salir)", 28);
                continue;
            }

            /* Logica para usar la tecla de borrar (BackSpace) */
            if (keysym == XK_BackSpace && len_texto > 0) {
                texto_pantalla[--len_texto] = '\0';
                XClearWindow(display, window);
                XDrawString(display, window, gc, 20, 30, "Escribe y presiona Enter para enviar la oracion.", 48);
                XDrawString(display, window, gc, 20, 50, "(Presiona Escape para salir)", 28);
                if (len_texto > 0) XDrawString(display, window, gc, 20, 100, texto_pantalla, len_texto);
                continue;
            }

            char buf[8];
            int n = XLookupString(&event.xkey, buf, sizeof(buf) - 1, NULL, NULL);

            if (n > 0 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127) {
                char msg_char[TAM_MAX_MSG];
                snprintf(msg_char, sizeof(msg_char), "%s %c", PROTO_CHAR, buf[0]);
                enviar_mensaje(fd_socket, msg_char);
                printf("[Ventana %ld] Tecla enviada: '%c'\n", id_ventana, buf[0]);
                
                /* Acumular letra y redibujar en pantalla */
                if (len_texto < (int)sizeof(texto_pantalla) - 1) {
                    texto_pantalla[len_texto++] = buf[0];
                    texto_pantalla[len_texto] = '\0';
                    XClearWindow(display, window);
                    XDrawString(display, window, gc, 20, 30, "Escribe y presiona Enter para enviar la oracion.", 48);
                    XDrawString(display, window, gc, 20, 50, "(Presiona Escape para salir)", 28);
                    XDrawString(display, window, gc, 20, 100, texto_pantalla, len_texto);
                }
            }
        }
    }

    enviar_mensaje(fd_socket, PROTO_FIN);
    if (fd_socket >= 0) close(fd_socket);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return EXIT_SUCCESS;
}
