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
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>

#include "../include/protocolo.h"

/* ============================================================
 * ESTADO GLOBAL
 * ============================================================ */

static volatile sig_atomic_t g_activo = 1;
static int g_fd_servidor = -1;

static pthread_mutex_t g_mutex_consola = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_mutex_lote = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond_detectores = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_cond_lote_terminado = PTHREAD_COND_INITIALIZER;

static ColaOraciones g_cola;

static int g_parametro_p = 0;

static Oracion *g_lote_trabajo = NULL;
static int g_oraciones_en_lote = 0;
static int g_detectores_completados = 0;

/* ============================================================
 * CONTROL DE VENTANAS
 * ============================================================ */

static int g_total_ventanas = 0;
static int g_ventanas_terminadas = 0;

static pthread_mutex_t g_mutex_ventanas = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * DICCIONARIOS
 * ============================================================ */

static const char *DICC_CORREO[] = {
    "thank",
    "please",
    "regards",
    "meeting",
    "attached",
    "information",
    "update",
    "schedule",
    "team",
    "project",
    NULL
};

static const char *DICC_ARTICULO[] = {
    "data",
    "analysis",
    "results",
    "method",
    "study",
    "model",
    "research",
    "system",
    "significant",
    "effect",
    NULL
};

static const char *DICC_REPORTE[] = {
    "system",
    "data",
    "network",
    "security",
    "application",
    "server",
    "user",
    "performance",
    "service",
    "infrastructure",
    NULL
};

/* ============================================================
 * TIPOS DE USUARIO
 *
 * Tabla:
 *
 *                 Correo  Articulo  Reporte
 * Administrativo    X        -         X
 * Tecnico           X        -         -
 * Profesor          X        X         -
 * Estudiante        -        X         X
 * ============================================================ */

static const char *NOMBRE_USUARIO[] = {
    "Personal administrativo",
    "Personal tecnico",
    "Profesor",
    "Estudiante",
    "Indeterminado"
};

/* ============================================================
 * BAG OF WORDS
 * ============================================================ */

typedef struct {
    char palabra[TAM_MAX_PALABRA];
    int frecuencia;
} EntradaFrecuencia;

typedef struct {
    EntradaFrecuencia entradas[MAX_VOCABULARIO];
    int tamano;
} BolsaPalabras;

/* ============================================================
 * REGISTRO DE CADA VENTANA
 * ============================================================ */

typedef struct {
    int id_ventana;
    int en_uso;
    int conexion_cerrada;

    int pendiente;

    BolsaPalabras bolsa;

    ClaseDocumento clase;
    int evaluado;

    int tuvo_correo;
    int tuvo_articulo;
    int tuvo_reporte;

    pthread_mutex_t mutex;

} RegistroDocumento;

static RegistroDocumento g_documentos[MAX_VENTANAS];

static pthread_mutex_t g_mutex_docs = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * SEÑALES
 * ============================================================ */

static void manejar_senal(int sig)
{
    (void)sig;

    g_activo = 0;

    if (g_fd_servidor >= 0) {
        close(g_fd_servidor);
        g_fd_servidor = -1;
    }

    pthread_cond_broadcast(&g_cola.cond_loader);
    pthread_cond_broadcast(&g_cond_detectores);
    pthread_cond_broadcast(&g_cond_lote_terminado);
}

/* ============================================================
 * UTILIDADES
 * ============================================================ */

static void imprimir_seguro(const char *formato, ...)
{
    va_list args;

    pthread_mutex_lock(&g_mutex_consola);

    va_start(args, formato);
    vprintf(formato, args);
    va_end(args);

    fflush(stdout);

    pthread_mutex_unlock(&g_mutex_consola);
}

static const char *nombre_clase(ClaseDocumento clase)
{
    switch (clase) {

        case CLASE_CORREO:
            return "CORREO";

        case CLASE_ARTICULO:
            return "ARTICULO";

        case CLASE_REPORTE:
            return "REPORTE";

        default:
            return "DESCONOCIDO";
    }
}

static void a_minusculas(
    const char *origen,
    char *destino,
    size_t max_len
)
{
    size_t i;

    if (max_len == 0)
        return;

    for (
        i = 0;
        i < max_len - 1 && origen[i] != '\0';
        i++
    ) {
        destino[i] =
            (char)tolower(
                (unsigned char)origen[i]
            );
    }

    destino[i] = '\0';
}

