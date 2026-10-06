# Revisión de despliegue de espnow_split

Fecha: 2026-10-05. Referencia: copia de trabajo del firmware integrado, código de
los tres nodos y salida adjunta del agente anterior. Esta revisión no programa
placas, no activa salidas y no cambia recursos ni credenciales de Azure.

Nota posterior: la petición de corrección de color del mismo día sí actualiza el
integrado y A. Sus referencias y validación se documentan en [COLOR.md](COLOR.md);
las cifras y pruebas de este informe corresponden a la revisión previa.

## Conclusión

La distribución de sensores y comandos existe, pero **no es una sustitución
idéntica ni está lista para producción con los valores de ejemplo**. Hay
bloqueos de configuración y riesgos concretos de persistencia y compatibilidad
del consumidor. Compilar no verifica el enlace cifrado ESP8266–ESP32, el cableado,
los niveles de potencia ni la recepción en Cosmos DB.

## Cómo se comunican realmente

```text
A: ESP32, 9 sensores  <--- ESP-NOW cifrado ---> C: ESP32 gateway + actuadores
B: ESP8266, 4 sensores <--- ESP-NOW cifrado ---> C
                                               |
                                        Wi-Fi STA, 2,4 GHz
                                               |
                                            router
                                               |
                                      MQTT/TLS, TCP 8883
                                               |
                                          Azure IoT Hub
```

A y B inicializan WiFi en modo STA, desactivan la reconexión y llaman a
`WiFi.disconnect()`. **No se asocian al router y no necesitan dirección IP,
SSID ni contraseña para hablar con C.** El generador pone el SSID y la
contraseña únicamente en `gateway_secrets.h` de C.

ESP-NOW transmite directamente entre MAC STA. El router no retransmite esos
mensajes. Estar cerca del mismo router o usar el mismo SSID no sustituye la
condición necesaria: las radios deben estar en el mismo canal. Una vez C se
conecta al router, su radio adopta el canal del router. A/B buscan ese canal
si pierden el enlace. Un router de 2,4 GHz con canal fijo reduce interrupciones;
los barridos y cambios de canal del gateway pueden interrumpir el transporte.

Si se modifica A/B para asociarlos también al Wi-Fi, hay que revisar la búsqueda
de canal: `setChannel()` rechaza cambios mientras están asociados. En redes mesh,
el mismo SSID puede existir en puntos de acceso con canales distintos.

Los enlaces implementados son A↔C y B↔C; no hay un enlace A↔B. C agrega los datos
y enruta los comandos hacia el nodo propietario. A/B reportan cada 5 s; C toma
instantáneas para Azure cada 120 s. B conserva la muestra O2 cada 120 s y el filtro
de diez muestras. C sincroniza el reloj de los nodos por `TIME_SYNC`.

| Tramo | Protocolos y requisitos |
| --- | --- |
| A/B ↔ C | ESP-NOW unicast; MAC STA reales; canal común; PMK, LMK por enlace y HMAC; `systemId` común |
| Fiabilidad local | Sesión por arranque, secuencias, ACK de aplicación, reintentos, resultado de comando y fragmentación; tramas ≤250 bytes |
| C ↔ router | Wi-Fi STA de 2,4 GHz, configuración de red y obtención de IP |
| C ↔ servicios de hora | DNS y NTP; reloj válido antes de SAS/TLS |
| C ↔ IoT Hub | MQTT/TLS por TCP 8883, CA del servidor, hostname e identidad del dispositivo; autenticación SAS |

No hace falta instalar un broker MQTT local, una Azure Function ni MQTT en A/B.
La implementación de `BioIoTCommon` ya aporta el protocolo local. El ACK de radio
no equivale a que una calibración esté aplicada y persistida. MQTT publica con
QoS 0: aceptación local tampoco prueba almacenamiento en Cosmos DB.

## Hallazgos que afectan al despliegue

