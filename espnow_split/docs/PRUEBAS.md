# Pruebas realizadas y pruebas físicas pendientes

## 1. Qué se ejecutó (con evidencia, 2026-10-04)

### Compilación (arduino-cli 1.5.1)

| Sketch | Placa / FQBN | Resultado |
| --- | --- | --- |
| BioIoT_NodeA_Sensors | `esp32:esp32:esp32` (3.3.12) | OK: 964.000 B de flash (73 %), 58.160 B de RAM estática |
| BioIoT_NodeB_I2C | `esp8266:esp8266:nodemcuv2:eesz=4M1M` (3.1.2) | OK: IROM 332.844, IRAM 28.595, DATA 4.752, RODATA 7.964, BSS 36.024 |
| BioIoT_NodeB_I2C con perfil `nodo_b_nodemcu` (descarga aislada de núcleo y bibliotecas fijadas) | idem | OK: binario idéntico (IROM 332.844) |
| BioIoT_NodeC_Gateway | `esp32:esp32:esp32` por defecto | OK: ≈1.233.500 B (94 %) |
| BioIoT_NodeC_Gateway | `…:PartitionScheme=min_spiffs` | OK: 1.233.661 B (62 %), 59.156 B de RAM estática |
| A, B y C con secretos **generados** (ruta con ESP-NOW habilitado) | idem | OK: 973.204 B / IROM 338.924 / 1.242.569 B |
| tools/BioIoT_CalibrationExport | `esp32:esp32:esp32` | OK: 365.444 B |
| **BioIoT_Azure_Integrated.ino original, sin cambios**, con `espnow_split/` dentro | `esp32:esp32:esp32` | OK: 1.277.760 B (97 % con 3.3.12). Ningún objeto compilado de `espnow_split` |

- **Advertencias del código nuevo:** ninguna. Se eliminaron las de `volatile` y `containsKey`.
- **Perfiles ESP32:** no se ejecutaron (descarga de más de 1 GB). A y C se compilaron con el mismo núcleo 3.3.12 ya instalado.

### Pruebas de PC (58/58, 0 fallos)

Ejecución: `tools/run-host-tests.sh` (o `.ps1`), compilando con zig 0.13 (clang, C++17, `-Wall -Wextra`) contra ArduinoJson 7.4.3 real.

