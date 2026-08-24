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

El sistema requiere **dos terminales**: una para el servidor y otra para el launcher.

El IALearner recibe dos parámetros:

    ./ia_learner/ia_learner <puerto> <P_oraciones>

donde:

- `<puerto>` es el puerto TCP en el que escuchará el servidor.
- `<P_oraciones>` es el tamaño de lote utilizado por el patrón Productor-Consumidor.
- `P_oraciones` debe ser un valor entero entre 1 y 128.

**Terminal 1 — servidor:**

    ./ia_learner/ia_learner 9500 2

Ejemplo:

    Agentic-OS V2 Iniciado (Puerto 9500 | P = 2)
    Presiona Ctrl+C para detener el servidor.

**Terminal 2 — launcher:**

    ./launcher/launcher <N_ventanas> [host] [puerto]

donde:

- `<N_ventanas>` indica cuántas ventanas gráficas se crearán.
- `N_ventanas` debe ser mayor o igual a `MAX_VENTANAS`.
- `MAX_VENTANAS` está definido actualmente como 16 en `protocolo.h`.
- `[host]` es opcional y por defecto utiliza `127.0.0.1`.
- `[puerto]` es opcional y por defecto utiliza `9500`.

Ejemplos:

    ./launcher/launcher 16
    ./launcher/launcher 16 127.0.0.1 9500

Al ejecutarse, el launcher:

1. Notifica automáticamente `TOTAL <n>` al IALearner.
2. Abre N ventanas gráficas mediante `fork()` + `execv()`.
3. Mantiene un menú interactivo en consola para monitorear o cerrar los procesos.
4. Utiliza `SIGCHLD` y `waitpid()` para detectar y recolectar los procesos hijos terminados.

## Uso de cada ventana gráfica

- Haz clic sobre la ventana para darle el foco.
- Escribe palabras en inglés libremente; cada tecla imprimible se envía carácter por carácter al servidor.
- La ventana dibuja en tiempo real los caracteres escritos.
- Presiona **Enter** para enviar la oración completa y limpiar la interfaz para comenzar una nueva.
- Presiona **Escape** para cerrar la ventana.
- El launcher detecta el cierre mediante `SIGCHLD` y recolecta el proceso hijo para evitar procesos zombie.

> **Nota sobre Backspace:** La tecla Backspace solo modifica visualmente el texto mostrado en la ventana. No se envía un evento de Backspace al IALearner, por lo que el servidor conserva los caracteres previamente enviados mediante `CHAR`. Por esta razón, el texto visual de la ventana puede diferir del texto que finalmente procesa el servidor.

Cuando se completan las condiciones de procesamiento, el IALearner evalúa las frecuencias obtenidas y muestra el contexto de usuario inferido.

## Protocolo interno (IPC vía sockets TCP)

Cada línea enviada por socket termina en un salto de línea. Los mensajes válidos definidos en `protocolo.h` son:

| Mensaje | Dirección | Significado |
|---|---|---|
| `ID <n>` | ventana → IALearner | La ventana n se identifica al conectar |
| `CHAR <c>` | ventana → IALearner | Se presionó la tecla imprimible c |
| `RET` | ventana → IALearner | Se presionó Enter (fin de oración) |
| `FIN` | ventana → IALearner | La ventana se cerró (Escape) |
| `TOTAL <n>` | launcher → IALearner | Avisa cuántas ventanas esperar en la ronda |

El launcher utiliza una conexión TCP corta para enviar `TOTAL` e inicializar el estado del servidor.

Cada ventana gráfica mantiene su propia conexión TCP durante toda su ejecución, enviando los mensajes `CHAR`, `RET` y `FIN`.

El servidor atiende cada conexión mediante un hilo independiente utilizando `pthread_create()`.

## Diccionarios de clasificación (Bag of Words)

| Correo electrónico | Artículo científico | Reporte |
|---|---|---|
| thank, please, regards, meeting, attached, information, update, schedule, team, project | data, analysis, results, method, study, model, research, system, significant, effect | system, data, network, security, application, server, user, performance, service, infrastructure |

Un documento se clasifica en una clase si aparecen al menos **3 coincidencias** de su diccionario, controlado mediante `MIN_COINCIDENCIAS`.

Si un documento califica para más de una clase, se asigna la clase que tenga la **mayor frecuencia total acumulada**.

## Procesamiento por lotes

El IALearner implementa un patrón **Productor-Consumidor**.

Las oraciones recibidas desde las ventanas se almacenan en una cola compartida.

El hilo **Loader** espera hasta disponer de `P` oraciones:

    P = parámetro recibido al iniciar el IALearner

Cuando la cola alcanza `P` elementos:

1. El Loader extrae exactamente `P` oraciones.
2. Las coloca en el lote de trabajo.
3. Despierta a los hilos detectores.
4. Cada detector procesa una oración.
5. El Loader espera a que los `P` detectores terminen.
6. Se continúa con el siguiente lote.

La cola utiliza:

- `pthread_mutex_t` para proteger el acceso concurrente.
- `pthread_cond_t` para despertar al Loader cuando hay suficientes oraciones.
- Un buffer circular de tamaño `MAX_COLA`.

