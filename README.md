# Agentic-OS

**Autor:** _Dario Anchundia_

Sistema compuesto por tres programas en C que simulan un "backdoor" en el API de ventanas X11: cada tecla presionada en una ventana gráfica se envía por socket TCP a un servidor remoto (IALearner), que arma palabras, las clasifica con la técnica *Bag of Words* (Bolsa de Palabras) y, al finalizar todos los procesos, infiere el **contexto de usuario** que puede ser: Personal administrativo, Personal técnico, Profesor o Estudiante.

## Estructura del proyecto

    agentic-OS/
    ├── include/
    │   └── protocolo.h           # Constantes y protocolo compartido
    ├── ia_learner/
    │   └── ia_learner.c          # Servidor multi-hilo (clasificador TCP)
    ├── launcher/
    │   └── launcher.c            # Consola interactiva (crea/monitorea procesos)
    ├── x11_client/
    │   └── x11_client.c          # Cliente gráfico X11 (envía teclas por socket)
    ├── docs/
    │   └── Agentic-OS_DISEÑO.pdf # Diagrama de despliegue y documento de diseño
    └── README.md

## Requisitos

- Linux o WSL con servidor X11 corriendo (echo $DISPLAY no debe estar vacío)
- gcc
- Librería de desarrollo de X11 (libx11-dev en Debian/Ubuntu/WSL)
- Librería pthreads (incluida en glibc, no requiere instalación aparte)

Instalar dependencias en Debian/Ubuntu/WSL si falta X11:

    sudo apt update
    sudo apt install build-essential libx11-dev

## Compilación

Desde la raíz del proyecto (agentic-OS/), ejecuta los siguientes comandos para compilar cada módulo en su respectiva carpeta:

    gcc ia_learner/ia_learner.c -o ia_learner/ia_learner -Wall -Wextra -lpthread
    gcc launcher/launcher.c     -o launcher/launcher     -Wall -Wextra
    gcc x11_client/x11_client.c -o x11_client/x11_client -Wall -Wextra -lX11

Verifica que los tres binarios se hayan generado correctamente:

    ls -la ia_learner/ia_learner launcher/launcher x11_client/x11_client

## Ejecución

El sistema requiere **dos terminales**: una para el servidor IALearner y otra para el launcher.

El servidor recibe dos parámetros:

    ./ia_learner/ia_learner <puerto> <P>

donde:

- `<puerto>` es el puerto TCP donde escuchará el servidor.
- `<P>` es la cantidad de oraciones que deben acumularse antes de formar un lote y enviarlo a los detectores.

### Terminal 1 — IALearner

Por ejemplo, para utilizar el puerto 9500 y procesar lotes de 1 oración:

    ./ia_learner/ia_learner 9500 1

Para trabajar con lotes de 2 oraciones:

    ./ia_learner/ia_learner 9500 2

Para trabajar con lotes de 3 oraciones:

    ./ia_learner/ia_learner 9500 3

El servidor mostrará información sobre:

- Las ventanas conectadas.
- Las oraciones agregadas a la cola.
- Los lotes enviados a los detectores.
- El hilo y CPU utilizados para procesar cada documento.
- La clase detectada.
- Las coincidencias encontradas.
- La frecuencia total acumulada.
- El contexto de usuario inferido.

### Terminal 2 — Launcher

El launcher recibe tres parámetros:

    ./launcher/launcher <N_ventanas> <host> <puerto>

Por ejemplo:

    ./launcher/launcher 1 127.0.0.1 9500

Para lanzar 2 ventanas:

    ./launcher/launcher 2 127.0.0.1 9500

Para lanzar 3 ventanas:

    ./launcher/launcher 3 127.0.0.1 9500

Donde:

- `<N_ventanas>` es la cantidad de ventanas X11 que se crearán.
- `<host>` es la dirección IP del servidor IALearner.
- `<puerto>` debe coincidir con el puerto utilizado por IALearner.