| Prioridad | Hallazgo verificado | Consecuencia y acción necesaria |
| --- | --- | --- |
| Alta | `iot_configs.example.h` define `BIOIOT_IOT_PLACEHOLDER 1`; `AzureLink::azureConfigured()` comprueba su presencia con `#ifdef` | Copiar la plantilla y completar sus cuatro parámetros deja Azure deshabilitado. **Eliminar la definición**, no ponerla a cero; o copiar los cuatro parámetros del original a un header real sin esa marca. |
| Alta | C busca un `iot_configs.h` en su propio sketch; no incluye automáticamente el de la raíz | Reutilizar la identidad requiere configurar expresamente `BioIoT_NodeC_Gateway/iot_configs.h`. La misma conexión TCP no se traslada entre placas: se reutilizan hub, identidad, rutas y autenticación. |
| Alta, **corregido en código; falta verificación física** | B llamaba a `LittleFS.begin()` sin desactivar el autoformato y además llamaba expresamente a `LittleFS.format()` si el montaje fallaba | ESP8266 core 3.1.2 tiene el autoformato activado por defecto. Un fallo de montaje podía borrar las calibraciones antes de que la doble ranura y el CRC las protegieran. **Corrección (2026-10-05):** `setAutoFormat(false)` antes de `begin()`; sin `format()` en el arranque. Sin montaje, B mide, informa `storage_mounted=false` y rechaza calibraciones con `rejected_storage_failed`. Una placa nueva se inicializa con un firmware de puesta en marcha y una orden con el token del chip ([INSTALACION.md](INSTALACION.md) §3.1). Pendientes en placa: P11 y P12 de [PRUEBAS.md](PRUEBAS.md). Mantener el respaldo exportado. |
| Alta | `ACTUATOR_COMMISSIONING_MODE=1`, confirmaciones eléctricas=0 | Los comandos se simulan; no hay accionamiento físico. Para producción faltan etapa eléctrica, polaridad, frecuencia PWM permitida y confirmaciones. Alta impedancia exige que la etapa tenga un estado de reposo adecuado. |
| Alta | El perfil de C es compresor/R/G/B = 13/32/33/4; v4 usa 32/33/4/13 | No conservar el cableado viejo con el perfil nuevo. Recablear al perfil objetivo o seleccionar `ACTUATOR_PIN_PROFILE_LEGACY_V4` antes de activar salidas. |
| Alta | El enlace cifrado mixto B↔C no se ha verificado físicamente | Probar ambos sentidos, ACK, calibración, reinicios y coexistencia con Azure. El tamaño ≤250 B favorece compatibilidad, pero las pruebas de PC no ejecutan las radios. |
| Media | `generate-secrets.mjs` valida sintaxis y MAC no nula, pero acepta difusión/multicast y no exige tres MAC distintas | Se comprobó que acepta `FF:FF:FF:FF:FF:FF` y `01:00:00:00:00:01`. Usar exclusivamente las tres MAC STA reales, diferentes. Añadir rechazo de multicast/difusión y duplicados al generador. |
| Media | El resumen diagnóstico de telemetría no conserva todos los campos de v4 | Faltan `diagnostics.i2c.tca_address`, `last_error_name`, `main_bus_isolated`, `diagnostics.analog_mux.sampled_at_ms` y `diagnostics.color.sampled_at_ms`. No asumir compatibilidad completa de la UI. Algunos detalles siguen en los informes por nodo; `quick` de C no sondea B. |
| Media | `report_interval_s` se multiplica por 1000 antes de comprobar el rango | En el ESP32 de 32 bits, 4294973 s se convierte por desbordamiento en 5704 ms y pasa el rango. Validar segundos antes de multiplicar o usar cálculo de 64 bits. |
| Media | `writeNodes()` calcula edades con el tiempo de la instantánea y las últimas recepciones/STATUS del momento de publicación | Al reenviar una instantánea antigua, `link_age_ms` y `status.age_ms` pueden quedar negativos. Calcular las edades del estado actual con `info.nowMono` o guardar el estado histórico junto con la instantánea. |
| Media, **retirado del índice; clave pendiente de rotar** | `iot_configs.h` de la raíz estaba versionado en Git (commits `8ca9da7` y `31f6a58`, ya presentes en `origin/main`) | **2026-10-05:** `git rm --cached iot_configs.h`. La copia local se conserva, el `.gitignore` raíz excluye `iot_configs.h`, `node_secrets.h`, `gateway_secrets.h` y `*.export.json`, y se añade `iot_configs.example.h` con valores ficticios. El retiro surte efecto en el próximo commit, pero **la clave sigue en el historial y en GitHub**. Rotarla en Azure Portal (IoT Hub → dispositivo → regenerar la clave) y actualizar los dos `iot_configs.h` locales. Rotar una clave SAS no exige un certificado nuevo. No se rotó ni se reescribió el historial en esta revisión. |

