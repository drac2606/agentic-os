#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>

#include "../include/protocolo.h"

static pthread_mutex_t g_mutex_consola = PTHREAD_MUTEX_INITIALIZER;
#define CONSOLA_LOCK()   pthread_mutex_lock(&g_mutex_consola)
#define CONSOLA_UNLOCK() pthread_mutex_unlock(&g_mutex_consola)

static volatile sig_atomic_t g_activo = 1;

static const char *DICC_CORREO[] = {
    "thank", "please", "regards", "meeting", "attached",
    "information", "update", "schedule", "team", "project", NULL
};
static const char *DICC_ARTICULO[] = {
    "data", "analysis", "results", "method", "study",
    "model", "research", "system", "significant", "effect", NULL
};
static const char *DICC_REPORTE[] = {
    "system", "data", "network", "security", "application",
    "server", "user", "performance", "service", "infrastructure", NULL
};
static const char **DICCIONARIOS[NUM_CLASES] = { DICC_CORREO, DICC_ARTICULO, DICC_REPORTE };
static const char *NOMBRE_CLASE[NUM_CLASES] = { "Correo electronico", "Articulo cientifico", "Reporte" };
static const char *NOMBRE_USUARIO[] = { "Personal administrativo", "Personal tecnico", "Profesor", "Estudiante", "Indeterminado" };

typedef struct {
    char palabra[TAM_MAX_PALABRA];
    int  frecuencia;
} EntradaFrecuencia;

typedef struct {
    EntradaFrecuencia entradas[MAX_VOCABULARIO];
    int tamano;
} BolsaPalabras;

static void a_minusculas(const char *origen, char *destino, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && origen[i]; i++) {
        destino[i] = (char)tolower((unsigned char)origen[i]);
    }
    destino[i] = '\0';
}

static void bolsa_agregar(BolsaPalabras *b, const char *palabra) {
    if (!palabra || palabra[0] == '\0') return;
    char minus[TAM_MAX_PALABRA];
    a_minusculas(palabra, minus, sizeof(minus));

    for (int i = 0; i < b->tamano; i++) {
        if (strncmp(b->entradas[i].palabra, minus, TAM_MAX_PALABRA) == 0) {
            b->entradas[i].frecuencia++;
            return;
        }
    }
    if (b->tamano < MAX_VOCABULARIO) {
        strncpy(b->entradas[b->tamano].palabra, minus, TAM_MAX_PALABRA - 1);
        b->entradas[b->tamano].frecuencia = 1;
        b->tamano++;
    }
}

static int bolsa_frecuencia(const BolsaPalabras *b, const char *palabra) {
    char minus[TAM_MAX_PALABRA];
    a_minusculas(palabra, minus, sizeof(minus));
    for (int i = 0; i < b->tamano; i++) {
        if (strncmp(b->entradas[i].palabra, minus, TAM_MAX_PALABRA) == 0)
            return b->entradas[i].frecuencia;
    }
    return 0;
}

typedef struct {
    int id_ventana;
    int en_uso;
    BolsaPalabras bolsa;
    ClaseDocumento clase;
    char palabra_actual[TAM_MAX_PALABRA];
    int pos_palabra;
    pthread_mutex_t mutex;
} RegistroDocumento;

typedef struct {
    RegistroDocumento documentos[MAX_VENTANAS];
    int terminados;
    int total_esperado;
    pthread_mutex_t mutex;
    pthread_cond_t cambio;
} TablaDocumentos;

static TablaDocumentos g_tabla;

static ClaseDocumento clasificar_documento(const BolsaPalabras *b, int id_ventana) {
    ClaseDocumento mejor = CLASE_DESCONOCIDA;
    int mejor_suma = -1;

    CONSOLA_LOCK();
    printf("\n[IALearner] Clasificando ventana %d:\n", id_ventana);
    for (int c = 0; c < NUM_CLASES; c++) {
        int matches = 0, suma = 0;
        for (int i = 0; DICCIONARIOS[c][i] != NULL; i++) {
            int f = bolsa_frecuencia(b, DICCIONARIOS[c][i]);
            if (f > 0) { suma += f; matches++; }
        }
        printf("  %-22s -> %d coincidencias, frec. total = %d", NOMBRE_CLASE[c], matches, suma);
        if (matches >= MIN_COINCIDENCIAS) {
            printf(" [ELEGIBLE]");
            if (suma > mejor_suma) { mejor_suma = suma; mejor = (ClaseDocumento)c; }
        }
        printf("\n");
    }
    if (mejor != CLASE_DESCONOCIDA) printf("  -> Clase asignada: %s\n", NOMBRE_CLASE[mejor]);
    CONSOLA_UNLOCK();
    return mejor;
}