| # (encargo) | Pruebas |
| --- | --- |
| 1. Serialización, ≤ 250 B, corruptos y truncados | Vectores SHA-256, HMAC (RFC 4231) y CRC32; little-endian explícito; trama de 250 B y rechazo de 251; **cada bit invertido** rechazado; **cada longitud truncada** rechazada; sistema, versión, destino, tipo y clave erróneos; vector dorado (autoprueba en placa); tamaños reales de cada mensaje; decodificadores contra NaN/inf, sensor ajeno, conteo, calidad reservada y muestra «del futuro» |
| 2. Duplicados, pérdidas, desorden, timeouts, comandos repetidos | Ventana de deduplicación; canal con pérdida 1/3, duplicación 1/4 y orden invertido ⇒ entrega exactamente una vez; `txUptime` reescrito en cada reintento con el mismo `seq`; comando sin ACK ⇒ `Failed` acotado; ACK `queue_full`; caché de idempotencia; `commandId` repetido en actuadores |
| 3. Persistencia, importación y conservación (incluida escritura fallida) | Doble ranura: primer arranque, alternancia, **corte a mitad de escritura**, escritura fallida, relectura corrupta (invalida la ranura), CRC, formato futuro bloqueado, generación con desbordamiento; calibración no activada si no se guarda; importación v4 → nodos bit a bit (`_f32`), rechazo de `_f32` incoherente, sección inválida, versión de formato; reglas `force` |
| 4. Warm-up y desconexión ≠ inválida | Política por alimentación (encendido, reinicio con RTC, sin RTC, alimentación no compartida, latch ante desbordamiento); O2 `warming_up`, `communication_fault`, `out_of_range`, `stabilizing`, `good` y `above_nominal_range` como estados distintos |
| 5. Dato vencido, falta de reloj, reinicio con sesión nueva | Edad en el reloj del nodo; vencido ⇒ `stale`, nunca entrada de control; nodo sin enlace ⇒ `node_offline`; UTC nunca inventado (`null`/`unsynchronized`) y derivado por reloj monótono; reinicio de nodo ⇒ comandos viejos `node_rebooted`; reinicio del gateway ⇒ telemetría reasociada; **replay** de sesión anterior rechazado |
| 6. Pérdida de Azure, SAS y prioridad de control **sin cargas reales** | Controlador de salidas simulado: en puesta en marcha no se escribe ningún GPIO; bloqueo por parámetros no confirmados; 299 s sin UTC ni nube ⇒ sigue encendido y vence a los 300 s por reloj monótono; protecciones de v4; OFF manual sobre el programa local; programa sin parámetros deshabilitado |
| 7. Colas llenas y recuperación acotada | Cola de salida acotada y expulsión de telemetría antigua; comandos nunca expulsados; vaciado tras reconectar; TTL; cola de recepción SPSC con descarte contado; instantáneas: 101 en 40 ranuras ⇒ 61 descartadas contadas; tabla de comandos acotada; fragmentación con duplicados, desorden, inconsistencia y plazo |
| Recuperación de canal | El nodo arranca en el canal 1, el gateway está en el 6 ⇒ barrido, sesión, canal guardado, dentro de los plazos |
| Comandos v4 y JSON 1.1 | Ejemplos de README v4 enrutados y validados; contrato 1.1; ISO UTC (incluido bisiesto); `calibration_ack` y resumen parcial; sin JSON truncado |

**Memoria:** el enlace, las colas, las instantáneas y los ensambladores son arreglos de tamaño fijo, sin asignaciones tras el arranque. Los documentos ArduinoJson son locales y se liberan al salir de su función. En placa, `min_free_heap` (A, C) y `min_free_heap_observed` (B) se publican en `nodes.*.status`, para vigilar fugas en la prueba de estabilidad.

### Pruebas del proyecto original

`node tests/*.mjs` en la carpeta v4: las 6 pasan, sin cambios.

## 1b. Corrección de LittleFS del nodo B (2026-10-05)

Cambio: el autoformato queda desactivado (`LittleFSConfig::setAutoFormat(false)` antes de `begin()`) y desaparece `LittleFS.format()` del arranque. Inicializar una placa nueva exige un firmware de puesta en marcha y una orden con el token del chip ([INSTALACION.md](INSTALACION.md) §3.1).

**Compilación** (arduino-cli 1.5.1, perfil `nodo_b_nodemcu`, ESP8266 core 3.1.2, `eesz=4M1M`, sin `node_secrets.h`, es decir, marcadores de `node_secrets.example.h`):

| Variante | Resultado | IROM / IRAM / RAM estática |
| --- | --- | --- |
| Producción (`NODE_B_STORAGE_COMMISSIONING` = 0, por defecto), `--clean` | OK, sin advertencias | 333.276 B / 28.595 B / 49.268 de 80.192 B |
| Puesta en marcha (`--build-property "compiler.cpp.extra_flags=-DNODE_B_STORAGE_COMMISSIONING=1"`) | OK; solo el `#warning` intencionado | 333.500 B / 28.595 B / 49.608 B |

Comprobación del binario: la cadena `Formateando LittleFS` aparece en el `.bin` de puesta en marcha (1 vez) y **no** en el de producción (0 veces). En producción, la ruta hacia `LittleFS.format()` no se compila.

**Pruebas de PC:** 62/62, 0 comprobaciones fallidas. Se compilaron con zig 0.13.0 (`python -m ziglang c++`, instalado aparte, fuera del proyecto), C++17, `-Wall -Wextra` y ArduinoJson 7.4.3. Las tres nuevas están en `tests/host/test_storage_b.cpp`:

- `storage_b_format_requires_commissioning_build_and_token`: en producción nunca se formatea. En puesta en marcha tampoco si LittleFS monta o no hay partición. Además se exige el token exacto del chip.
- `storage_b_unmounted_rejects_calibration_and_keeps_previous_slots`: con LittleFS sin montar, `set`, `calibrate` en aire e `import` con `force` se rechazan con `rejected_storage_failed`. La calibración en aire no llega al SEN0322 y nada se informa como guardado. No hay escrituras y las ranuras previas quedan idénticas: al volver a montar se recupera la calibración anterior.
- `storage_b_mounted_rejections_keep_their_reason`: con LittleFS montado, los rechazos por warm-up, esquema futuro o parámetros conservan su motivo.

Las 5 pruebas previas de doble ranura (`store_*`) siguen pasando. La única advertencia del compilador es previa y ajena a este cambio: `GatewayState.h:155`, campo privado sin uso en C.

**Pruebas Node del original** (`node tests/*.mjs`): las 7 pasan, incluida `color-calibration.mjs`.

**No verificado en placa:** que el core 3.1.2 no formatee de verdad al fallar el montaje, el mensaje periódico, el procedimiento `storage_format` y la conservación de LittleFS tras recargar. Vea P11 y P12.

## 1c. Wi-Fi del gateway y hotspot móvil (2026-10-05)

**Compilación de C:** `esp32:esp32:esp32:PartitionScheme=min_spiffs`, Arduino-ESP32 3.3.12, WiFiManager 2.0.17, secretos de marcador. Resultado: 1.242.821 B (63 %), 59.332 B de RAM estática, sin advertencias.

**Pruebas de PC:** 77/77, 0 fallos. Hay 15 nuevas en `tests/host/test_wifi_policy.cpp`: un hotspot simulado ejecuta las acciones de `WifiPolicy` igual que `AzureLink`, y la última usa además los `Endpoint`/`NodeLink` reales.

| Prueba | Verifica |
| --- | --- |
| `wifi_stored_credentials_valid_connect_first` | Primero las credenciales de NVS; unas predeterminadas incorrectas no se usan |
| `wifi_default_network_available_when_nothing_stored` | Sin NVS, usa las de `gateway_secrets.h` |
| `wifi_default_network_absent_opens_portal_within_bound` | Red ausente ⇒ portal en ≤ 15 s; la radio vuelve al canal de ESP-NOW |
| `wifi_wrong_default_password_cannot_block_portal` | Contraseña incorrecta ⇒ portal (antes del plazo si el fallo es definitivo) |
| `wifi_no_credentials_opens_portal_immediately` | Sin credenciales ⇒ portal inmediato |
| `wifi_portal_by_serial_keeps_connection_and_espnow_channel` | Portal por Serial: sigue conectado, el AP está en el canal de ESP-NOW y no se abre dos veces |
| `wifi_portal_expires_after_180_s_and_retries_spaced` | Cierre a los 180 s, sin intentos con el portal abierto, reintentos espaciados y un solo portal automático |
| `wifi_new_network_saved_in_portal_is_reused` | Contraseña errónea en el portal: no se guarda. Correcta: se guarda, se reconecta tras perderla y tras «reiniciar» |
| `hotspot_available_at_boot_adopts_its_channel` | El canal guardado es solo una pista: ESP-NOW adopta el canal del hotspot |
| `hotspot_off_bounded_searches_and_no_restart` | Primera búsqueda completa a 20–45 s de la pérdida. En 30 min: 4–9 búsquedas, < 1 % del tiempo fuera de canal, sin portal ni reinicio |
| `hotspot_returns_same_channel_and_bssid_quick_reconnect` | Retorno igual: reconexión rápida sin barrido |
| `hotspot_same_ssid_different_channel` | Otro canal: búsqueda completa, ESP-NOW adopta el canal nuevo y `espnow_channel_changes`=1 |
| `hotspot_same_ssid_different_bssid` | Otro BSSID: como máximo 2 intentos rápidos y después búsqueda por SSID |
| `hotspot_portal_opens_after_boot_failure_and_by_serial` | Portal automático tras el fallo de arranque y por Serial; con el portal abierto no hay barridos |
| `espnow_survives_internet_loss_and_follows_channel_change` | 15 min sin hotspot: A entrega su telemetría a C sin perder el enlace ni buscar canal. El hotspot vuelve en el canal 11: C lo adopta y A lo encuentra con `channel_hunting` y vuelve a entregar |