## Reutilización de Azure y certificados

Copiar a `BioIoT_NodeC_Gateway/iot_configs.h` los valores de
`IOT_HUB_HOSTNAME`, `DEVICE_ID`, `EXPERIMENT_ID` y la clave de dispositivo vigente.
Este header está excluido por `espnow_split/.gitignore`. El original de la raíz
está excluido por el `.gitignore` raíz desde 2026-10-05 y fue retirado del índice.

Apagar o desconectar de Azure la placa v4 antes de conectar C con la misma
identidad. IoT Hub admite una sola sesión MQTT activa por identidad de dispositivo;
dos placas con el mismo `DEVICE_ID` se expulsarán mutuamente.

Si C es la misma placa que antes ejecutaba v4 y se conservan sus credenciales
Wi-Fi en NVS, el código intenta reutilizarlas. Una placa C distinta necesita
aprovisionar también el Wi-Fi mediante `gateway_secrets.h` o el portal.

La MAC de C puede ser distinta: la identidad SAS configurada en este firmware
no depende de la MAC. A/B no necesitan nuevas identidades en IoT Hub ni claves
Azure; tienen sus claves locales ESP-NOW.

**No hay que generar un certificado de dispositivo por dividir el firmware.**
Se verificó por SHA-256 que `azure_ca.h` y `AzureIoTSasToken.cpp` de C son copias
idénticas a los de la raíz. C valida al servidor con DigiCert Global Root G2 y
se autentica ante Azure usando `DEVICE_KEY` para generar SAS de 60 minutos.
La CA pública autentica al servidor; no es un certificado privado de la tarjeta.

Microsoft indica incorporar también **Microsoft RSA Root Certificate Authority
2017** para tolerar cambios de cadena. Se descargaría la raíz pública oficial y
se añadiría al conjunto PEM; no se genera una raíz propia ni se desactiva TLS.
Esto es una mejora pendiente que viene de v4, no una necesidad creada por el split.

## MAC: identificación y asignación

El proyecto ya tiene la función necesaria. En las tres placas, monitor serie
a **115200**, enviar con terminador de línea:

```json
{"action":"pairing_info"}
```

Registrar `sta_mac` y el nodo correspondiente. Cada sketch también imprime su
MAC STA al arrancar. `WiFi.macAddress()` lee esa MAC tanto en ESP32 como en
ESP8266; no requiere instalar otro repositorio. Usar la MAC STA, no la MAC
del portal SoftAP `BioIoT-AP` ni la dirección del router.

Desde `espnow_split`, con MAC reales en lugar de estos ejemplos:

```powershell
node tools/generate-secrets.mjs --gateway-mac 24:6F:28:AA:BB:CC `
  --node-a-mac 24:6F:28:11:22:33 --node-b-mac 5C:CF:7F:44:55:66 `
  --channel 6 --ssid "RedLab-2.4GHz" --password "CLAVE_WIFI"
```

El generador registra en cada firmware las MAC de sus pares; **no cambia la MAC
de fábrica de ninguna placa**. Genera claves aleatorias comunes/coherentes para
los tres headers; por eso después hay que compilar y cargar los tres nodos.

Si por «definir MAC» se desea cambiarla expresamente, las APIs oficiales son
`esp_wifi_set_mac(WIFI_IF_STA, ...)` en ESP32 y `wifi_set_macaddr(STATION_IF, ...)`
en ESP8266. El proyecto no las usa. Conservar la MAC de fábrica evita introducir
duplicados y cambios de identidad local. La API ESP32 exige la interfaz desactivada
y una MAC unicast; debe seguirse su secuencia de inicialización documentada.

## Funcionalidades conservadas y diferencias reales