static TipoUsuario inferir_tipo_usuario() {
    int correo = 0, articulo = 0, reporte = 0;
    for (int i = 0; i < MAX_VENTANAS; i++) {
        if (!g_tabla.documentos[i].en_uso) continue;
        if (g_tabla.documentos[i].clase == CLASE_CORREO) correo = 1;
        if (g_tabla.documentos[i].clase == CLASE_ARTICULO) articulo = 1;
        if (g_tabla.documentos[i].clase == CLASE_REPORTE) reporte = 1;
    }

    if (correo && !articulo && !reporte) return USUARIO_ADMINISTRATIVO;
    if (correo && !articulo && reporte) return USUARIO_TECNICO;
    if (correo && articulo && !reporte) return USUARIO_PROFESOR;
    if (!correo && articulo && reporte) return USUARIO_ESTUDIANTE;
    return USUARIO_INDETERMINADO;
}

/* --- SOLUCION CONTEXTO: Bucle permanente y reinicio de variables --- */
static void *hilo_clasificador(void *arg) {
    (void)arg;
    while (g_activo) {
        pthread_mutex_lock(&g_tabla.mutex);
        /* Espera asíncrona hasta que se terminen las ventanas esperadas de la ronda actual */
        while (g_activo && !(g_tabla.total_esperado > 0 && g_tabla.terminados >= g_tabla.total_esperado)) {
            pthread_cond_wait(&g_tabla.cambio, &g_tabla.mutex);
        }
        
        if (!g_activo) {
            pthread_mutex_unlock(&g_tabla.mutex);
            break;
        }

        /* Reiniciamos el esperado para obligarlo a dormir hasta que el Launcher vuelva a abrirse */
        g_tabla.total_esperado = 0;
        pthread_mutex_unlock(&g_tabla.mutex);

        TipoUsuario usuario = inferir_tipo_usuario();
        CONSOLA_LOCK();
        printf("\n================================================\n");
        printf("  CONTEXTO DE USUARIO DETECTADO: %s\n", NOMBRE_USUARIO[usuario]);
        printf("================================================\n\n");
        CONSOLA_UNLOCK();
    }
    return NULL;
}