/* ============================================================
 * BAG OF WORDS
 * ============================================================ */

static void bolsa_agregar(
    BolsaPalabras *b,
    const char *palabra
)
{
    char minus[TAM_MAX_PALABRA];

    if (palabra == NULL || palabra[0] == '\0')
        return;

    a_minusculas(
        palabra,
        minus,
        sizeof(minus)
    );

    for (int i = 0; i < b->tamano; i++) {

        if (
            strcmp(
                b->entradas[i].palabra,
                minus
            ) == 0
        ) {
            b->entradas[i].frecuencia++;
            return;
        }
    }

    if (b->tamano < MAX_VOCABULARIO) {

        strncpy(
            b->entradas[b->tamano].palabra,
            minus,
            TAM_MAX_PALABRA - 1
        );

        b->entradas[b->tamano]
            .palabra[TAM_MAX_PALABRA - 1] = '\0';

        b->entradas[b->tamano].frecuencia = 1;

        b->tamano++;
    }
}

/* ============================================================
 * CONTAR COINCIDENCIAS
 * ============================================================ */

static int contar_coincidencias(
    BolsaPalabras *b,
    const char **diccionario,
    int *freq_total
)
{
    int coincidencias = 0;

    *freq_total = 0;

    for (
        int i = 0;
        diccionario[i] != NULL;
        i++
    ) {

        char minus_dicc[TAM_MAX_PALABRA];

        a_minusculas(
            diccionario[i],
            minus_dicc,
            sizeof(minus_dicc)
        );

        for (int j = 0; j < b->tamano; j++) {

            if (
                strcmp(
                    b->entradas[j].palabra,
                    minus_dicc
                ) == 0
            ) {

                coincidencias++;

                *freq_total +=
                    b->entradas[j].frecuencia;

                break;
            }
        }
    }

    return coincidencias;
}

/* ============================================================
 * CLASIFICACION DEL DOCUMENTO
 *
 * Regla:
 * Si >= 3 palabras del diccionario aparecen,
 * pertenece a esa clase.
 *
 * Si pertenece a varias:
 * gana la de mayor frecuencia total.
 * ============================================================ */

static ClaseDocumento clasificar_documento(
    BolsaPalabras *b,
    int *mejores_coincidencias,
    int *mejor_frecuencia
)
{
    int f_correo = 0;
    int f_articulo = 0;
    int f_reporte = 0;

    int c_correo =
        contar_coincidencias(
            b,
            DICC_CORREO,
            &f_correo
        );

    int c_articulo =
        contar_coincidencias(
            b,
            DICC_ARTICULO,
            &f_articulo
        );

    int c_reporte =
        contar_coincidencias(
            b,
            DICC_REPORTE,
            &f_reporte
        );

    ClaseDocumento mejor_clase =
        CLASE_DESCONOCIDA;

    int max_freq = -1;
    int max_coincidencias = 0;

    if (
        c_correo >= MIN_COINCIDENCIAS &&
        f_correo > max_freq
    ) {

        mejor_clase = CLASE_CORREO;
        max_freq = f_correo;
        max_coincidencias = c_correo;
    }

    if (
        c_articulo >= MIN_COINCIDENCIAS &&
        f_articulo > max_freq
    ) {

        mejor_clase = CLASE_ARTICULO;
        max_freq = f_articulo;
        max_coincidencias = c_articulo;
    }

    if (
        c_reporte >= MIN_COINCIDENCIAS &&
        f_reporte > max_freq
    ) {

        mejor_clase = CLASE_REPORTE;
        max_freq = f_reporte;
        max_coincidencias = c_reporte;
    }

    *mejores_coincidencias =
        max_coincidencias;

    *mejor_frecuencia =
        max_freq < 0 ? 0 : max_freq;

    return mejor_clase;
}

/* ============================================================
 * INFERIR USUARIO
 *
 * TABLA EXACTA:
 *
 * Administrativo = Correo + Reporte
 * Tecnico        = Correo
 * Profesor       = Correo + Articulo
 * Estudiante     = Articulo + Reporte
 * ============================================================ */

static TipoUsuario inferir_tipo_usuario(
    int correo,
    int articulo,
    int reporte
)
{
    if (
        correo &&
        !articulo &&
        reporte
    ) {
        return USUARIO_ADMINISTRATIVO;
    }

    if (
        correo &&
        !articulo &&
        !reporte
    ) {
        return USUARIO_TECNICO;
    }

    if (
        correo &&
        articulo &&
        !reporte
    ) {
        return USUARIO_PROFESOR;
    }

    if (
        !correo &&
        articulo &&
        reporte
    ) {
        return USUARIO_ESTUDIANTE;
    }

    return USUARIO_INDETERMINADO;
}

