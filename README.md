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

El sistema requiere **dos terminales**: una para el servidor y otra para el launcher. El orquestador (launcher) se encarga de avisar el total de ventanas al servidor y levantar los procesos gráficos.

**Terminal 1 — servidor (arrancar siempre primero):**

    ./ia_learner/ia_learner

Por defecto escucha en el puerto 9500. Puede indicarse otro puerto pasándolo como argumento:

    ./ia_learner/ia_learner 9500

**Terminal 2 — launcher:**

    ./launcher/launcher <N_ventanas> [host] [puerto]

Ejemplos:

    ./launcher/launcher 1                 # 1 ventana, servidor local, puerto 9500
    ./launcher/launcher 3                 # 3 ventanas simultáneas
    ./launcher/launcher 2 127.0.0.1 9500  # host y puerto explícitos

Al ejecutarse, el launcher:
1. Notifica automáticamente TOTAL <n> al IALearner.
2. Abre N ventanas gráficas mediante fork() + execv().
3. Despliega un menú interactivo en consola para monitorear o cerrar los procesos.

**Uso de cada ventana gráfica:**

- Haz clic sobre la ventana para darle el foco (verás instrucciones en pantalla).
- Escribe palabras en inglés libremente; cada tecla se envía carácter por carácter al servidor. La ventana dibuja en tiempo real lo que escribes.
- Presiona **Enter** para enviar la oración completa (limpia la interfaz para una nueva oración).
- Presiona **Escape** para cerrar la ventana; el launcher detecta el cierre vía la señal SIGCHLD sin dejar procesos zombie.

Cuando **todas** las ventanas lanzadas terminan, el IALearner evalúa las sumatorias de frecuencias y muestra en su terminal el contexto de usuario inferido.

## Protocolo interno (IPC vía sockets TCP)

Cada línea enviada por socket termina en un salto de línea. Los mensajes válidos definidos en protocolo.h son:

| Mensaje | Dirección | Significado |
|---|---|---|
| ID <n> | ventana → IALearner | La ventana n se identifica al conectar |
| CHAR <c> | ventana → IALearner | Se presionó la tecla imprimible c |
| RET | ventana → IALearner | Se presionó Enter (fin de palabra/oración) |
| FIN | ventana → IALearner | La ventana se cerró (Escape) |
| TOTAL <n> | launcher → IALearner | Avisa cuántas ventanas esperar en la ronda |

El launcher usa una conexión TCP corta solo para enviar TOTAL e inicializar el estado del servidor. Cada ventana gráfica mantiene su propia conexión abierta durante toda su ejecución, enviando ráfagas asíncronas de CHAR para no bloquear el hilo de dibujado gráfico.

## Diccionarios de clasificación (Bag of Words)

| Correo electrónico | Artículo científico | Reporte |
|---|---|---|
| thank, please, regards, meeting, attached, information, update, schedule, team, project | data, analysis, results, method, study, model, research, system, significant, effect | system, data, network, security, application, server, user, performance, service, infrastructure |

Un documento (ventana) se clasifica en una clase si aparecen al menos **3 coincidencias** de su diccionario (controlado por MIN_COINCIDENCIAS). Si califica para más de una clase, se asigna la de mayor frecuencia total acumulada.

## Inferencia de tipo de usuario

Una vez terminan todos los procesos esperados, se evalúa qué clases de documento resultaron ganadoras en cada ventana y se compara contra la tabla de perfiles:

| Tipo de usuario | Correo | Artículo | Reporte |
|---|---|---|---|
| Personal administrativo | X | | |
| Personal técnico | X | | X |
| Profesor | X | X | |
| Estudiante | | X | X |

> **Nota de diseño:** Los cuatro patrones de la tabla son combinaciones binarias mutuamente excluyentes. Se interpretó cada X como presencia binaria (al menos un documento clasificado con esa etiqueta) en la ronda activa.

## Notas de diseño (Concurrencia y Programación Defensiva)

- **IALearner (Servidor):** Acepta múltiples conexiones TCP y atiende cada una en un hilo independiente (pthread_create). Cada ventana tiene su propio espacio de memoria protegido por su propio pthread_mutex_t en la tabla de documentos, evitando condiciones de carrera al sumar frecuencias.
- **Sincronización:** Un hilo clasificador se mantiene en bucle infinito usando pthread_cond_wait. Esto evita la espera activa (ahorrando CPU). Solo despierta cuando el contador de ventanas terminadas coincide con el TOTAL esperado.
- **Consola Segura:** Toda impresión en el servidor (printf) pasa por un mutex global (g_mutex_consola) para evitar que los caracteres de distintos hilos se sobreescriban en la terminal.
- **Launcher:** Utiliza un manejador de SIGCHLD con la llamada no bloqueante waitpid(-1, &estado, WNOHANG) dentro de un while. Esto recolecta de inmediato los recursos de los procesos hijos apenas el usuario presiona Escape, garantizando **cero procesos zombies** sin congelar el menú interactivo.

## Solución de problemas

- **bind: Address already in use**: Quedó un servidor ejecutándose en segundo plano. Verifica con `lsof -i :9500` y mata el proceso con `kill -9 <PID>` antes de reiniciar.
- **Launcher dice error de conexión**: Asegúrate de levantar primero el servidor en la Terminal 1 antes de usar el launcher. Si falla, el launcher avisará pero seguirá funcionando en modo local.
- **La ventana gráfica no aparece**: Confirma que tienes un servidor X11 corriendo (en Windows/WSL, instala VcXsrv y asegúrate de configurar export DISPLAY=:0).