| Función | Resultado de la revisión |
| --- | --- |
| 13 sensores y sus unidades/crudos | Presentes: A tiene pH, CO2×2, turbidez, DO, TDS, temperatura y color×2; B luz×2 y O2×2 |
| Ecuaciones y perfiles CO2 | Implementados; el modelo portable CO2 procede del original |
| Calibración set/reset/reset_all y aire O2 | Enrutadas hacia A/B; guardar y verificar precede a aplicar |
| Persistencia de calibración de v4 | **Requiere exportar/importar**. Los nuevos namespaces/archivos no heredan la NVS `biocal`, aunque se reutilice una placa física |
| Diagnósticos y recuperación I2C | Existen por nodo, con informe agregado; cambian las rutas JSON y el resumen `quick` |
| Compresor y RGB remoto | Implementados en C, con reposo mínimo y vencimiento; accionamiento deshabilitado por los valores actuales |
| Telemetría a Azure y C2D | Conservados en C, condicionados a configurar credenciales reales sin la marca de plantilla |
| JSON UTF-8 hacia rutas existentes | Conservado el transporte y sus propiedades; la aplicación debe aceptar schema 1.1 |
| Bomba, PID, umbrales automáticos y fotoperiodos | No implementados en v4; la división tampoco los completa |

Diferencias que impiden afirmar «no cambia nada»:

- v4 apaga cargas tras 60 s sin IoT Hub; C las mantiene hasta el vencimiento
  manual, como máximo 900 s. El programa autónomo de respaldo está deshabilitado.
- `reboot` sin `node` reinicia solo C; para todo el sistema usar
  `{"action":"reboot","node":"all"}`. Requiere que A/B reciban las órdenes.
- `reset_all` distribuido puede terminar parcialmente si un nodo falla: hay que
  comprobar el resultado por nodo y no solo el ACK de recepción.
- `calibration.version` pasa a null, hay revisiones por nodo/sensor,
  `connected` puede ser null y aparecen `stale`/`node_offline`. Un valor vencido
  deja de publicarse como actual.
- Los diagnósticos completos se anidan bajo `nodes.node_a.report` y
  `nodes.node_b.report`; cambian los caminos usados por una aplicación existente.
- La deduplicación de actuadores por `commandId` conserva 16 IDs en RAM, no un
  historial permanente que sobreviva al reinicio.
- C almacena 40 instantáneas, unos 80 minutos a 120 s, **solo en RAM**. Un reinicio
  o corte de energía pierde lo pendiente; una interrupción más larga sobrescribe
  instantáneas. No equivale a almacenamiento garantizado ni a todas las muestras.

## Orden de despliegue

1. Exportar calibraciones v4 y custodiar el JSON con patrones `_f32`; evitar
   `Erase All Flash`. La herramienta de exportación requiere cargar un sketch
   temporal y abre `biocal` en solo lectura.
2. Verificar las placas: A/C son ESP32 WROOM clásico, B NodeMCU/D1 mini ESP8266;
   los pines actuales no son un perfil genérico para C3/S3/ESP-01.
3. Medir las MAC STA sin cargas y generar los secretos de los tres nodos.
4. Configurar Azure en el header de C, sin `BIOIOT_IOT_PLACEHOLDER`, con la clave
   ya rotada. Si B es nueva, inicializar su LittleFS (INSTALACION.md §3.1) y volver
   al firmware normal antes de importar calibraciones productivas.
5. Verificar mux analógico 15/14/13/3/12/11 o seleccionar perfil 0..5; TCA 0x77
   y SEN0322 0x73 según selectores reales. B mueve I2C de 21/22 a GPIO4/5.
   Sensores con la misma dirección pueden convivir en canales separados del TCA;
   TCA y sensor con la misma dirección en un canal activo sí colisionan.
6. Compilar con núcleos/bibliotecas fijados; C con partición `min_spiffs`.
7. Apagar el cliente Azure v4, cargar C/A/B, importar calibraciones y comparar
   los patrones `_f32`. Confirmar persistencia tras reinicio de A/B.
8. Comprobar `status` de C, enlace cifrado bidireccional, comandos, diagnósticos,
   cambios de canal, cortes de red y un documento nuevo de Cosmos con schema 1.1.
9. Activar actuadores físicos solo después de resolver perfil de pines y
   parámetros eléctricos; probar apagado, vencimiento, reinicios y reposo mínimo.

## Evidencia de esta revisión

- Pasaron los cinco scripts Node del original: `calibration-model`,
  `calibration-json`, `telemetry-contract`, `hardware-diagnostics` y
  `diagnostic-size`. Son comprobaciones de lógica/contrato con entradas simuladas.