/* ============================================================
 * OBTENER EVIDENCIA GLOBAL
 * ============================================================ */

static void obtener_evidencia_global(
    int *correo,
    int *articulo,
    int *reporte
)
{
    *correo = 0;
    *articulo = 0;
    *reporte = 0;

    pthread_mutex_lock(&g_mutex_docs);

    for (int i = 0; i < MAX_VENTANAS; i++) {

        if (!g_documentos[i].en_uso)
            continue;

        if (g_documentos[i].tuvo_correo)
            *correo = 1;

        if (g_documentos[i].tuvo_articulo)
            *articulo = 1;

        if (g_documentos[i].tuvo_reporte)
            *reporte = 1;
    }

    pthread_mutex_unlock(&g_mutex_docs);
}

/* ============================================================
 * MOSTRAR CONTEXTO ACTUAL
 * ============================================================ */

static void mostrar_contexto_actual(void)
{
    int correo;
    int articulo;
    int reporte;

    obtener_evidencia_global(
        &correo,
        &articulo,
        &reporte
    );

    TipoUsuario tipo =
        inferir_tipo_usuario(
            correo,
            articulo,
            reporte
        );

    imprimir_seguro(
        "  Contexto de usuario: %s\n"
        "  Evidencia global -> "
        "Correo: %s | "
        "Articulo: %s | "
        "Reporte: %s\n",

        NOMBRE_USUARIO[tipo],

        correo ? "SI" : "NO",
        articulo ? "SI" : "NO",
        reporte ? "SI" : "NO"
    );
}

/* ============================================================
 * MOSTRAR CONTEXTO FINAL
 * ============================================================ */

static void mostrar_contexto_final(void)
{
    int correo;
    int articulo;
    int reporte;

    obtener_evidencia_global(
        &correo,
        &articulo,
        &reporte
    );

    TipoUsuario tipo =
        inferir_tipo_usuario(
            correo,
            articulo,
            reporte
        );

    pthread_mutex_lock(&g_mutex_consola);

    printf("\n");
    printf("========================================\n");
    printf("       CONTEXTO FINAL DEL USUARIO\n");
    printf("========================================\n");

    printf(
        "Correo:   %s\n",
        correo ? "SI" : "NO"
    );

    printf(
        "Articulo: %s\n",
        articulo ? "SI" : "NO"
    );

    printf(
        "Reporte:  %s\n",
        reporte ? "SI" : "NO"
    );

    printf(
        "Contexto: %s\n",
        NOMBRE_USUARIO[tipo]
    );

    printf("========================================\n\n");

    fflush(stdout);

    pthread_mutex_unlock(&g_mutex_consola);
}

/* ============================================================
 * BUSCAR DOCUMENTO
 * ============================================================ */

static RegistroDocumento *buscar_documento(
    int id_ventana
)
{
    for (int i = 0; i < MAX_VENTANAS; i++) {

        if (
            g_documentos[i].en_uso &&
            g_documentos[i].id_ventana == id_ventana
        ) {
            return &g_documentos[i];
        }
    }

    return NULL;
}

/* ============================================================
 * CREAR DOCUMENTO
 * ============================================================ */

static int crear_documento(
    int id_ventana
)
{
    pthread_mutex_lock(&g_mutex_docs);

    if (
        buscar_documento(id_ventana) != NULL
    ) {
        pthread_mutex_unlock(&g_mutex_docs);
        return 0;
    }

    for (int i = 0; i < MAX_VENTANAS; i++) {

        if (!g_documentos[i].en_uso) {

            g_documentos[i].id_ventana =
                id_ventana;

            g_documentos[i].en_uso = 1;
            g_documentos[i].conexion_cerrada = 0;
            g_documentos[i].pendiente = 0;

            g_documentos[i].bolsa.tamano = 0;

            g_documentos[i].clase =
                CLASE_DESCONOCIDA;

            g_documentos[i].evaluado = 0;

            g_documentos[i].tuvo_correo = 0;
            g_documentos[i].tuvo_articulo = 0;
            g_documentos[i].tuvo_reporte = 0;

            pthread_mutex_init(
                &g_documentos[i].mutex,
                NULL
            );

            pthread_mutex_lock(
                &g_mutex_ventanas
            );

            g_total_ventanas++;

            pthread_mutex_unlock(
                &g_mutex_ventanas
            );

            pthread_mutex_unlock(
                &g_mutex_docs
            );

            return 0;
        }
    }

    pthread_mutex_unlock(&g_mutex_docs);

    return -1;
}