static void *hilo_conexion(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    char buffer[TAM_MAX_MSG];
    ssize_t nbytes;
    int id_ventana = -1;
    RegistroDocumento *doc = NULL;

    while ((nbytes = recv(fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[nbytes] = '\0';
        char *guardado, *linea = strtok_r(buffer, "\n", &guardado);

        while (linea != NULL) {
            if (strncmp(linea, PROTO_TOTAL, strlen(PROTO_TOTAL)) == 0) {
                int total = atoi(linea + strlen(PROTO_TOTAL) + 1);
                pthread_mutex_lock(&g_tabla.mutex);
                
                /* SOLUCION CONTEXTO: Limpiar la tabla de registros viejos cada que arranca el launcher */
                for (int i = 0; i < MAX_VENTANAS; i++) {
                    g_tabla.documentos[i].en_uso = 0;
                    g_tabla.documentos[i].clase = CLASE_DESCONOCIDA;
                    g_tabla.documentos[i].bolsa.tamano = 0;
                    g_tabla.documentos[i].pos_palabra = 0;
                    g_tabla.documentos[i].palabra_actual[0] = '\0';
                }
                g_tabla.terminados = 0;
                g_tabla.total_esperado = total;
                pthread_cond_broadcast(&g_tabla.cambio);
                pthread_mutex_unlock(&g_tabla.mutex);
                
                CONSOLA_LOCK();
                printf("\n[IALearner] Launcher informo: esperar %d ventana(s)\n", total);
                CONSOLA_UNLOCK();
                close(fd);
                return NULL;
            } 
            else if (strncmp(linea, PROTO_ID, strlen(PROTO_ID)) == 0) {
                id_ventana = atoi(linea + strlen(PROTO_ID) + 1);
                pthread_mutex_lock(&g_tabla.mutex);
                for (int i = 0; i < MAX_VENTANAS; i++) {
                    if (!g_tabla.documentos[i].en_uso) {
                        doc = &g_tabla.documentos[i];
                        doc->id_ventana = id_ventana;
                        doc->en_uso = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&g_tabla.mutex);
                CONSOLA_LOCK();
                printf("[IALearner] Ventana %d conectada\n", id_ventana);
                CONSOLA_UNLOCK();
            } 
            else if (strncmp(linea, PROTO_CHAR, strlen(PROTO_CHAR)) == 0 && doc) {
                char c = linea[strlen(PROTO_CHAR) + 1];
                pthread_mutex_lock(&doc->mutex);
                if (c == ' ') {
                    if (doc->pos_palabra > 0) {
                        bolsa_agregar(&doc->bolsa, doc->palabra_actual);
                        doc->pos_palabra = 0;
                        doc->palabra_actual[0] = '\0';
                    }
                } else if (doc->pos_palabra < TAM_MAX_PALABRA - 1) {
                    doc->palabra_actual[doc->pos_palabra++] = c;
                    doc->palabra_actual[doc->pos_palabra] = '\0';
                }
                pthread_mutex_unlock(&doc->mutex);
            } 
            else if (strncmp(linea, PROTO_RET, strlen(PROTO_RET)) == 0 && doc) {
                /* Capturar si presionó enter antes de un espacio */
                pthread_mutex_lock(&doc->mutex);
                if (doc->pos_palabra > 0) {
                    bolsa_agregar(&doc->bolsa, doc->palabra_actual);
                    doc->pos_palabra = 0;
                    doc->palabra_actual[0] = '\0';
                }
                pthread_mutex_unlock(&doc->mutex);
            }
            else if (strncmp(linea, PROTO_FIN, strlen(PROTO_FIN)) == 0) {
                goto fin_ventana;
            }
            linea = strtok_r(NULL, "\n", &guardado);
        }
    }

fin_ventana:
    close(fd);
    if (doc) {
        pthread_mutex_lock(&doc->mutex);
        if (doc->pos_palabra > 0) {
            bolsa_agregar(&doc->bolsa, doc->palabra_actual);
        }
        doc->clase = clasificar_documento(&doc->bolsa, id_ventana);
        pthread_mutex_unlock(&doc->mutex);

        pthread_mutex_lock(&g_tabla.mutex);
        g_tabla.terminados++;
        pthread_cond_broadcast(&g_tabla.cambio);
        pthread_mutex_unlock(&g_tabla.mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int puerto = PUERTO_DEFECTO;
    if (argc == 2) puerto = atoi(argv[1]);

    memset(&g_tabla, 0, sizeof(g_tabla));
    pthread_mutex_init(&g_tabla.mutex, NULL);
    pthread_cond_init(&g_tabla.cambio, NULL);
    for (int i = 0; i < MAX_VENTANAS; i++) pthread_mutex_init(&g_tabla.documentos[i].mutex, NULL);

    pthread_t hilo_clf;
    pthread_create(&hilo_clf, NULL, hilo_clasificador, NULL);

    int fd_servidor = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd_servidor, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dir;
    memset(&dir, 0, sizeof(dir));
    dir.sin_family = AF_INET;
    dir.sin_addr.s_addr = INADDR_ANY;
    dir.sin_port = htons(puerto);

    bind(fd_servidor, (struct sockaddr *)&dir, sizeof(dir));
    listen(fd_servidor, MAX_VENTANAS);

    printf("[IALearner] Escuchando en el puerto %d...\n\n", puerto);

    while (g_activo) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int fd_cliente = accept(fd_servidor, (struct sockaddr *)&cli, &len);
        if (fd_cliente < 0) continue;

        int *arg = malloc(sizeof(int));
        *arg = fd_cliente;
        pthread_t tid;
        pthread_create(&tid, NULL, hilo_conexion, arg);
        pthread_detach(tid);
    }

    close(fd_servidor);
    return EXIT_SUCCESS;
}