- Se compararon los JSON de ejemplo: 13 sensores en ambos; se identificaron los
  cinco campos diagnósticos ausentes indicados arriba, contrastados con el
  serializador real de C.
- Igualdad SHA-256 de CA y generador SAS entre original y gateway.
- Se ejecutó el `parseMac` real del generador en aislamiento, sin escribir
  secretos: aceptó MAC broadcast y multicast.
- `run-host-tests.ps1` no pudo recompilar aquí: faltan `g++`, `clang++` y
  `python` en PATH. Las 58 pruebas del informe anterior no constituyen una
  ejecución nueva en esta revisión.
- Recompilación con `tools/build-all.ps1 -Original`, núcleos instalados,
  Arduino-ESP32 **3.3.12**, headers de ejemplo y salidas físicas deshabilitadas:

  | Sketch | Resultado actual | Flash / RAM estática |
  | --- | --- | --- |
  | A, `esp32:esp32:esp32` | Compila | 964000 B (73 %) / 58160 B |
  | B, `esp8266:esp8266:nodemcuv2:eesz=4M1M` | No se pudo compilar: núcleo ESP8266 no instalado en este equipo | Sin medición nueva |
  | C, `PartitionScheme=min_spiffs` | Compila | 1233661 B (62 %) / 59156 B |
  | Original v4, `esp32:esp32:esp32` | Compila | 1277760 B (97 %) / 58276 B |

  El script terminó con código 1 por la ausencia del núcleo de B; A, C y v4
  sí completaron sus compilaciones. El original tiene poco margen de flash.
  Las cifras de RAM no incluyen los picos de heap de TLS, JSON y buffers.
  Los binarios están en `../../tmp/review-espnow-build/`, excluido de Git.
  Para verificar B se requiere instalar su núcleo 3.1.2 o ejecutar su perfil
  reproducible, que descarga las dependencias aisladas; no se instalaron aquí.
- No se han comprobado hardware, TLS desde las placas, C2D real ni Cosmos real.

### Actualización 2026-10-05: Wi-Fi de C y hotspot móvil

Auditoría de `AzureLink` contra WiFiManager 2.0.17 y Arduino-ESP32 3.3.12 (código
fuente instalado). Defectos encontrados y **corregidos en código**; falta la
verificación física (P13–P15 de [PRUEBAS.md](PRUEBAS.md)):

| Defecto | Consecuencia | Corrección |
| --- | --- | --- |
| `WiFi.persistent(false)` antes de `WiFi.mode()` fija el almacenamiento Wi-Fi en RAM para toda la sesión (`wifiLowLevelInit`); el `WiFi.persistent(true)` posterior del portal no tiene efecto | **La red elegida en el portal no se guardaba en NVS**: se perdía al reiniciar | `commitPortalCredentials()`: tras conectar, guarda la red con `esp_wifi_set_storage(FLASH)` + `esp_wifi_set_config` y vuelve a RAM |
| Con `WIFI_SSID` definido se usaban siempre las predeterminadas, también en los reintentos | Las credenciales guardadas, incluidas las del portal, quedaban ignoradas | Orden: NVS primero; predeterminadas solo si no hay ninguna guardada |
| Con credenciales (guardadas o predeterminadas) que fallaban, solo había reintentos | Una contraseña predeterminada incorrecta **impedía abrir el portal** | Portal automático si falla el primer intento acotado (≤ 15 s) |
| WiFiManager apaga la STA al abrir el portal sin conexión; desde modo STA llega a detener el Wi-Fi (`espWiFiStop`) | ESP-NOW usa la interfaz STA: **C dejaba de oír a A y B durante el portal** | Subclase con `_disableSTAConn=false`: AP+STA en el canal de ESP-NOW |
| Doble cierre del portal: WiFiManager se cierra solo a los 180 s y `AzureLink` llamaba otra vez a `stopConfigPortal()` (a los 185 s), con `server` ya liberado | Desreferencia nula ⇒ **reinicio de C al vencer el portal** | Un solo temporizador (`WifiPolicy`); WiFiManager con timeout 0; se cierra solo si `getConfigPortalActive()` |
| Portal abierto por Serial estando conectado: la máquina pasaba a `Connected` y dejaba de llamar a `process()` | AP sin servidor indefinidamente | El portal es independiente del estado STA; MQTT sigue mientras está abierto |
| Depuración de WiFiManager activa por defecto | Imprimía el SSID por Serial | `setDebugOutput(false)` |
| Guardado en el portal sin plazo (`waitForConnectResult()` de 60 s) | Bloqueaba `loop()` hasta 60 s | `setSaveConnectTimeout(15)` |
| Hotspot: barrido completo a los 5 min como mínimo y reintento rápido ligado para siempre al BSSID | Con un hotspot que vuelve en otro canal o con otro BSSID, C tardaba ≥ 5 min | 2 intentos rápidos y búsqueda del SSID en ≤ 45 s; luego 60 s → 10 min |
| HELLO de C con el canal de arranque | Campo `channel` desactualizado tras un cambio | HELLO actualizado y reanuncio de 60 s a los nodos sin trama fresca |