/* ============================================================
 * LIBERAR DOCUMENTO
 * ============================================================ */

static void liberar_documento(
    int id_ventana
)
{
    int mostrar_final = 0;

    pthread_mutex_lock(&g_mutex_docs);

    RegistroDocumento *doc =
        buscar_documento(id_ventana);

    if (doc != NULL) {

        doc->conexion_cerrada = 1;

        /*
         * No se elimina el documento de g_documentos.
         *
         * La información debe permanecer disponible
         * para el contexto global hasta finalizar el servidor.
         */

        pthread_mutex_lock(
            &g_mutex_ventanas
        );

        g_ventanas_terminadas++;

        imprimir_seguro(
            "[CONEXION] Ventana %d cerrada. "
            "Ventanas terminadas: %d/%d\n",
            id_ventana,
            g_ventanas_terminadas,
            g_total_ventanas
        );

        if (
            g_total_ventanas > 0 &&
            g_ventanas_terminadas >= g_total_ventanas
        ) {
            mostrar_final = 1;
        }

        pthread_mutex_unlock(
            &g_mutex_ventanas
        );
    }

    pthread_mutex_unlock(&g_mutex_docs);

    if (mostrar_final) {
        mostrar_contexto_final();
    }
}

/* ============================================================
 * HILO DETECTOR
 * ============================================================ */

static void *hilo_detector(
    void *arg
)
{
    int id_hilo = *(int *)arg;

    free(arg);

    int num_cores =
        (int)sysconf(
            _SC_NPROCESSORS_ONLN
        );

    if (num_cores < 1)
        num_cores = 1;

    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);

    CPU_SET(
        id_hilo % num_cores,
        &cpuset
    );

    pthread_setaffinity_np(
        pthread_self(),
        sizeof(cpu_set_t),
        &cpuset
    );

    while (g_activo) {

        pthread_mutex_lock(
            &g_mutex_lote
        );

        while (
            g_oraciones_en_lote == 0 &&
            g_activo
        ) {

            pthread_cond_wait(
                &g_cond_detectores,
                &g_mutex_lote
            );
        }

        if (!g_activo) {

            pthread_mutex_unlock(
                &g_mutex_lote
            );

            break;
        }

        /*
         * Cada hilo obtiene UNA oración.
         *
         * Como g_oraciones_en_lote representa
         * las oraciones pendientes del lote,
         * máximo P hilos estarán trabajando.
         */

        Oracion mi_oracion =
            g_lote_trabajo[
                --g_oraciones_en_lote
            ];

        pthread_mutex_unlock(
            &g_mutex_lote
        );

        pthread_mutex_lock(
            &g_mutex_docs
        );

        RegistroDocumento *doc =
            buscar_documento(
                mi_oracion.id_ventana
            );

        if (doc != NULL)
            doc->pendiente++;

        pthread_mutex_unlock(
            &g_mutex_docs
        );

        if (doc != NULL) {

            pthread_mutex_lock(
                &doc->mutex
            );

            /*
             * Convertir la oración en Bag of Words.
             */

            char *guardado = NULL;

            char *palabra =
                strtok_r(
                    mi_oracion.texto,
                    " \t\r\n",
                    &guardado
                );

            while (palabra != NULL) {

                bolsa_agregar(
                    &doc->bolsa,
                    palabra
                );

                palabra =
                    strtok_r(
                        NULL,
                        " \t\r\n",
                        &guardado
                    );
            }

            int coincidencias = 0;
            int frecuencia = 0;

            ClaseDocumento clase_nueva =
                clasificar_documento(
                    &doc->bolsa,
                    &coincidencias,
                    &frecuencia
                );

            if (
                clase_nueva !=
                CLASE_DESCONOCIDA
            ) {

                doc->clase =
                    clase_nueva;

                doc->evaluado = 1;

                /*
                 * La evidencia global se conserva.
                 */

                if (
                    clase_nueva ==
                    CLASE_CORREO
                ) {
                    doc->tuvo_correo = 1;
                }

                else if (
                    clase_nueva ==
                    CLASE_ARTICULO
                ) {
                    doc->tuvo_articulo = 1;
                }

                else if (
                    clase_nueva ==
                    CLASE_REPORTE
                ) {
                    doc->tuvo_reporte = 1;
                }

                imprimir_seguro(
                    "\n[Hilo %d | CPU Core %d]\n"
                    "  Documento: %d\n"
                    "  Clase: %s\n"
                    "  Coincidencias: %d\n"
                    "  Frecuencia total: %d\n",

                    id_hilo,
                    id_hilo % num_cores,

                    mi_oracion.id_ventana,

                    nombre_clase(
                        clase_nueva
                    ),

                    coincidencias,
                    frecuencia
                );

                /*
                 * El contexto se calcula DESPUÉS de actualizar
                 * la evidencia de esta oración.
                 *
                 * Esto permite que la decisión sea asincrónica.
                 */

                mostrar_contexto_actual();
            }

            else {

                imprimir_seguro(
                    "\n[Hilo %d | CPU Core %d]\n"
                    "  Documento: %d\n"
                    "  Clase: DESCONOCIDO\n"
                    "  No alcanzo las %d "
                    "coincidencias minimas.\n\n",

                    id_hilo,
                    id_hilo % num_cores,

                    mi_oracion.id_ventana,

                    MIN_COINCIDENCIAS
                );
            }

            pthread_mutex_unlock(
                &doc->mutex
            );

            /*
             * Reducir contador de trabajo pendiente.
             */

            pthread_mutex_lock(
                &g_mutex_docs
            );

            doc->pendiente--;

            pthread_mutex_unlock(
                &g_mutex_docs
            );
        }

        /*
         * Avisar al Loader que este detector
         * terminó su oración.
         */

        pthread_mutex_lock(
            &g_mutex_lote
        );

        g_detectores_completados++;

        if (
            g_detectores_completados >=
            g_parametro_p
        ) {

            pthread_cond_signal(
                &g_cond_lote_terminado
            );
        }

        pthread_mutex_unlock(
            &g_mutex_lote
        );
    }

    return NULL;
}

