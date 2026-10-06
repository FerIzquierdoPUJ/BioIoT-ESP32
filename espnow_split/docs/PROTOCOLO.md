# Protocolo ESP-NOW BioIoT v1

Implementación: `libraries/BioIoTCommon` (`bioiot_protocol`, `bioiot_messages`, `bioiot_link`, `bioiot_fragment`, `bioiot_node_link`). Toda la lógica es portable y se prueba en PC. Los adaptadores ESP32/ESP8266 son la única parte específica de plataforma.

## 1. Trama (máximo 250 bytes, límite ESP-NOW v1 común a ESP8266 y ESP32)

Todos los enteros van en *little-endian* y se serializan campo a campo: nunca se envían `struct` de memoria. Los `float` son IEEE-754 de 32 bits; NaN significa «sin valor» y siempre va acompañado de una calidad explícita. Infinito se rechaza.

| Offset | Tamaño | Campo |
| ---: | ---: | --- |
| 0 | 2 | magic `B1 0E` |
| 2 | 1 | versión de protocolo = 1 (cualquier otra se rechaza) |
| 3 | 1 | tipo de mensaje |
| 4 | 4 | `systemId` (rechaza otros sistemas) |
| 8 | 1 | nodo origen (1 = C, 2 = A, 3 = B) |
| 9 | 1 | nodo destino |
| 10 | 1 | flags (bit0 ACK solicitado, bit1 retransmisión) |
| 11 | 1 | longitud del payload N (≤ 218) |
| 12 | 4 | `srcBoot`: aleatorio de 32 bits por arranque del emisor |
| 16 | 4 | `dstBoot`: arranque del receptor según el emisor (0 = desconocido) |
| 20 | 4 | `seq`: por emisor y arranque; una retransmisión conserva `seq` |
| 24 | N | payload |
| 24+N | 8 | HMAC-SHA256 truncado (clave de 32 B **por enlace**) sobre los bytes 0..24+N |

`24 + 218 + 8 = 250`. Hay `static_assert` en compilación y pruebas de tamaño, de cada bit invertido, de cada truncado y de versión, sistema, destino y clave erróneos (`tests/host/test_protocol.cpp`). Una autoprueba de arranque compara una trama codificada con un vector dorado generado en PC. Si ESP32 o ESP8266 producen otros bytes, ESP-NOW no se activa.

## 2. Tipos y tamaños reales

| Tipo | Sentido | ACK app | Payload (B) | Trama (B) |
| --- | --- | --- | ---: | ---: |
| HELLO (1) | ambos | no | 11 | 43 |
| STATUS (2) | nodo→C, cada 30 s | no | 51 | 83 |
| TELEMETRY (3) A parte 0 (pH, CO2×2, turbidez, DO, TDS, temperatura) | A→C, cada 5 s | sí | 188 | 220 |
| TELEMETRY (3) A parte 1 (color×2) | A→C | sí | 105 | 137 |
| TELEMETRY (3) B (luz×2, O2×2) | B→C, cada 5 s | sí | 119 | 151 |
| ACK (4) | ambos | — | 6 | 38 |
| CONFIG (5) | C→nodo | sí | 11 | 43 |
| CALIBRATION (6): set/reset/reset_all/calibrate/import | C→nodo | sí | 9–136 | ≤ 168 |
| COMMAND_RESULT (7) | nodo→C | sí | 18 | 50 |
| DIAGNOSTIC_REQUEST (8) | C→nodo | sí | 5 | 37 |
| DIAGNOSTIC_RESULT (9), fragmento | nodo→C | sí | ≤ 218 | ≤ 250 |
| TIME_SYNC (10) | C→nodo, cada 60 s | no | 17 | 49 |
| COMMAND (11): reboot, send_state | C→nodo | sí | 5 | 37 |
| CALIBRATION_STATE (12) A / B | nodo→C | sí | 114 / 65 | 146 / 97 |

Cada registro de telemetría lleva:
- `sensorId`, `quality`, `flags` (connected, expected, valid, warming, stabilizing, i2c_detected, below_reference, provisional, modo/usuario de calibración, observed).
- **Revisión de calibración del sensor.**
- `sampleUptimeMs` (reloj del nodo) y sus valores (crudo, voltaje, valor y auxiliares).