Al ejecutarse, el launcher:

1. Notifica automáticamente al IALearner la cantidad de ventanas que participarán en la ronda.
2. Abre N ventanas gráficas mediante `fork()` + `execv()`.
3. Asigna un identificador único a cada ventana.
4. Despliega un menú interactivo en consola para monitorear o cerrar los procesos.
5. Utiliza `SIGCHLD` para detectar la finalización de los procesos hijos.

### Uso de cada ventana gráfica

- Haz clic sobre la ventana para darle el foco.
- Escribe palabras o frases en inglés; cada tecla imprimible se envía carácter por carácter al servidor.
- La ventana muestra en tiempo real el texto escrito.
- Presiona **Enter** para finalizar una oración.
- Presiona **Escape** para cerrar la ventana.
- Las oraciones de cada ventana se almacenan independientemente para evitar mezclar sus frecuencias y clasificación.

Cuando todas las ventanas lanzadas terminan, el IALearner evalúa las evidencias obtenidas y muestra el **contexto final del usuario**.

## Protocolo interno (IPC vía sockets TCP)

Cada línea enviada por socket termina en un salto de línea. Los mensajes válidos definidos en `protocolo.h` son:

| Mensaje | Dirección | Significado |
|---|---|---|
| `ID <n>` | ventana → IALearner | La ventana n se identifica al conectar |
| `CHAR <c>` | ventana → IALearner | Se presionó la tecla imprimible c |
| `RET` | ventana → IALearner | Se presionó Enter (fin de oración) |
| `FIN` | ventana → IALearner | La ventana se cerró (Escape) |
| `TOTAL <n>` | launcher → IALearner | Avisa cuántas ventanas participarán en la ronda |

El launcher utiliza una conexión TCP corta para enviar `TOTAL`. Cada ventana gráfica mantiene su propia conexión TCP durante toda su ejecución, enviando los caracteres de forma asíncrona al servidor.

## Diccionarios de clasificación (Bag of Words)

| Correo electrónico | Artículo científico | Reporte |
|---|---|---|
| thank, please, regards, meeting, attached, information, update, schedule, team, project | data, analysis, results, method, study, model, research, system, significant, effect | system, data, network, security, application, server, user, performance, service, infrastructure |

Un documento (ventana) se clasifica en una clase si aparecen al menos **3 coincidencias** de su diccionario (controlado por `MIN_COINCIDENCIAS`).

Si un documento califica para más de una clase, se considera la frecuencia total acumulada de las coincidencias para determinar la clase correspondiente.

## Inferencia de tipo de usuario

Una vez terminan todos los procesos esperados, se evalúa qué clases de documento resultaron presentes y se compara contra la tabla de perfiles:

| Tipo de usuario | Correo | Artículo | Reporte |
|---|:---:|:---:|:---:|
| Personal administrativo | X | - | X |
| Personal técnico | X | - | - |
| Profesor | X | X | - |
| Estudiante | - | X | X |

Los cuatro patrones de la tabla representan las combinaciones válidas de evidencias utilizadas para inferir el contexto del usuario.

- **Personal administrativo:** evidencia de Correo y Reporte.
- **Personal técnico:** evidencia únicamente de Correo.
- **Profesor:** evidencia de Correo y Artículo.
- **Estudiante:** evidencia de Artículo y Reporte.

Si las evidencias obtenidas no coinciden exactamente con ninguno de los cuatro patrones definidos, el contexto se muestra como **Indeterminado**.

## Notas de diseño (Concurrencia y Programación Defensiva)