/* ============================================================
 * HILO LOADER
 * ============================================================ */

static void *hilo_loader(
    void *arg
)
{
    (void)arg;

    while (g_activo) {

        pthread_mutex_lock(
            &g_cola.mutex
        );

        /*
         * Esperar hasta tener P oraciones.
         */

        while (
            g_cola.cantidad_actual <
                g_parametro_p &&
            g_activo
        ) {

            pthread_cond_wait(
                &g_cola.cond_loader,
                &g_cola.mutex
            );
        }

        if (!g_activo) {

            pthread_mutex_unlock(
                &g_cola.mutex
            );

            break;
        }

        pthread_mutex_lock(
            &g_mutex_lote
        );

        g_oraciones_en_lote = 0;
        g_detectores_completados = 0;

        /*
         * Extraer exactamente P oraciones.
         */

        for (
            int i = 0;
            i < g_parametro_p;
            i++
        ) {

            g_lote_trabajo[i] =
                g_cola.buffer[
                    g_cola.frente
                ];

            g_cola.frente =
                (
                    g_cola.frente + 1
                ) % MAX_COLA;

            g_cola.cantidad_actual--;

            g_oraciones_en_lote++;
        }

        imprimir_seguro(
            "[LOADER] Lote de %d "
            "oraciones enviado a los detectores.\n",
            g_parametro_p
        );

        /*
         * Despertar los P detectores.
         */

        pthread_cond_broadcast(
            &g_cond_detectores
        );

        /*
         * Esperar a que los P detectores terminen.
         */

        while (
            g_detectores_completados <
                g_parametro_p &&
            g_activo
        ) {

            pthread_cond_wait(
                &g_cond_lote_terminado,
                &g_mutex_lote
            );
        }

        pthread_mutex_unlock(
            &g_mutex_lote
        );

        pthread_mutex_unlock(
            &g_cola.mutex
        );
    }

    return NULL;
}

/* ============================================================
 * AGREGAR ORACION A COLA
 * ============================================================ */