El mensaje lleva `txUptimeMs`, que el enlace reescribe y vuelve a firmar en **cada** retransmisión.

Los decodificadores exigen consumo exacto y rechazan:
- sensor inexistente o que no pertenece al nodo emisor;
- número de valores distinto del contrato;
- calidades reservadas al gateway (`stale`, `node_offline`);
- muestras posteriores a su transmisión;
- UTC fuera de 2020–2100;
- intervalos fuera de 1..600 s;
- coeficientes no finitos.

## 3. Sesiones, deduplicación y *replay*

- **Sesión:** HELLO con `dstBoot` igual a mi arranque prueba que el par me escuchó en este arranque y confirma su sesión. Cualquier otra trama exige `dstBoot == mi arranque` y `srcBoot` igual a la sesión confirmada, o a la candidata, que entonces se promueve. Si no se cumple, se rechaza y se responde HELLO (máximo uno cada 300 ms).
- **Arranques retirados:** los 4 últimos `srcBoot` sustituidos no se aceptan nunca más en este arranque. Reinyectar tramas de una sesión anterior se rechaza (prueba `replayed_old_session_frames_rejected`).
- **Deduplicación:** ventana de 64 por `(par, srcBoot, seq)`. Un duplicado se vuelve a confirmar (`ACK duplicate`) sin entregarlo otra vez a la aplicación. Fuera de la ventana: `too_old`.
- **Reinicio de un nodo:**
  - Su nuevo `bootId` abre otra sesión y su ventana de deduplicación empieza de cero.
  - Los comandos de C dirigidos al arranque anterior **no se reenvían**: quedan como `node_rebooted`, resultado desconocido. Comparar con la revisión de calibración informada por el nodo.
  - Los resultados en caché del nodo se pierden. Un comando del arranque anterior es imposible de entregar, porque llevaría otro `dstBoot`.
- **Reinicio del gateway:** los nodos reenvían su telemetría pendiente a la nueva sesión (`rebindOnReboot`). Las muestras conservan su instante original.
- **Idempotencia:** cada comando lleva `cmdId`. El nodo guarda los 8 últimos resultados por arranque del gateway y, ante un comando repetido, reenvía el resultado sin ejecutarlo. En C, los actuadores ignoran un `commandId` C2D repetido (16 últimos).

## 4. ACK, resultados y persistencia

| Señal | Significa | No significa |
| --- | --- | --- |
| Callback de envío del driver | La radio entregó la trama al aire | Que llegó o se procesó |
| ACK de aplicación `accepted` | El receptor verificó HMAC y sesión y **encoló** el mensaje | Que lo ejecutó |
| ACK `queue_full` | Recibido pero no aceptado; se reintenta sin marcarlo visto | — |
| COMMAND_RESULT `applied` + `saved` | Ejecutado **y** persistido (la candidata se guardó y se releyó antes de activarse) | Calibración física confirmada |
| COMMAND_RESULT `rejected_storage_failed` | No se pudo guardar: nada cambió | — |
| `calibrate` O2 `command_sent` | El SEN0322 aceptó la escritura I2C | Que la calibración interna del sensor sea correcta (`physical_calibration_confirmed=false`) |

En C, los estados por comando son:
- `pending`: aceptado por C.
- `delivered`: ACK de aplicación recibido.
- `applied`, `rejected`, `failed`: resultado del nodo.
- `expired`: venció su `expiresAt` antes de entregarse.
- `timeout`: sin ACK en 30 s, o sin resultado 30 s después del ACK.
- `node_rebooted`: el nodo reinició antes de responder.

## 5. Reintentos y colas (acotados, no bloqueantes)