Estas pruebas modelan la radio. No sustituyen la verificación con WiFiManager, el SDK y un hotspot reales (P13–P15).

## 1d. Publicación por trozos de C (2026-10-06)

Corrige `Telemetria: JSON no generado (memoria/tamano)`: sin buffer del tamaño del mensaje, ~40 KB de heap liberados y sin pérdida de muestras por falta de heap. Detalle en [NODE_C_ARRANQUE.md](NODE_C_ARRANQUE.md). Pruebas de PC: 88/88, incluidas 5 nuevas en `tests/host/test_publish_stream.cpp`. C compila con `min_spiffs` (1.251.069 B, 63 %) sin advertencias. Pendiente en placa (P16): con Azure conectado, 30 min de telemetría sin `JSON sin memoria`, `json_no_memory` en `status` y `HEAP after_mqtt_tls_connect` registrados.

## 2. No verificado (requiere hardware)

- Interoperabilidad **cifrada** ESP8266 ↔ ESP32 (CCMP con PMK/LMK) con estos núcleos.
- La autoprueba del vector dorado en cada placa (se imprime al arrancar).
- Cableado, perfiles de canales y dirección del TCA, tiempos reales de adquisición, BH1750/SEN0322 en ESP8266 con stretch de 50 ms.
- TLS/MQTT con IoT Hub desde C, ruta a Cosmos y recepción C2D.
- Etapas de potencia, polaridades, PWM y cargas: nada se activó.
- Wi-Fi de C con WiFiManager, el SDK y un hotspot reales: portal, guardado en NVS, cambio de canal/BSSID y reenganche de A/B (P13–P15).
- LittleFS de B en flash real: montaje sin autoformato, puesta en marcha de placa nueva, persistencia tras recargar el firmware (P11, P12).

## 3. Procedimiento de pruebas físicas pendientes

Registre cada prueba con la fecha, la versión de firmware, los números de serie o MAC, las capturas de monitor serie de los tres nodos y `{"action":"status"}` de C antes y después. **No invente resultados**: si no se pudo medir, escriba «no medido».