A y B no se asocian al Wi-Fi: solo llaman a `WiFi.mode(WIFI_STA)` y
`WiFi.disconnect()`, nunca a `WiFi.begin()`. Se conservan `pairing_info` y el canal
guardado (`bioiot_c/ch`), ahora solo como pista inicial. No cambian sensores,
calibraciones, actuadores, protocolo ESP-NOW ni credenciales. La telemetría
añade tres campos en `gateway` (`wifi_search_state`, `wifi_ms_since_connected` y
`espnow_channel_changes`). Compilación de C y pruebas: [PRUEBAS.md](PRUEBAS.md) §1c.

Observación sin cambiar: con router pero sin Internet, cada intento MQTT puede
bloquear `loop()` hasta ~30 s (timeout TCP por defecto de `NetworkClientSecure`).
No reinicia C (watchdog de `loop()` 120 s) y no afecta a la tarea local, pero
retrasa la respuesta del portal y de Serial mientras dura.

### Actualización 2026-10-05: LittleFS de B y credenciales

- B **ya compila** en este equipo: perfil `nodo_b_nodemcu`, ESP8266 core 3.1.2 y
  secretos de marcador. Producción: IROM 333.276 B y RAM estática 49.268 B, sin
  advertencias. Variante de puesta en marcha: IROM 333.500 B. Detalle en
  [PRUEBAS.md](PRUEBAS.md) §1b.
- Pruebas de PC 62/62 con zig 0.13.0; 3 nuevas de almacenamiento de B. Pruebas
  Node del original: 7/7.
- Búsqueda de credenciales (sin revelar valores):
  - Única clave real versionada: `iot_configs.h` de la raíz, retirada del índice.
  - Su copia `BioIoT_NodeC_Gateway/iot_configs.h` es idéntica y ya estaba ignorada.
  - Copias en `tmp/build-*` y `tmp/review-espnow-build/original` quedan fuera de
    Git porque `/tmp/` está ignorado, pero siguen en disco.
  - Sin `node_secrets.h` ni `gateway_secrets.h` reales.
  - Ninguna cadena de conexión `HostName=…;SharedAccessKey=…` ni token SAS literal.
  - El literal hexadecimal largo de `README.md` es la huella SHA-256 pública de la CA.
- Sigue pendiente en placa: P11 y P12. En Azure Portal: rotar `DEVICE_KEY`.
- Nada de esto cambia curvas, campos `_f32`, pines, direcciones I2C, protocolo,
  claves, contrato de telemetría, warm-up ni el firmware v4.

## Fuentes oficiales

- [ESP-NOW: canales, seguridad, ACK y tamaños](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html).
- [Una sesión MQTT por identidad y autenticación IoT Hub](https://learn.microsoft.com/en-us/azure/iot-hub/iot-mqtt-connect-to-iot-hub).
- [Raíces TLS indicadas por Microsoft](https://learn.microsoft.com/en-us/azure/iot-hub/migrate-tls-certificate).
- [WiFi.macAddress en ESP8266](https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/station-class.html#macaddress).
- [APIs MAC de ESP32](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/network/esp_wifi.html).
- [LittleFS del núcleo ESP8266 3.1.2: autoformato](https://github.com/esp8266/Arduino/blob/3.1.2/libraries/LittleFS/src/LittleFS.h).