- **Reintentos:** 250, 500, 1000, 2000 ms y +0..99 ms aleatorios (desfase entre nodos). Hay 4 o 5 intentos por ronda. Los mensajes persistentes (telemetría, resultados) hacen otra ronda cada 3 s hasta su TTL. Hay un solo mensaje en vuelo por destino y se respeta el orden FIFO.
- **Reportes:** A cada 5 s con fase 0; B cada 5 s con fase de 2,5 s; ±0–199 ms aleatorios en cada reporte.
- **Cola de salida:** 24 entradas en ESP32 y 16 en ESP8266. Con la cola llena se expulsa la telemetría más antigua que no esté en vuelo (contada como `telemetry_dropped`). Los comandos y resultados nunca se expulsan; si no caben, se rechazan.
- **Cola de recepción:** el callback solo hace comprobaciones baratas (MAC conocida, magic, versión, sistema, destino, longitud y coherencia MAC↔nodo) y copia la trama a una cola SPSC de 16 (ESP32) u 8 (ESP8266) tramas. HMAC, sesión y procesamiento ocurren en `loop()` o en la tarea local. Sin TLS, flash ni adquisición en el callback.
- **Comandos de C a la tarea local:** cola FreeRTOS de 4. Llena ⇒ rechazo `gateway_busy`.

## 6. Fragmentación (solo DIAGNOSTIC_RESULT)

| Parámetro | Valor |
| --- | --- |
| Tamaño máximo del informe por nodo | 4096 B |
| Bytes por fragmento | 210 |
| Fragmentos máximos | 20 |
| Ensamblado | uno por nodo en C, 2 × 4 KB estáticos |
| Plazo entre fragmentos | 20 s |
| Plazo total | 100 s (el barrido I2C completo de B puede tardar 60–90 s con el bus colgado) |
| Validación | `requestId`, cuenta y longitud coherentes; cada fragmento completo salvo el último; duplicados ignorados; desorden admitido |

## 7. Tiempo

- Los nodos nunca inventan UTC. C envía TIME_SYNC solo con hora NTP válida. El nodo mantiene `utc = utcSync + (millis − millisSync)` durante un máximo de 24 h.
- **Edad de una muestra:** `sampleMono_C = rxMono_C − (txUptime_nodo − sampleUptime_nodo)`. La resta se hace dentro de un mismo reloj, así que no se mezclan `millis()` de placas distintas. Una retransmisión reescribe `txUptime` y no rejuvenece el dato (prueba `tx_time_is_rewritten_on_each_retransmission`).
- C usa `esp_timer` (64 bits, sin desbordamiento a 49,7 días). Si no había hora al tomar una instantánea, `timestampUtc` se deriva después por el reloj monótono (`time_status: derived_after_sync`). Si sigue sin hora: `null` y `unsynchronized`.

## 8. Seguridad

- **Cifrado ESP-NOW:** unicast con PMK común y **LMK propia por enlace** (A↔C, B↔C).
  - ESP32: `esp_now_set_pmk` y `peer.encrypt=true`.
  - ESP8266: `esp_now_set_kok` y la LMK en `esp_now_add_peer`.
  - Si el cifrado no se puede configurar, ESP-NOW **no arranca**. No hay degradación silenciosa a texto plano. Existe `BIOIOT_ALLOW_UNENCRYPTED_ESPNOW=1` solo como opción de diagnóstico explícita (se informa en el adaptador).
- **Autenticación de aplicación:** HMAC-SHA256 de 8 bytes con clave de 32 bytes por enlace y `systemId`, independiente de la MAC. La MAC solo filtra en el callback y debe coincidir con el nodo declarado.
- **Interoperabilidad cifrada ESP8266↔ESP32:** ambos SDK usan CCMP con PMK/LMK de 16 bytes y ESP-NOW v1 (≤ 250 B). La API es correcta para las dos versiones fijadas. **No está verificada en hardware.** Si fallara, el HMAC sigue autenticando, pero no hay confidencialidad. Ver la prueba física 1.
- **Claves:** se generan con `tools/generate-secrets.mjs` en archivos excluidos de Git. Con claves de marcador, ESP-NOW no se activa.

## 9. Canal (radio única en C)

Solo C se asocia al router o al hotspot móvil; A y B nunca llaman a `WiFi.begin()`. El canal guardado (`bioiot_c/ch`) y `BIOIOT_INITIAL_CHANNEL` son **solo una pista inicial**. En el prototipo, el hotspot puede cambiar de canal y de BSSID en cada reinicio.

