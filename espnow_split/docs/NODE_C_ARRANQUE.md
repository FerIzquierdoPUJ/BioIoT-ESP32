# Reinicio previo a setup del nodo C

Revisión del 2026-10-05, sobre `espnow-split-wifi-littlefs`, base `2345018`.
Placa confirmada por el usuario en COM3, actuadores físicamente desconectados.
El flasher identifica ESP32-D0WD-V3, revisión 3.1, MAC STA
`38:3E:51:D4:30:1C`. Se conserva commissioning y no se habilitan GPIO de cargas.

## Causa demostrada

Se reprodujo en COM3 el assertion original doce veces: ningún banner, ninguna
respuesta a `pairing_info` ni a `status`. Se encontró el ELF exacto en la caché
de Arduino, SHA256
`a5bd1e6bf12b193d3a4201905ba15980620d7111e2afd139a9709bc31855bb39`.

Arduino-ESP32 instalado: **3.3.12**, ESP-IDF **5.5.5**, commit **b774170ff46**,
según `esp32-libs/3.3.12/versions.txt`. Se descargó el archivo del commit exacto:
[app_startup.c](https://raw.githubusercontent.com/espressif/esp-idf/b774170ff46/components/freertos/app_startup.c).
Sus líneas 83–85 crean `main` con `xTaskCreatePinnedToCore`; la 86 comprueba
`res == pdTRUE`. El desensamblado del ELF original confirma esa misma llamada,
la pila de 4608 bytes y el salto al assertion al devolver un resultado distinto
de 1. Los constructores globales se ejecutan antes de crear esa tarea; parte de
la RAM de arranque se incorpora al heap dentro de `main`, después de crearla.
Referencias: [startup.c](https://raw.githubusercontent.com/espressif/esp-idf/b774170ff46/components/esp_system/startup.c)
y el archivo de FreeRTOS anterior.

Backtrace decodificado con `xtensa-esp32-elf-addr2line`, **usando ese ELF**:

| Dirección | Símbolo / archivo:línea |
|---|---|
| `0x4008c204` | `panic_abort`, `panic.c:477` |
| `0x4008c1c9` | `esp_system_abort`, `esp_system_chip.c:87` |
| `0x400927e5` | `__assert_func`, `assert.c:81` |
| `0x401b0fd6` | `esp_startup_start_app`, `app_startup.c:86` |
| `0x40104ce6` | `start_cpu0_default`, `startup.c:222` |
| `0x40081cae` | `call_start_cpu0`, `cpu_start.c:985` |
| `0x40079912` | Sin símbolo/línea en este ELF; no se atribuye una función |

El marcador `CORRUPTED` del monitor no demuestra por sí solo corrupción del
heap. El punto de fallo sí queda identificado.

## Tamaños y reservas

Tamaños del ABI ESP32 extraídos de DWARF y contrastados con el constructor
desensamblado y `nm -S --size-sort`. No son tamaños de un PC de 64 bits.

| Objeto o recurso | Bytes | Cuándo se reserva |
|---|---:|---|
| `GatewayState` | **47264** | Antes: `new` global; ahora: `new (std::nothrow)` en setup |
| JSON de publicación `g_pub` | **24576** | Antes: `malloc` global; ahora: en setup |
| `AzureLink` | 228 | Objeto estático; no reserva un estado grande |
| `EspNowPort` | 4344 | Objeto estático; su constructor no inicia ESP-NOW |
| `NetworkClientSecure` | 100 | Objeto estático de TLS |
| `sslclient_context` | 2096 | Su constructor de biblioteca hace `new`, más el control de `shared_ptr` |
| `PubSubClient` | 96 | Objeto estático; constructor reserva un buffer inicial de 256 |
| Buffer interno de PubSubClient | **24576** | Otra reserva, distinta de `g_pub`, al iniciar Azure en setup |
| `AzIoTSasToken` | 40 | Objeto estático, referencias a buffers estáticos |
| `Endpoint` | 6744 | Estático local al habilitar radio; las plantillas eliminan esa ruta |
| `Request` | 568 | La cola conserva cuatro: 2272, más metadatos FreeRTOS |
| Pila IDF `main` | **4608** | 4096 de sdkconfig + 512 de `TASK_EXTRA_STACK_SIZE` |
| Pila Arduino `loopTask` | **8192** | Antes de setup; no se reduce |
| Pila `bioiot_local` | **8192** | En setup, comprobando `pdPASS`; no se reduce |

Las dos reservas grandes globales solicitaban **71840 bytes**, además de los
constructores de TLS/MQTT, objetos de bibliotecas y metadatos del asignador.
Estos últimos explican que el descenso observado no sea exactamente igual al
tamaño solicitado. No se declara haber contado todas las reservas internas de
todas las bibliotecas. Los objetos propios `AzureLink`, actuadores y radio no
añaden una reserva global comparable a la de `GatewayState`.

El ELF que fallaba tiene `.dram0.data=31426` y `.dram0.bss=27904` (59330 bytes,
antes del redondeo del resumen Arduino). Los mayores objetos RAM de `nm` son:
`g_port` 4344, `port_IntStack` 4192, `g_cnxMgr` 3880, `s_wifi_nvs` 1308,
`g_actuators` 1232, `dns_table` 1184 y `TxRxCxt` 972. Se verificaron también las
secciones y los objetos propios en el `.map`. Los 47264 bytes del estado **no**
aparecen como objeto estático: el constructor global pide `0xb8a0` a `operator new`.

## Medición antes de main

Se compiló y cargó temporalmente una copia del código anterior con plantillas,
sin credenciales reales, instrumentada con `esp_rom_printf`. No se usó Serial
antes del scheduler. Un wrapper del linker midió la llamada a crear `main`.
Se reprodujo el mismo assertion; esta variante tiene otro ELF, `b055f5326…`.
Estas cifras pertenecen a la variante instrumentada, no a una supuesta lectura
retrospectiva del ELF original.

Capacidades: `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`; todos los valores en bytes.

| Etapa anterior | Libre | Bloque máximo | Mínimo |
|---|---:|---:|---:|
| Antes de `GatewayState` | 92892 | 77812 | 92892 |
| Después del estado | 43736 | 30708 | 43736 |
| Después de `g_pub` | 18132 | 11252 | 18132 |
| Justo antes de crear `main` | **5052** | **4852** | 5028 |
| Después del intento fallido | 5052 | 4852 | **184** |

La tarea solicitó 4608 bytes de pila y devolvió **-1**. Necesita además TCB y
metadatos de asignación. La memoria libre total era insuficiente para la
creación completa. La diferencia libre–bloque máximo era solamente 200 bytes:
hay separación de bloques, pero **no se demostró fragmentación importante como
causa dominante**. Se demostró agotamiento del heap disponible en esa fase.

## Corrección y modo seguro

`g_state` y `g_pub` empiezan como punteros nulos. `StartupResources.h` ejecuta
la secuencia comprobada de reservas en setup: estado, publicación, mutex,
cola y tarea. No se trasladan los 47 KB a `.bss` y no se reduce ningún buffer.
La tarea local espera una notificación; solo empieza a usar estado/radio/salidas
cuando setup termina la inicialización local. Se libera antes de iniciar Azure.

Si fallan estado, mutex, cola o tarea: se informa el recurso, se liberan las
reservas propias ya realizadas y setup retorna antes de configurar GPIO/redes.
El loop permanece en modo seguro, sin watchdog añadido ni reinicio automático:
`status` muestra `safe_failed`, los comandos operativos se rechazan y
`pairing_info` obtiene la MAC desde eFuse sin necesitar Wi-Fi. No se accede a un
estado, mutex o cola nulos. No se inicializan parcialmente Azure ni ESP-NOW.

Si falla solo `g_pub`: continúa la tarea local, estado, radio y Serial; Azure
no se inicializa para publicar y se informa la razón. Los diagnósticos locales
se ejecutan, pero su JSON agregado no puede generarse sin ese buffer. El estado
expone `publish_buffer_ready` y `azure_enabled`. También se comprueba el `new`
del portal; no se anuncia abierto si no existe realmente.

`MemoryDiagnostics.h` imprime libre, bloque máximo y mínimo internos a la entrada
de setup, tras cada reserva, Wi-Fi, ESP-NOW, MQTT y antes/después de TLS. `status`
y la telemetría conservan el mínimo de heap. La MAC se obtiene sin esperar a que
la STA termine de arrancar; se corrigió el mensaje transitorio de MAC cero.

Archivos funcionales: `NodeC.cpp`, `AzureLink.cpp/.h`, `StartupResources.h`,
`MemoryDiagnostics.h`. Pruebas: `tests/host/test_startup.cpp` (seis casos).
No se modificaron A, B, el integrado, los encabezados locales de secretos ni las
CA/claves de Azure. No se generó un certificado nuevo: este cambio de memoria
conserva el cliente SAS y la CA existentes.

## Validación

Compilador: ESP32 Dev Module, Arduino-ESP32 3.3.12,
`PartitionScheme=min_spiffs`, sin PSRAM, commissioning habilitado.
Los encabezados locales ignorados se usan únicamente por el compilador, sin
mostrar su contenido; la copia de plantillas excluye esos archivos.

Las **83 pruebas C++** pasan, incluyendo seis casos nuevos: éxito y fallos de
estado, buffer, mutex, cola y tarea. Pasan las seis suites JavaScript existentes
de calibración, contrato, diagnósticos, tamaño y color. `git diff --check` pasa.

Las políticas portables verifican Wi-Fi/hotspot/portal, almacenamiento B,
criptografía, ACK/reintentos/replay, colas, instantáneas y actuadores. Se conservan
40 instantáneas (~80 minutos), TIME_SYNC, diagnóstico, comandos, operación
offline, WiFiManager AP+STA y recuperación de canal/BSSID. No se confunde esta
cobertura de PC con una prueba física de A/B o de IoT Hub.

Antes de cargar se leyó únicamente la tabla de particiones de la placa:
coincide byte a byte con la de Minimal SPIFFS. NVS ocupa `0x9000–0xdfff` y
no se escribió ese rango. Se usó `EraseFlash=none`: carga normal de firmware,
tabla idéntica y metadatos de arranque OTA; no borrado total ni formato de FS.

La primera prueba del arreglo mostró banner, `startup_state=ready`,
`publish_buffer_ready=true`, commissioning y la respuesta:

```json
{"node":"gateway","sta_mac":"38:3E:51:D4:30:1C","espnow_channel":0}
```

Falta `gateway_secrets.h` local: ESP-NOW permanece deshabilitado explícitamente.
El encabezado local de Azure compila y habilita su cliente, pero el nodo abre
el portal; no se verificó una conexión real MQTT/TLS ni recepción en IoT Hub.
La autoprueba de protocolo da OK. No se inventan mediciones TLS si no hay enlace.

Resultados finales de compilación:

| Variante | Flash usada | RAM estática |
|---|---:|---:|
| Antes, con plantillas | 1239133 | 59332 |
| Arreglo final, con plantillas | 1241989 | 59340 |
| Arreglo final, con `iot_configs.h` local ignorado | 1245673 | 59340 |

Comparación con plantillas: **+2856 bytes de flash y +8 de RAM estática**.
Los ~47 KB de estado siguen siendo dinámicos. SHA256 del ELF final local cargado:
`b5fe7fae07df6f355186afd86c3167702e5f1c794f2429c06e0ab09de238fcd6`.

Heap de un arranque de esa versión final, con Azure configurado y portal abierto:

| Etapa | Libre interno | Bloque máximo | Mínimo interno |
|---|---:|---:|---:|
| Entrada a setup | **224664** | 110580 | 192652 |
| Estado asignado | 175508 | 110580 | 149100 |
| Buffer de publicación asignado | 149904 | 86004 | 123496 |
| Mutex creado | 149796 | 86004 | 123496 |
| Cola creada | 147232 | 86004 | 123496 |
| Tarea local creada, esperando notificación | 138136 | 86004 | 123496 |
| Wi-Fi inicializado | 87372 | 86004 | 85484 |
| Paso ESP-NOW, deshabilitado por plantillas | 87004 | 86004 | 85452 |
| Antes de inicializar MQTT | 86756 | 86004 | 85452 |
| Después de inicializar MQTT, buffer interno 24 KB | 61444 | 59380 | 61144 |
| Wi-Fi/portal iniciado | 48264 | 47092 | 48128 |
| Consulta status | 48136 | 45044 | 47012 |

Se ejecutaron **diez reinicios eléctricos de EN mediante RTS**, manteniendo
GPIO0 libre. El monitor confirma `POWERON_RESET` y `SPI_FAST_FLASH_BOOT`:
**10 banners, 10 respuestas pairing, 10 estados ready, 0 assertions**.
No fueron llamadas a `ESP.restart()`, ni se afirma haber hecho diez cortes de
alimentación o diez pulsaciones manuales del botón. Cada arranque se observó
unos cinco segundos. La primera secuencia de prueba entró en modo descarga;
se corrigió antes de contar los diez arranques válidos.

En los diez arranques: `publish_buffer_ready=true`, `azure_enabled=true`,
`actuation=commissioning_simulated`, MAC correcta y portal activo. Nueve
consultas mostraron libre 48136/mínimo 47012; una 48124/47000. Bloque máximo
45044 en todas. La carga final escribió **solo app0 en `0x10000`**, verificó
su hash y dejó ese firmware ejecutándose. Los GPIO de actuadores conservan el
camino de alta impedancia de commissioning; no se midieron eléctricamente.

**Pendiente:** provisionar el emparejamiento real A/B, verificar su enlace cifrado
en hardware, configurar/conectar el Wi-Fi desde el portal y medir el heap durante
el handshake TLS y la publicación real a IoT Hub. Los logs de esos pasos ya
están instrumentados. Las inyecciones de fallos de reserva se comprobaron en PC,
no mediante agotamiento artificial de memoria de la placa. La prueba de arranque
es satisfactoria; esta revisión no certifica estabilidad prolongada ni todos los
escenarios de producción.
