#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <sys/types.h>

/* Constantes generales del sistema */
#define MAX_VENTANAS      16
#define PUERTO_DEFECTO    9500
#define HOST_DEFECTO      "127.0.0.1"
#define TAM_MAX_MSG       64      
#define TAM_MAX_ORACION   512
#define TAM_MAX_PALABRA   32
#define MAX_VOCABULARIO   64
#define MIN_COINCIDENCIAS 3

/* Prefijos del protocolo (mensajes terminados en '\n') */
#define PROTO_ID     "ID"
#define PROTO_CHAR   "CHAR"
#define PROTO_RET    "RET"
#define PROTO_FIN    "FIN"
#define PROTO_TOTAL  "TOTAL"

/* Clases de documento y tipos de usuario */
typedef enum { CLASE_CORREO = 0, CLASE_ARTICULO, CLASE_REPORTE,
               NUM_CLASES, CLASE_DESCONOCIDA } ClaseDocumento;

typedef enum { USUARIO_ADMINISTRATIVO = 0, USUARIO_TECNICO,
               USUARIO_PROFESOR, USUARIO_ESTUDIANTE,
               USUARIO_INDETERMINADO } TipoUsuario;

/* Estado de un proceso hijo (para el launcher) */
typedef enum { PROC_ACTIVO, PROC_TERMINADO } EstadoProceso;

typedef struct {
    pid_t         pid;
    int           id_ventana;
    EstadoProceso estado;
    int           codigo_salida;
} ProcesoHijo;

#endif