static int agregar_oracion_cola(
    const char *texto,
    int id_ventana
)
{
    pthread_mutex_lock(
        &g_cola.mutex
    );

    if (
        g_cola.cantidad_actual >=
        MAX_COLA
    ) {

        pthread_mutex_unlock(
            &g_cola.mutex
        );

        return -1;
    }

    strncpy(
        g_cola.buffer[
            g_cola.final
        ].texto,

        texto,

        TAM_MAX_ORACION - 1
    );

    g_cola.buffer[
        g_cola.final
    ].texto[
        TAM_MAX_ORACION - 1
    ] = '\0';

    g_cola.buffer[
        g_cola.final
    ].id_ventana =
        id_ventana;

    g_cola.final =
        (
            g_cola.final + 1
        ) % MAX_COLA;

    g_cola.cantidad_actual++;

    int cantidad =
        g_cola.cantidad_actual;

    /*
     * Despertar Loader solamente cuando
     * ya existen P oraciones.
     */

    if (
        cantidad >=
        g_parametro_p
    ) {

        pthread_cond_signal(
            &g_cola.cond_loader
        );
    }

    pthread_mutex_unlock(
        &g_cola.mutex
    );

    imprimir_seguro(
        "[COLA] Oracion de ventana %d agregada. "
        "Cantidad: %d / P=%d\n",

        id_ventana,
        cantidad,
        g_parametro_p
    );

    return 0;
}

/* ============================================================
 * PROCESAR LINEA DEL PROTOCOLO
 * ============================================================ */

static void procesar_linea(
    const char *linea,
    int *id_ventana,
    char *oracion_local,
    int *pos_oracion
)
{
    if (
        strncmp(
            linea,
            PROTO_ID,
            strlen(PROTO_ID)
        ) == 0
    ) {

        int id =
            atoi(
                linea +
                strlen(PROTO_ID) +
                1
            );

        if (
            id < 1 ||
            id > MAX_VENTANAS
        ) {

            imprimir_seguro(
                "[ERROR] ID de ventana invalido: %d\n",
                id
            );

            return;
        }

        *id_ventana = id;

        if (
            crear_documento(id) < 0
        ) {

            imprimir_seguro(
                "[ERROR] No hay espacio "
                "para la ventana %d.\n",
                id
            );

            *id_ventana = -1;

            return;
        }

        imprimir_seguro(
            "[CONEXION] Ventana %d registrada.\n",
            id
        );
    }

    else if (
        strncmp(
            linea,
            PROTO_CHAR,
            strlen(PROTO_CHAR)
        ) == 0
    ) {

        if (*id_ventana == -1)
            return;

        size_t prefijo =
            strlen(PROTO_CHAR);

        if (
            linea[prefijo] != ' '
        )
            return;

        char c =
            linea[prefijo + 1];

        if (
            *pos_oracion <
            TAM_MAX_ORACION - 1
        ) {

            oracion_local[
                *pos_oracion
            ] = c;

            (*pos_oracion)++;

            oracion_local[
                *pos_oracion
            ] = '\0';
        }
    }

    else if (
        strcmp(
            linea,
            PROTO_RET
        ) == 0
    ) {

        if (*id_ventana == -1)
            return;

        if (*pos_oracion > 0) {

            if (
                agregar_oracion_cola(
                    oracion_local,
                    *id_ventana
                ) == 0
            ) {

                *pos_oracion = 0;

                oracion_local[0] =
                    '\0';
            }
        }
    }

    else if (
        strcmp(
            linea,
            PROTO_FIN
        ) == 0
    ) {

        imprimir_seguro(
            "[CONEXION] Ventana %d envio FIN.\n",
            *id_ventana
        );
    }
}

/* ============================================================
 * HILO DE CONEXION
 * ============================================================ */

