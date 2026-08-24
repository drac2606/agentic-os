#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ctype.h>

#include "../include/protocolo.h"

/* ─── CONTROL DE CONSOLA Y ESTADO GLOBAL ─── */
static pthread_mutex_t g_mutex_consola = PTHREAD_MUTEX_INITIALIZER;
#define CONSOLA_LOCK()   pthread_mutex_lock(&g_mutex_consola)
#define CONSOLA_UNLOCK() pthread_mutex_unlock(&g_mutex_consola)

static volatile sig_atomic_t g_activo = 1;

/* ─── VARIABLES GLOBALES V2 (ORQUESTACIÓN Y LOTES) ─── */
static ColaOraciones g_cola;
static int g_parametro_p = 0;

static Oracion *g_lote_trabajo;
static int g_oraciones_en_lote = 0;
static int g_detectores_completados = 0;

static pthread_mutex_t g_mutex_lote = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond_detectores = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_cond_lote_terminado = PTHREAD_COND_INITIALIZER;

/* ─── DICCIONARIOS Y BAG OF WORDS ─── */
static const char *DICC_CORREO[] = {"thank", "please", "regards", "meeting", "attached", "information", "update", "schedule", "team", "project", NULL};
static const char *DICC_ARTICULO[] = {"data", "analysis", "results", "method", "study", "model", "research", "system", "significant", "effect", NULL};
static const char *DICC_REPORTE[] = {"system", "data", "network", "security", "application", "server", "user", "performance", "service", "infrastructure", NULL};
static const char *NOMBRE_USUARIO[] = {"Personal administrativo", "Personal tecnico", "Profesor", "Estudiante", "Indeterminado"};

typedef struct {
    char palabra[TAM_MAX_PALABRA];
    int frecuencia;
} EntradaFrecuencia;

typedef struct {
    EntradaFrecuencia entradas[MAX_VOCABULARIO];
    int tamano;
} BolsaPalabras;

typedef struct {
    int id_ventana;
    int en_uso;
    BolsaPalabras bolsa;
    ClaseDocumento clase;
    int evaluado;
    pthread_mutex_t mutex;
} RegistroDocumento;

static RegistroDocumento g_documentos[MAX_VENTANAS];
static pthread_mutex_t g_mutex_docs = PTHREAD_MUTEX_INITIALIZER;

/* ─── FUNCIONES AUXILIARES ─── */
static void a_minusculas(const char *origen, char *destino, size_t max_len) {
    size_t i;
    for (i = 0; i < max_len - 1 && origen[i] != '\0'; i++) {
        destino[i] = tolower((unsigned char)origen[i]);
    }
    destino[i] = '\0';
}