## Inferencia de tipo de usuario

Una vez procesadas las oraciones, se determina qué clases de documento fueron detectadas y se compara la evidencia global contra la siguiente tabla:

| Tipo de usuario | Correo | Artículo | Reporte |
|---|---|---|---|
| Personal administrativo | X | | X |
| Personal técnico | X | | |
| Profesor | X | X | |
| Estudiante | | X | X |

Los patrones utilizados son:

- **Personal administrativo:** Correo + Reporte
- **Personal técnico:** Correo
- **Profesor:** Correo + Artículo
- **Estudiante:** Artículo + Reporte

Si la combinación de evidencia no coincide con ninguno de estos patrones, el sistema determina el usuario como **Indeterminado**.

> **Nota de diseño:** Las evidencias de cada clase se almacenan de forma binaria: una clase cuenta como presente cuando al menos un documento ha sido clasificado con dicha etiqueta.

## Concurrencia y programación defensiva

### IALearner

- Acepta múltiples conexiones TCP.
- Cada conexión es atendida mediante un hilo independiente.
- Los detectores trabajan concurrentemente utilizando `pthread`.
- Cada documento posee su propio `pthread_mutex_t`, protegiendo su Bolsa de Palabras y sus resultados.
- La cola de oraciones posee un mutex y una variable de condición.
- El Loader utiliza `pthread_cond_wait()` para evitar espera activa.
- Los detectores utilizan variables de condición para sincronizar el procesamiento de cada lote.
- La salida por consola está protegida mediante `g_mutex_consola`, evitando que varios hilos mezclen sus mensajes.
- Los hilos detectores se asocian a CPUs mediante `pthread_setaffinity_np()` cuando el sistema lo permite.
- Se utilizan límites estáticos definidos en `protocolo.h` para controlar el uso de memoria.

### Launcher

- Utiliza `fork()` + `execv()` para crear cada ventana.
- Utiliza `SIGCHLD` para detectar procesos hijos terminados.
- Utiliza `waitpid(-1, &estado, WNOHANG)` para recolectar procesos terminados sin bloquear el menú.
- Permite cerrar todas las ventanas mediante `SIGTERM`.
- Ignora `SIGPIPE` para evitar la terminación inesperada ante conexiones cerradas.

### Cliente X11

- Utiliza Xlib para crear las ventanas.
- Captura eventos de teclado mediante `KeyPressMask`.
- Envía las teclas imprimibles al servidor mediante mensajes `CHAR`.
- Mantiene un búfer local para mostrar visualmente el texto escrito.
- Permite borrar caracteres visualmente mediante Backspace.
- Utiliza `SIGPIPE` ignorado y `MSG_NOSIGNAL` al enviar datos por socket.

## Límites del sistema

Los principales límites están definidos en `protocolo.h`:

| Constante | Valor | Descripción |
|---|---:|---|
| `MAX_VENTANAS` | 16 | Límite de ventanas/procesos soportados |
| `TAM_MAX_MSG` | 64 | Tamaño máximo de un mensaje del protocolo |
| `TAM_MAX_ORACION` | 512 | Tamaño máximo de una oración |
| `TAM_MAX_PALABRA` | 32 | Tamaño máximo de una palabra |
| `MAX_VOCABULARIO` | 64 | Máximo de palabras almacenadas por documento |
| `MAX_COLA` | 1000 | Capacidad máxima de la cola de oraciones |
| `MIN_COINCIDENCIAS` | 3 | Coincidencias mínimas para clasificar un documento |

El parámetro `P` del IALearner debe estar entre **1 y 128**.

## Solución de problemas

### `bind: Address already in use`

Quedó un servidor ejecutándose en segundo plano.

Verifica qué proceso está utilizando el puerto:

    lsof -i :9500

Luego puedes finalizarlo:

    kill -9 <PID>

### `Launcher dice error de conexión`

Asegúrate de levantar primero el IALearner:

    ./ia_learner/ia_learner 9500 2

Después ejecuta el launcher:

    ./launcher/launcher 16

También verifica que el host y puerto utilizados por ambos programas coincidan.

### `N_ventanas inválido`

El launcher requiere que el número de ventanas sea **mayor o igual a `MAX_VENTANAS`**.

Actualmente:

    MAX_VENTANAS = 16

Por lo tanto, utiliza:

    ./launcher/launcher 16

o un valor superior.

### `P inválido`

El segundo parámetro del IALearner debe encontrarse entre 1 y 128:

    ./ia_learner/ia_learner 9500 2

### La ventana gráfica no aparece

Confirma que tienes un servidor X11 funcionando:

    echo $DISPLAY

El resultado no debe estar vacío.

En Windows/WSL puedes utilizar VcXsrv y configurar correctamente la variable `DISPLAY`.

### El texto visual no coincide con el texto procesado

Esto puede ocurrir si se utiliza **Backspace**.

El Backspace actualmente solamente modifica el texto mostrado en la ventana y no envía ninguna instrucción al IALearner para eliminar el carácter previamente enviado.

Por lo tanto, el servidor puede conservar un carácter que visualmente ya no aparece en la ventana.