static void *hilo_conexion(
    void *arg
)
{
    int fd = *(int *)arg;

    free(arg);

    char buffer[256];

    char acumulador[4096];

    size_t acumulador_len = 0;

    int id_ventana = -1;

    int pos_oracion = 0;

    char oracion_local[TAM_MAX_ORACION];

    memset(
        oracion_local,
        0,
        sizeof(oracion_local)
    );

    while (g_activo) {

        ssize_t nbytes =
            recv(
                fd,
                buffer,
                sizeof(buffer) - 1,
                0
            );

        if (nbytes <= 0)
            break;

        if (
            acumulador_len +
            (size_t)nbytes >=
            sizeof(acumulador) - 1
        ) {

            imprimir_seguro(
                "[ERROR] Buffer TCP "
                "de conexion lleno.\n"
            );

            break;
        }

        memcpy(
            acumulador +
                acumulador_len,

            buffer,

            (size_t)nbytes
        );

        acumulador_len +=
            (size_t)nbytes;

        acumulador[
            acumulador_len
        ] = '\0';

        char *inicio =
            acumulador;

        char *salto;

        while (
            (
                salto =
                    strchr(
                        inicio,
                        '\n'
                    )
            ) != NULL
        ) {

            *salto = '\0';

            if (*inicio != '\0') {

                procesar_linea(
                    inicio,
                    &id_ventana,
                    oracion_local,
                    &pos_oracion
                );
            }

            inicio =
                salto + 1;
        }

        size_t restantes =
            acumulador +
            acumulador_len -
            inicio;

        memmove(
            acumulador,
            inicio,
            restantes
        );

        acumulador_len =
            restantes;

        acumulador[
            acumulador_len
        ] = '\0';
    }

    close(fd);

    if (
        id_ventana != -1
    ) {

        liberar_documento(
            id_ventana
        );
    }

    return NULL;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(
    int argc,
    char *argv[]
)
{
    if (argc != 3) {

        fprintf(
            stderr,
            "Uso: %s <puerto> <P_oraciones>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    char *fin_puerto = NULL;
    char *fin_p = NULL;

    long puerto =
        strtol(
            argv[1],
            &fin_puerto,
            10
        );

    long p =
        strtol(
            argv[2],
            &fin_p,
            10
        );

    if (
        *argv[1] == '\0' ||
        *fin_puerto != '\0' ||
        puerto < 1 ||
        puerto > 65535
    ) {

        fprintf(
            stderr,
            "Puerto invalido.\n"
        );

        return EXIT_FAILURE;
    }

    if (
        *argv[2] == '\0' ||
        *fin_p != '\0' ||
        p < 1 ||
        p > 128
    ) {

        fprintf(
            stderr,
            "P invalido.\n"
        );

        return EXIT_FAILURE;
    }

    g_parametro_p =
        (int)p;

    memset(
        g_documentos,
        0,
        sizeof(g_documentos)
    );

    pthread_mutex_init(
        &g_cola.mutex,
        NULL
    );

    pthread_cond_init(
        &g_cola.cond_loader,
        NULL
    );

    g_cola.frente = 0;
    g_cola.final = 0;
    g_cola.cantidad_actual = 0;

    g_lote_trabajo =
        malloc(
            sizeof(Oracion) *
            g_parametro_p
        );

    if (
        g_lote_trabajo == NULL
    ) {

        fprintf(
            stderr,
            "No se pudo reservar memoria.\n"
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
        manejar_senal;

    sigemptyset(
        &sa.sa_mask
    );

    sa.sa_flags = 0;

    sigaction(
        SIGINT,
        &sa,
        NULL
    );

    sigaction(
        SIGTERM,
        &sa,
        NULL
    );

    signal(
        SIGPIPE,
        SIG_IGN
    );

    pthread_t loader;

    if (
        pthread_create(
            &loader,
            NULL,
            hilo_loader,
            NULL
        ) != 0
    ) {

        fprintf(
            stderr,
            "No se pudo crear el Loader.\n"
        );

        free(g_lote_trabajo);

        return EXIT_FAILURE;
    }

    pthread_t *detectores =
        malloc(
            sizeof(pthread_t) *
            g_parametro_p
        );

    if (
        detectores == NULL
    ) {

        fprintf(
            stderr,
            "No se pudo reservar memoria "
            "para detectores.\n"
        );

        g_activo = 0;

        pthread_cond_broadcast(
            &g_cola.cond_loader
        );

        pthread_cond_broadcast(
            &g_cond_detectores
        );

        pthread_join(
            loader,
            NULL
        );

        free(
            g_lote_trabajo
        );

        return EXIT_FAILURE;
    }

    for (
        int i = 0;
        i < g_parametro_p;
        i++
    ) {

        int *id =
            malloc(
                sizeof(int)
            );

        if (id == NULL) {

            g_activo = 0;

            break;
        }

        *id = i;

        if (
            pthread_create(
                &detectores[i],
                NULL,
                hilo_detector,
                id
            ) != 0
        ) {

            free(id);

            g_activo = 0;

            break;
        }
    }

    g_fd_servidor =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (
        g_fd_servidor < 0
    ) {

        perror("socket");

        g_activo = 0;

        pthread_cond_broadcast(
            &g_cola.cond_loader
        );

        pthread_cond_broadcast(
            &g_cond_detectores
        );

        pthread_join(
            loader,
            NULL
        );

        for (
            int i = 0;
            i < g_parametro_p;
            i++
        ) {

            pthread_join(
                detectores[i],
                NULL
            );
        }

        free(
            detectores
        );

        free(
            g_lote_trabajo
        );

        return EXIT_FAILURE;
    }

    int opt = 1;

    setsockopt(
        g_fd_servidor,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    );

    struct sockaddr_in direccion;

    memset(
        &direccion,
        0,
        sizeof(direccion)
    );

    direccion.sin_family =
        AF_INET;

    direccion.sin_addr.s_addr =
        INADDR_ANY;

    direccion.sin_port =
        htons(
            (uint16_t)puerto
        );

    if (
        bind(
            g_fd_servidor,
            (struct sockaddr *)&direccion,
            sizeof(direccion)
        ) < 0
    ) {

        perror("bind");

        close(
            g_fd_servidor
        );

        g_fd_servidor = -1;

        g_activo = 0;

        pthread_cond_broadcast(
            &g_cola.cond_loader
        );

        pthread_cond_broadcast(
            &g_cond_detectores
        );

        pthread_join(
            loader,
            NULL
        );

        for (
            int i = 0;
            i < g_parametro_p;
            i++
        ) {

            pthread_join(
                detectores[i],
                NULL
            );
        }

        free(
            detectores
        );

        free(
            g_lote_trabajo
        );

        return EXIT_FAILURE;
    }

    if (
        listen(
            g_fd_servidor,
            MAX_VENTANAS
        ) < 0
    ) {

        perror("listen");

        close(
            g_fd_servidor
        );

        g_fd_servidor = -1;

        g_activo = 0;

        pthread_cond_broadcast(
            &g_cola.cond_loader
        );

        pthread_cond_broadcast(
            &g_cond_detectores
        );

        pthread_join(
            loader,
            NULL
        );

        for (
            int i = 0;
            i < g_parametro_p;
            i++
        ) {

            pthread_join(
                detectores[i],
                NULL
            );
        }

        free(
            detectores
        );

        free(
            g_lote_trabajo
        );

        return EXIT_FAILURE;
    }

    imprimir_seguro(
        "Agentic-OS V2 Iniciado "
        "(Puerto %d | P = %d)\n",
        (int)puerto,
        g_parametro_p
    );

    imprimir_seguro(
        "Presiona Ctrl+C para detener "
        "el servidor.\n"
    );

    while (g_activo) {

        struct sockaddr_in cliente;

        socklen_t len =
            sizeof(cliente);

        int fd_cliente =
            accept(
                g_fd_servidor,
                (struct sockaddr *)&cliente,
                &len
            );

        if (
            fd_cliente < 0
        ) {

            if (!g_activo)
                break;

            if (errno == EINTR)
                continue;

            continue;
        }

        int *arg =
            malloc(
                sizeof(int)
            );

        if (arg == NULL) {

            close(
                fd_cliente
            );

            continue;
        }

        *arg =
            fd_cliente;

        pthread_t tid;

        if (
            pthread_create(
                &tid,
                NULL,
                hilo_conexion,
                arg
            ) != 0
        ) {

            free(arg);

            close(
                fd_cliente
            );

            continue;
        }

        pthread_detach(tid);
    }

    g_activo = 0;

    pthread_cond_broadcast(
        &g_cola.cond_loader
    );

    pthread_cond_broadcast(
        &g_cond_detectores
    );

    pthread_cond_broadcast(
        &g_cond_lote_terminado
    );

    pthread_join(
        loader,
        NULL
    );

    for (
        int i = 0;
        i < g_parametro_p;
        i++
    ) {

        pthread_join(
            detectores[i],
            NULL
        );
    }

    if (
        g_fd_servidor >= 0
    ) {

        close(
            g_fd_servidor
        );

        g_fd_servidor = -1;
    }

    pthread_mutex_destroy(
        &g_cola.mutex
    );

    pthread_cond_destroy(
        &g_cola.cond_loader
    );

    pthread_mutex_destroy(
        &g_mutex_lote
    );

    pthread_cond_destroy(
        &g_cond_detectores
    );

    pthread_cond_destroy(
        &g_cond_lote_terminado
    );

    free(
        detectores
    );

    free(
        g_lote_trabajo
    );

    printf(
        "Servidor detenido correctamente.\n"
    );

    return EXIT_SUCCESS;
}
