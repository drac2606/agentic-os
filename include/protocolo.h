#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <pthread.h>
#include <sys/types.h>

/* ─── CONSTANTES DEL SISTEMA ───
   Definimos los límites de memoria de forma estática para evitar desbordamientos. */
#define MAX_VENTANAS      16
#define PUERTO_DEFECTO    9500
#define HOST_DEFECTO      "127.0.0.1"
#define TAM_MAX_MSG       64      
#define TAM_MAX_ORACION   512
#define TAM_MAX_PALABRA   32
#define MAX_VOCABULARIO   64
#define MIN_COINCIDENCIAS 3

/* ─── PROTOCOLO DE CAPA DE APLICACIÓN (IPC) ─── */
#define PROTO_ID     "ID"
#define PROTO_CHAR   "CHAR"
#define PROTO_RET    "RET"
#define PROTO_FIN    "FIN"
#define PROTO_TOTAL  "TOTAL"

/* ─── ESTRUCTURAS DE DATOS COMPARTIDAS (V1) ─── */
typedef enum { CLASE_CORREO = 0, CLASE_ARTICULO, CLASE_REPORTE, NUM_CLASES, CLASE_DESCONOCIDA } ClaseDocumento;
typedef enum { USUARIO_ADMINISTRATIVO = 0, USUARIO_TECNICO, USUARIO_PROFESOR, USUARIO_ESTUDIANTE, USUARIO_INDETERMINADO } TipoUsuario;
typedef enum { PROC_ACTIVO, PROC_TERMINADO } EstadoProceso;

typedef struct {
    pid_t         pid;
    int           id_ventana;
    EstadoProceso estado;
    int           codigo_salida;
} ProcesoHijo;

/* ─── NUEVAS ESTRUCTURAS V2 (PROCESAMIENTO POR LOTES) ─── 
   Para implementar el patrón Productor-Consumidor exigido en la V2. */
typedef struct {
    char texto[TAM_MAX_ORACION];
    int id_ventana; /* Para saber a qué documento pertenece esta oración */
} Oracion;

#define MAX_COLA 1000
typedef struct {
    Oracion buffer[MAX_COLA];
    int frente;
    int final;
    int cantidad_actual;
    pthread_mutex_t mutex;       /* Protege el acceso concurrente a la cola */
    pthread_cond_t cond_loader;  /* Señal para despertar al hilo Loader */
} ColaOraciones;

#endif