| Estado de C (`wifi_search_state`) | Canal ESP-NOW |
| --- | --- |
| `connected` | El del AP (lo impone la STA). Si cambió, aumenta `espnow_channel_changes`, se guarda como nueva pista y el HELLO lleva el canal nuevo |
| `waiting_backoff` (sin red) | El último canal adoptado: A y B no tienen que buscar mientras falte Internet |
| `quick_retry_known_channel` | `WiFi.begin(ssid, pass, canal, BSSID)`: sin barrido, no se mueve. Tras perder la red: a los 5 s y 10 s después; máximo `WIFI_QUICK_BEFORE_FULL` (2) por ciclo |
| `full_scan` | `WiFi.begin(ssid, pass)`: busca el SSID en todos los canales, sin BSSID. Interrumpe ESP-NOW unos 2–3 s y después se restaura el canal |
| `portal` | AP `BioIoT-AP` (192.168.4.1) en el mismo canal que ESP-NOW, en AP+STA: la STA de ESP-NOW nunca se apaga |

Plazos tras perder la red: búsqueda completa **a más tardar 45 s** después (`WIFI_FIRST_FULL_SCAN_MS`), o antes si fallan los dos intentos rápidos. Si falla, nuevas búsquedas a 60 s, 120 s, 240 s… hasta 10 min (`WIFI_FULL_SCAN_MIN_MS`/`MAX_MS`). Nunca son continuas: en 30 min sin hotspot, la radio está menos del 1 % del tiempo fuera de canal (prueba `hotspot_off_bounded_searches_and_no_restart`). Al cambiar de canal, C reanuncia su HELLO cada 2 s durante 60 s a los nodos sin trama fresca. A y B lo encuentran con su barrido.

En A y B (`NodeLink`):
- **LOCKED:** si no llega una trama autenticada fresca de C en 20 s, pasa a HUNTING.
- **HUNTING:** recorre 13 canales, empezando por el último bueno, con HELLO y 250 ms de espera por canal (unos 3,3 s).
- **Éxito:** confirma la sesión y guarda el canal solo si cambió.
- **Fracaso:** pasa a BACKOFF en el último canal bueno, con HELLO cada 6 s, y vuelve a barrer tras 30 s → 60 → … hasta 5 min.

La adquisición nunca se detiene. Solo se acepta un gateway que supere HMAC y la sesión (pruebas `node_hunts_gateway_channel_and_persists_it` y `espnow_survives_internet_loss_and_follows_channel_change`).

## 10. Memoria y autonomía offline

| Recurso | Cálculo | Resultado |
| --- | --- | --- |
| Uso del canal | A: 220+137+2×38 B; B: 151+38 B, cada 5 s | ≈ 0,6 KB / 5 s, menos del 1 % a 1 Mbps |
| Cola de A (corte breve) | 24 entradas / 2 tramas por reporte | unos 60 s de reportes (TTL 10 min) |
| Cola de B | 16 entradas / 1 trama por reporte | unos 80 s |
| Instantáneas de C (RAM, no persistentes) | 40 × 920 B = 36,8 KB | 80 min a 120 s; luego se sobrescribe la más antigua y se cuenta `snapshots_dropped` |
| JSON de telemetría 1.1 | ejemplo completo generado | 7,3 KB (búfer MQTT de 24 KB) |
| Reserva única al arrancar en C | GatewayState 48,2 KB + búfer de publicación 24 KB + búfer PubSubClient 24 KB | ~96 KB del heap |

Las instantáneas se publican de la más antigua a la más nueva, con su `timestampUtc` original y un `messageId` estable (`deviceId:bootId:seq`) que permite deduplicar. Al publicar con éxito se marcan publicadas.

**Garantía:** PubSubClient publica con **QoS 0**, así que la entrega es «como mucho una vez por intento»; `publish()==true` significa aceptado por la biblioteca, no recibido por IoT Hub. Un corte de energía en C pierde las instantáneas no publicadas (están en RAM). Para persistirlas haría falta una microSD opcional (no disponible ni implementada); escribir cada muestra en la flash interna se evitó a propósito.