| # | Prueba | Procedimiento | Medir / criterio |
| --- | --- | --- | --- |
| P1 | ESP-NOW cifrado mixto | Secretos generados, los 3 nodos en la mesa, cargas desconectadas | Autoprueba OK en las 3; «Sesion confirmada» en A y B; en C, `rx_bad_tag=0`, `node_*.online=true`; telemetría de A y B en C. Repetir con una LMK alterada en B ⇒ B **no** debe enlazar |
| P2 | Arranque sin router | Router apagado; encender C, luego A y B | C sin bloqueos («operacion local»); sesiones en el canal inicial; sensado continuo; actuadores (simulados) responden por C2D al volver la red |
| P3 | Cambio de canal | Con todo enlazado, cambiar el canal fijo del router | C se reasocia en el canal nuevo; A y B pasan por `channel_hunting` y vuelven; medir el tiempo hasta `online` (esperado < 20 s + un barrido de 3,3 s) |
| P4 | Pérdida y retorno del router | Apagar el router 10 min, encenderlo | Instantáneas acumuladas (`buffered_unpublished`) y publicadas en orden con `replayed=true` y `timestampUtc` original; `snapshots_dropped=0` |
| P5 | Reinicios | Reiniciar A, B y C (botón y corte de energía) por separado | Nueva sesión; sin reprocesar comandos viejos; B: warm-up según la política (`o2_warmup_assumed_full`, `credited_powered_ms`); C: actuadores apagados al arrancar |
| P6 | Alcance con compresor funcionando | Etapa confirmada en producción; distancias reales; compresor encendido | `retries`, `tx_failures`, `telemetry_dropped` y RSSI por nodo durante 30 min; sin reinicios por *brownout* |
| P7 | Retraso de control | `control` C2D con marca de tiempo | Desde el envío hasta el cambio en GPIO (osciloscopio) y hasta `actuator_state`; vencimiento del lease con Wi-Fi caído |
| P8 | Estabilidad prolongada | 72 h con datos reales | Tendencia de `min_free_heap`, `max_alloc_heap` y `reset_reason`; pérdidas y reintentos por hora; sin reinicios por watchdog |
| P9 | Diagnóstico completo | `{"action":"diagnostics","scope":"full"}` y `i2c_recover` | Informe agregado con A y B; tiempo total; coherencia con el cableado (perfil de canales y TCA) |
| P10 | Migración | Exportar v4 → importar → `calibration_export` | Campos `_f32` idénticos al archivo exportado |
| P13 | Hotspot móvil: canal y BSSID cambiantes | Hotspot de 2,4 GHz y A, B y C enlazados. Apagar el hotspot 2 min y volver a encenderlo; repetir hasta que vuelva en **otro canal** | Registrar `wifi_search_state`, `wifi_ms_since_connected`, `espnow_channel`, `espnow_channel_changes`, `node_*.online` y el tiempo hasta la reconexión. Esperado: búsqueda completa ≤ 45 s tras la pérdida; con canal nuevo, A y B `online` en ≤ ~90 s; sensado y colas sin interrupción; sin reinicio de C |
| P14 | Portal | Sin credenciales, o con una contraseña predeterminada incorrecta: arrancar C. Conectarse a `BioIoT-AP` y abrir http://192.168.4.1. Probar primero una contraseña errónea y después la buena. Reiniciar C. Repetir con `{"action":"wifi_portal"}` mientras está conectado | Portal en ≤ 15 s; durante el portal, A y B siguen `online` en `status`. Contraseña errónea: no se guarda. Buena: «guardada en NVS» y se reutiliza tras reiniciar. El portal cierra a los 180 s si no se usa. Ningún log muestra el SSID ni la contraseña |
| P15 | Pérdida de Internet sin pérdida de Wi-Fi | Hotspot encendido, pero sin datos móviles | C sigue `connected`; MQTT reintenta cada 5 s sin reiniciar; A y B siguen `online`; instantáneas en `buffered_unpublished` |
| P11 | Placa B nueva | Procedimiento [INSTALACION.md](INSTALACION.md) §3.1 en una placa sin LittleFS: firmware normal, después puesta en marcha, `storage_format` y vuelta al normal | Firmware normal: `ALMACENAMIENTO NO DISPONIBLE (mount_failed_not_formatted)`, sensado activo y `calibration_import` → `rejected_storage_failed`. Token incorrecto → `confirm_token_mismatch`. Token correcto → `formatted`. Tras recargar el firmware normal: `storage_mounted=true`, `storage_state=ok` y `storage_format` → `storage_format_requires_commissioning_build` |
| P12 | Fallo de montaje sin pérdida | Con calibración importada: `calibration_export`, respaldo `read_flash 0x300000 0xFA000`. Cargar a propósito con otro `eesz` (p. ej. `4M2M`, que mueve la partición), arrancar e intentar `calibration_import`. Volver a cargar con `eesz=4M1M` | Con el `eesz` erróneo: `mount_failed_not_formatted`, sensado activo, importación rechazada, sin `formatted` en el registro. De vuelta en `4M1M`: `calibration_export` con campos `_f32` idénticos a los previos |