static void bolsa_agregar(BolsaPalabras *b, const char *palabra) {
    if (strlen(palabra) == 0) return;
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

static int contar_coincidencias(BolsaPalabras *b, const char **diccionario, int *freq_total) {
    int coincidencias = 0;
    *freq_total = 0;
    for (int i = 0; diccionario[i] != NULL; i++) {
        char minus_dicc[TAM_MAX_PALABRA];
        a_minusculas(diccionario[i], minus_dicc, sizeof(minus_dicc));
        for (int j = 0; j < b->tamano; j++) {
            if (strncmp(b->entradas[j].palabra, minus_dicc, TAM_MAX_PALABRA) == 0) {
                coincidencias++;
                *freq_total += b->entradas[j].frecuencia;
                break;
            }
        }
    }
    return coincidencias;
}

static ClaseDocumento clasificar_documento(BolsaPalabras *b) {
    int f_correo = 0, f_articulo = 0, f_reporte = 0;
    int c_correo = contar_coincidencias(b, DICC_CORREO, &f_correo);
    int c_articulo = contar_coincidencias(b, DICC_ARTICULO, &f_articulo);
    int c_reporte = contar_coincidencias(b, DICC_REPORTE, &f_reporte);

    ClaseDocumento mejor_clase = CLASE_DESCONOCIDA;
    int max_freq = -1;

    if (c_correo >= MIN_COINCIDENCIAS && f_correo > max_freq) { mejor_clase = CLASE_CORREO; max_freq = f_correo; }
    if (c_articulo >= MIN_COINCIDENCIAS && f_articulo > max_freq) { mejor_clase = CLASE_ARTICULO; max_freq = f_articulo; }
    if (c_reporte >= MIN_COINCIDENCIAS && f_reporte > max_freq) { mejor_clase = CLASE_REPORTE; max_freq = f_reporte; }

    return mejor_clase;
}

static TipoUsuario inferir_tipo_usuario_asincrono(void) {
    int correo = 0, articulo = 0, reporte = 0;
    
    pthread_mutex_lock(&g_mutex_docs);
    for (int i = 0; i < MAX_VENTANAS; i++) {
        if (!g_documentos[i].en_uso) continue;
        if (g_documentos[i].clase == CLASE_CORREO) correo = 1;
        if (g_documentos[i].clase == CLASE_ARTICULO) articulo = 1;
        if (g_documentos[i].clase == CLASE_REPORTE) reporte = 1;
    }
    pthread_mutex_unlock(&g_mutex_docs);

    if (correo && !articulo && reporte) return USUARIO_ADMINISTRATIVO;
    if (correo && !articulo && !reporte) return USUARIO_TECNICO;
    if (correo && articulo && !reporte) return USUARIO_PROFESOR;
    if (!correo && articulo && reporte) return USUARIO_ESTUDIANTE;
    
    return USUARIO_INDETERMINADO;
}

/* ─── HILO DETECTOR (CONSUMIDOR ASINCRÓNICO V2) ─── */
static void *hilo_detector(void *arg) {
    int id_hilo = *(int*)arg;
    free(arg);

    /* Balanceo de carga en CPUs disponibles */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int num_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    CPU_SET(id_hilo % num_cores, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    while (g_activo) {
        pthread_mutex_lock(&g_mutex_lote);
        while (g_oraciones_en_lote == 0 && g_activo) {
            pthread_cond_wait(&g_cond_detectores, &g_mutex_lote);
        }
        if (!g_activo) { pthread_mutex_unlock(&g_mutex_lote); break; }

        Oracion mi_oracion = g_lote_trabajo[--g_oraciones_en_lote];
        pthread_mutex_unlock(&g_mutex_lote);

        RegistroDocumento *doc = NULL;
        pthread_mutex_lock(&g_mutex_docs);
        for (int i = 0; i < MAX_VENTANAS; i++) {
            if (g_documentos[i].en_uso && g_documentos[i].id_ventana == mi_oracion.id_ventana) {
                doc = &g_documentos[i];
                break;
            }
        }
        pthread_mutex_unlock(&g_mutex_docs);

        /* Bag of Words y Clasificación */
        if (doc) {
            pthread_mutex_lock(&doc->mutex);
            char *guardado, *palabra = strtok_r(mi_oracion.texto, " ", &guardado);
            while (palabra != NULL) {
                bolsa_agregar(&doc->bolsa, palabra);
                palabra = strtok_r(NULL, " ", &guardado);
            }
            
            ClaseDocumento clase_nueva = clasificar_documento(&doc->bolsa);
            if (clase_nueva != CLASE_DESCONOCIDA && clase_nueva != doc->clase) {
                doc->clase = clase_nueva;
                doc->evaluado = 1;
                
                TipoUsuario tipo = inferir_tipo_usuario_asincrono();
                CONSOLA_LOCK();
                printf("\n[Hilo %d | CPU Core %d] Documento %d clasificado. Contexto Actualizado: %s\n",
                       id_hilo, id_hilo % num_cores, mi_oracion.id_ventana, NOMBRE_USUARIO[tipo]);
                CONSOLA_UNLOCK();
            }
            pthread_mutex_unlock(&doc->mutex);
        }

        /* Notificar al Loader que la oración asignada fue procesada */
        pthread_mutex_lock(&g_mutex_lote);
        g_detectores_completados++;
        if (g_detectores_completados == g_parametro_p) {
            pthread_cond_signal(&g_cond_lote_terminado);
        }
        pthread_mutex_unlock(&g_mutex_lote);
    }
    return NULL;
}

/* ─── HILO LOADER (ORQUESTADOR DE LOTES V2) ─── */
static void *hilo_loader(void *arg) {
    (void)arg;
    while (g_activo) {
        pthread_mutex_lock(&g_cola.mutex);
        
        /* Esperar hasta que se completen P oraciones en la cola */
        while (g_cola.cantidad_actual < g_parametro_p && g_activo) {
            pthread_cond_wait(&g_cola.cond_loader, &g_cola.mutex);
        }
        if (!g_activo) { pthread_mutex_unlock(&g_cola.mutex); break; }

        pthread_mutex_lock(&g_mutex_lote);
        for (int i = 0; i < g_parametro_p; i++) {
            g_lote_trabajo[i] = g_cola.buffer[g_cola.frente];
            g_cola.frente = (g_cola.frente + 1) % MAX_COLA;
            g_cola.cantidad_actual--;
            g_oraciones_en_lote++;
        }
        g_detectores_completados = 0;

        /* Despertar simultáneamente a los P detectores */
        pthread_cond_broadcast(&g_cond_detectores);

        /* Esperar que el lote actual de P oraciones culmine antes del siguiente */
        while (g_detectores_completados < g_parametro_p && g_activo) {
            pthread_cond_wait(&g_cond_lote_terminado, &g_mutex_lote);
        }

        pthread_mutex_unlock(&g_mutex_lote);
        pthread_mutex_unlock(&g_cola.mutex);
    }
    return NULL;
}

/* ─── HILO DE CONEXIÓN (PRODUCTOR V2) ─── */
static void *hilo_conexion(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    char buffer[TAM_MAX_MSG];
    ssize_t nbytes;
    int id_ventana = -1;

    char oracion_local[TAM_MAX_ORACION] = "";
    int pos_oracion = 0;

    while ((nbytes = recv(fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[nbytes] = '\0';
        char *guardado, *linea = strtok_r(buffer, "\n", &guardado);

        while (linea != NULL) {
            if (strncmp(linea, PROTO_ID, strlen(PROTO_ID)) == 0) {
                id_ventana = atoi(linea + strlen(PROTO_ID) + 1);
                
                pthread_mutex_lock(&g_mutex_docs);
                for (int i = 0; i < MAX_VENTANAS; i++) {
                    if (!g_documentos[i].en_uso) {
                        g_documentos[i].id_ventana = id_ventana;
                        g_documentos[i].en_uso = 1;
                        g_documentos[i].clase = CLASE_DESCONOCIDA;
                        g_documentos[i].evaluado = 0;
                        g_documentos[i].bolsa.tamano = 0;
                        pthread_mutex_init(&g_documentos[i].mutex, NULL);
                        break;
                    }
                }
                pthread_mutex_unlock(&g_mutex_docs);
            }
            else if (strncmp(linea, PROTO_CHAR, strlen(PROTO_CHAR)) == 0) {
                char c = linea[strlen(PROTO_CHAR) + 1];
                if (pos_oracion < TAM_MAX_ORACION - 1) {
                    oracion_local[pos_oracion++] = c;
                    oracion_local[pos_oracion] = '\0';
                }
            } 
            else if (strncmp(linea, PROTO_RET, strlen(PROTO_RET)) == 0) {
                /* Fin de oración -> Encolado concurrente */
                if (pos_oracion > 0 && id_ventana != -1) {
                    pthread_mutex_lock(&g_cola.mutex);
                    strncpy(g_cola.buffer[g_cola.final].texto, oracion_local, TAM_MAX_ORACION - 1);
                    g_cola.buffer[g_cola.final].texto[TAM_MAX_ORACION - 1] = '\0';
                    g_cola.buffer[g_cola.final].id_ventana = id_ventana;
                    g_cola.final = (g_cola.final + 1) % MAX_COLA;
                    g_cola.cantidad_actual++;

                    if (g_cola.cantidad_actual >= g_parametro_p) {
                        pthread_cond_signal(&g_cola.cond_loader);
                    }
                    pthread_mutex_unlock(&g_cola.mutex);
                    
                    pos_oracion = 0;
                    oracion_local[0] = '\0';
                }
            }
            else if (strncmp(linea, PROTO_FIN, strlen(PROTO_FIN)) == 0) {
                goto fin_ventana;
            }
            linea = strtok_r(NULL, "\n", &guardado);
        }
    }

fin_ventana:
    close(fd);
    if (id_ventana != -1) {
        pthread_mutex_lock(&g_mutex_docs);
        for (int i = 0; i < MAX_VENTANAS; i++) {
            if (g_documentos[i].en_uso && g_documentos[i].id_ventana == id_ventana) {
                g_documentos[i].en_uso = 0;
                pthread_mutex_destroy(&g_documentos[i].mutex);
                break;
            }
        }
        pthread_mutex_unlock(&g_mutex_docs);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: ./ia_learner <puerto> <P_oraciones>\n");
        return EXIT_FAILURE;
    }
    int puerto = atoi(argv[1]);
    g_parametro_p = atoi(argv[2]);

    memset(g_documentos, 0, sizeof(g_documentos));
    
    pthread_mutex_init(&g_cola.mutex, NULL);
    pthread_cond_init(&g_cola.cond_loader, NULL);
    g_cola.frente = 0; g_cola.final = 0; g_cola.cantidad_actual = 0;

    g_lote_trabajo = malloc(sizeof(Oracion) * g_parametro_p);

    pthread_t loader;
    pthread_create(&loader, NULL, hilo_loader, NULL);

    pthread_t *detectores = malloc(sizeof(pthread_t) * g_parametro_p);
    for (int i = 0; i < g_parametro_p; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        pthread_create(&detectores[i], NULL, hilo_detector, id);
    }

    int fd_servidor = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd_servidor, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dir;
    memset(&dir, 0, sizeof(dir));
    dir.sin_family = AF_INET;
    dir.sin_addr.s_addr = INADDR_ANY;
    dir.sin_port = htons((uint16_t)puerto);

    bind(fd_servidor, (struct sockaddr *)&dir, sizeof(dir));
    listen(fd_servidor, MAX_VENTANAS);

    CONSOLA_LOCK();
    printf("Agentic-OS V2 Iniciado (Puerto %d | Limite de oraciones y nucleos P = %d)\n", puerto, g_parametro_p);
    CONSOLA_UNLOCK();

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
    free(g_lote_trabajo);
    free(detectores);
    return EXIT_SUCCESS;
}