- **IALearner (Servidor):** Acepta múltiples conexiones TCP y atiende cada una en un hilo independiente mediante `pthread_create`.
- **Bag of Words independiente:** Cada ventana mantiene su propio documento y sus propias frecuencias, evitando que las palabras de diferentes ventanas se mezclen.
- **Sincronización:** Los datos compartidos entre hilos se protegen mediante mutex para evitar condiciones de carrera.
- **Procesamiento por lotes:** Las oraciones se almacenan en una cola y se procesan cuando se alcanza el valor `P` configurado al iniciar IALearner.
- **Hilos de detección:** Los documentos son procesados mediante hilos de trabajo y se muestra el hilo y el CPU utilizados durante la clasificación.
- **Consola Segura:** La impresión en el servidor se protege mediante un mutex global para evitar que mensajes de distintos hilos se mezclen en la terminal.
- **Launcher:** Utiliza un manejador de `SIGCHLD` con `waitpid(-1, &estado, WNOHANG)` para recolectar los procesos hijos terminados y evitar procesos zombie.
- **Procesos independientes:** Cada ventana gráfica se ejecuta como un proceso independiente creado mediante `fork()` y `execv()`.

## Pruebas realizadas

El sistema fue probado con diferentes configuraciones para verificar su funcionamiento.

### Prueba con una ventana y P = 1

    ./ia_learner/ia_learner 9500 1
    ./launcher/launcher 1 127.0.0.1 9500

Se verificó:

- Conexión correcta de la ventana.
- Procesamiento de cada oración.
- Clasificación mediante Bag of Words.
- Acumulación de frecuencias.
- Inferencia del contexto de usuario.

### Prueba con múltiples ventanas y P = 3

    ./ia_learner/ia_learner 9500 3
    ./launcher/launcher 3 127.0.0.1 9500

Se verificó:

- Registro de múltiples ventanas.
- Acumulación de P oraciones.
- Procesamiento de lotes.
- Uso de diferentes hilos y CPU.
- Clasificación independiente de los documentos.
- Finalización correcta de todas las ventanas.

### Prueba de independencia entre ventanas

Se utilizaron dos ventanas con múltiples oraciones:

    ./ia_learner/ia_learner 9500 2
    ./launcher/launcher 2 127.0.0.1 9500

Se comprobó que cada ventana conserva su propio **Bag of Words**, frecuencias y clasificación.

Por ejemplo:

    Documento: 1
    Clase: CORREO
    Frecuencia total: 3

    Documento: 1
    Clase: CORREO
    Frecuencia total: 6

mientras otra ventana mantiene sus propios valores:

    Documento: 2
    Clase: REPORTE
    Frecuencia total: 3

    Documento: 2
    Clase: REPORTE
    Frecuencia total: 4

Esto demuestra que las frecuencias de las diferentes ventanas no se mezclan.

## Solución de problemas

### `bind: Address already in use`

Significa que otro proceso está utilizando el puerto seleccionado.

Verifica qué proceso utiliza el puerto:

    lsof -i :9500

Si es necesario, termina el proceso:

    kill -9 <PID>

Después vuelve a iniciar IALearner.

### Launcher dice error de conexión

Asegúrate de iniciar primero el servidor:

    ./ia_learner/ia_learner 9500 1

y después el launcher:

    ./launcher/launcher 1 127.0.0.1 9500

Comprueba que el puerto utilizado en ambos comandos sea el mismo.

### La ventana gráfica no aparece

Confirma que existe un servidor X11 disponible:

    echo $DISPLAY

El comando no debe devolver una cadena vacía.

En WSL puede ser necesario configurar un servidor X11 como VcXsrv y establecer correctamente la variable `DISPLAY`.

### El contexto aparece como `Indeterminado`

Esto ocurre cuando las evidencias globales obtenidas no coinciden con ninguno de los cuatro patrones definidos:

| Correo | Artículo | Reporte | Resultado |
|:---:|:---:|:---:|---|
| X | - | X | Personal administrativo |
| X | - | - | Personal técnico |
| X | X | - | Profesor |
| - | X | X | Estudiante |

Por ejemplo:

    Correo: SI
    Articulo: SI
    Reporte: SI

no coincide con ningún patrón y, por lo tanto, el resultado es:

    Contexto: Indeterminado
