# Instalación, compilación, carga y emparejamiento

## 1. Versiones fijadas

| Componente | Versión | Usado por |
| --- | --- | --- |
| arduino-cli | ≥ 1.1 (verificado con 1.5.1, el incluido en Arduino IDE 2) | todos |
| Arduino-ESP32 (`esp32:esp32`) | 3.3.12 | A, C |
| ESP8266 core (`esp8266:esp8266`) | 3.1.2 | B |
| ArduinoJson | 7.4.3 | A, B, C |
| OneWire / DallasTemperature | 2.3.8 / 4.0.6 | A |
| BH1750 (claws) | 1.3.0 | B |
| PubSubClient | 2.8 | C |
| Azure SDK for C | 1.1.8 | C |
| WiFiManager (tzapu) | 2.0.17 | C |
| BioIoTCommon | 1.0.0 (local, `../libraries/BioIoTCommon`) | A, B, C |

Cada sketch tiene un `sketch.yaml` con estos perfiles. Con `arduino-cli compile --profile …`, arduino-cli descarga exactamente esas versiones en su caché, sin tocar las bibliotecas del IDE.

## 2. Compilar

### Opción reproducible (perfiles)

```powershell
cd BioIoT_Azure_Integrated\espnow_split
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
& $cli compile --profile nodo_a          BioIoT_NodeA_Sensors
& $cli compile --profile nodo_b_nodemcu  BioIoT_NodeB_I2C      # o nodo_b_d1mini
& $cli compile --profile nodo_c          BioIoT_NodeC_Gateway
```

### Opción rápida (núcleos ya instalados)

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-all.ps1 -Original
```

`-Original` compila también el firmware v4 sin cambios. Si el núcleo ESP8266 no está instalado:

```powershell
& $cli core install esp8266:esp8266@3.1.2 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
```

En Windows, si aparece `cc1plus: No such file or directory`, la ruta de instalación es demasiado larga (más de 260 caracteres). Instale en una ruta corta o mapee una unidad con `subst`.

### Arduino IDE 2 (interfaz gráfica)

1. Copie `libraries/BioIoTCommon` a `Documentos\Arduino\libraries\` (o *Sketch → Include Library → Add .ZIP Library* sobre esa carpeta comprimida).
2. Abra la carpeta de cada nodo como sketch.
3. Placas:
   - A: **ESP32 Dev Module**.
   - B: **NodeMCU 1.0 (ESP-12E Module)** o **LOLIN(WEMOS) D1 R2 & mini**, con *Flash Size: 4MB (FS:1MB OTA:~1019KB)*.
   - C: **ESP32 Dev Module**, *Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA)*.

Resultados verificados ([PRUEBAS.md](PRUEBAS.md)):

| Nodo | Flash | RAM estática |
| --- | --- | --- |
| A | 964.000 B (73 %) | 58.160 B |
| B | IROM 333.276 B, IRAM 28.595 B | 49.268 B de 80.192 B (2026-10-05, con la corrección de LittleFS) |
| C (`min_spiffs`) | 1.233.661 B (62 % de 1,9 MB) | 59.156 B |

C con la partición por defecto ocupa el 94 % (compila, pero con poco margen).

## 3. Cargar (en este orden)

No se programó ninguna placa durante esta implementación. Cuando lo haga:

1. **Exportar las calibraciones de la placa v4 antes de nada** ([MIGRACION.md](MIGRACION.md)).
   Si la placa B es nueva, inicialice antes su LittleFS (sección 3.1).
2. Cargue los tres firmwares con los **secretos de marcador** (sin generar) y sin cargas conectadas. Abra el monitor serie a 115200 y envíe `{"action":"pairing_info"}` a cada placa para anotar su MAC STA. En este modo, las placas miden y responden por serial, pero ESP-NOW permanece desactivado.
3. Genere los secretos (paso 4) y vuelva a compilar y cargar **los tres** firmwares.

```powershell
& $cli upload --profile nodo_a -p COM5 BioIoT_NodeA_Sensors
& $cli upload --profile nodo_b_nodemcu -p COM6 BioIoT_NodeB_I2C
& $cli upload --profile nodo_c -p COM7 BioIoT_NodeC_Gateway
```

### 3.1 Placa B nueva: inicializar LittleFS (una sola vez)

El firmware normal de B **nunca formatea LittleFS**. Tiene el autoformato desactivado con `LittleFSConfig::setAutoFormat(false)` y no incluye ninguna llamada a `LittleFS.format()`. En una placa nueva, LittleFS no monta y el monitor serie muestra `ALMACENAMIENTO NO DISPONIBLE (mount_failed_not_formatted)`. Esa placa mide igual, pero rechaza calibraciones, importaciones y configuración con `rejected_storage_failed`. Para prepararla, siga estos pasos:

> No aplique este procedimiento a una placa B que **ya tenía calibraciones**. En ese caso, que LittleFS no monte es una avería; vea «Fallo de montaje en una placa en servicio» más abajo.

1. **Firmware de puesta en marcha** (bandera solo en la línea de comandos; no edita archivos):

   ```powershell
   & $cli compile --profile nodo_b_nodemcu --clean `
     --build-property "compiler.cpp.extra_flags=-DNODE_B_STORAGE_COMMISSIONING=1" `
     -u -p COM6 BioIoT_NodeB_I2C
   ```

   La compilación debe mostrar `warning: #warning "NODE_B_STORAGE_COMMISSIONING=1: ... No usar en produccion."`. Este firmware **no activa ESP-NOW**, aunque haya secretos reales.

2. **Monitor serie a 115200.** Debe aparecer:

   ```text
   ALMACENAMIENTO NO DISPONIBLE (mount_failed_not_formatted): LittleFS NO se formatea. ...
   PUESTA EN MARCHA: para formatear SOLO esta placa envie {"action":"storage_format","confirm":"FORMAT-NODE-B-XXXXXX"}
   ESP-NOW DESHABILITADO: firmware de PUESTA EN MARCHA de LittleFS (NODE_B_STORAGE_COMMISSIONING=1).
   ```

   `XXXXXX` es el `ESP.getChipId()` de esa placa en hexadecimal. Si en cambio LittleFS monta (`{"action":"status"}` devuelve `storage_mounted: true`), no hay que formatear: pase al paso 4.

3. **Orden expresa.** Copie y envíe la línea exacta impresa, terminada en salto de línea. Respuesta esperada:

   ```json
   {"result":"formatted","storage_mounted":true,"storage_state":"ok"}
   ```

   La placa se niega a formatear en estos casos:

   | Respuesta | Causa |
   | --- | --- |
   | `storage_already_mounted_not_formatted` | LittleFS monta (puede contener calibraciones) |
   | `confirm_token_mismatch` | El token es de otra placa o está incompleto |
   | `no_fs_partition_check_flash_size` | FQBN sin partición FS: use `eesz=4M1M` (perfil `nodo_b_nodemcu`) |

4. **Volver al modo normal** (obligatorio). Recompile sin la propiedad y con `--clean`, para no reutilizar objetos de la puesta en marcha:

   ```powershell
   & $cli compile --profile nodo_b_nodemcu --clean -u -p COM6 BioIoT_NodeB_I2C
   ```

   Comprobaciones:
   - La compilación **no** muestra el `#warning`.
   - Al arrancar no aparecen `PUESTA EN MARCHA` ni `ALMACENAMIENTO NO DISPONIBLE`.
   - `{"action":"status"}` devuelve `storage_mounted: true` y `storage_state: "ok"`.
   - `{"action":"storage_format","confirm":"..."}` responde `storage_format_requires_commissioning_build`.

5. Importe las calibraciones ([MIGRACION.md](MIGRACION.md)), reinicie B y confirme con `{"action":"calibration_export"}` que siguen ahí.

**Arduino IDE:** en `node_b_config.h`, cambie temporalmente `#define NODE_B_STORAGE_COMMISSIONING 0` a `1`, cargue el firmware y siga los pasos 2 y 3. Después **vuelva a `0`** y cargue de nuevo. No versione el archivo con `1`.

**Cargas posteriores:** la carga normal (`Erase Flash: Only Sketch`, `wipe=none`, opción por defecto) conserva LittleFS. **No** use `All Flash Contents`: borra las calibraciones. Tampoco cambie `eesz`: otro tamaño de FS mueve la partición y LittleFS dejará de montar.

#### Fallo de montaje en una placa en servicio

B sigue midiendo con correcciones O2 por defecto en RAM y no modifica la flash. Las señales son `storage_mounted: false` en `status` y diagnóstico, `calibration.nodes.node_b.storage = "write_failed"` en telemetría y `rejected_storage_failed` en cualquier calibración. El aviso se repite en el monitor serie cada 60 s. Antes de pensar en formatear:

1. Confirme que se cargó con `eesz=4M1M` (perfil `nodo_b_nodemcu` o `nodo_b_d1mini`) y reinicie: un fallo transitorio se recupera solo, porque los datos no se borraron.
2. Respalde la partición completa (con 4M1M empieza en `0x300000` y mide `0xFA000`):

   ```powershell
   $py = "$env:LOCALAPPDATA\Arduino15\packages\esp8266\tools\python3\3.7.2-post1\python3.exe"
   $et = "$env:LOCALAPPDATA\Arduino15\packages\esp8266\hardware\esp8266\3.1.2\tools\esptool\esptool.py"
   & $py $et --port COM6 read_flash 0x300000 0xFA000 nodo_b_littlefs_respaldo.bin
   ```

3. Solo si dispone de un `calibration_export` reciente (en Cosmos o en archivo) que pueda reimportar, aplique el procedimiento 3.1 y reimporte con `force:true`.

**C (gateway) y Azure:**
1. Copie `iot_configs.example.h` → `iot_configs.h` y complete el host, `DEVICE_ID`, `EXPERIMENT_ID` y `DEVICE_KEY`.
2. Para conservar la identidad v4 (`esp32-bioiot-01`), **desconecte primero la placa v4**: IoT Hub no admite dos conexiones simultáneas con la misma identidad.
3. Wi-Fi de C (solo C se conecta; A y B nunca):
   - **Orden de credenciales:** primero las guardadas en NVS por WiFiManager. Si no hay ninguna, usa `WIFI_SSID`/`WIFI_PASSWORD` de `gateway_secrets.h`. Las predeterminadas nunca sustituyen a las guardadas.
   - **Portal automático:** si el primer intento no conecta en ≤ 15 s (o antes, si la contraseña es incorrecta o el SSID no existe), o si no hay credenciales, se abre el portal `BioIoT-AP`.
     - Es abierto (sin contraseña) y responde en http://192.168.4.1.
     - Dura 180 s y no bloquea C: ESP-NOW, A/B, colas y actuadores siguen funcionando.
     - Usa el mismo canal que ESP-NOW.
     - `{"action":"wifi_portal"}` por Serial lo abre en cualquier momento. Si C está conectado, sigue conectado.
   - **Guardado:** la red elegida en el portal se guarda en NVS **solo si conecta**, y se reutiliza tras reiniciar. Una contraseña errónea en el portal no borra la red buena.
   - **Hotspot móvil:** el canal guardado es solo una pista inicial; C adopta el canal del hotspot. Tras perder la red, C hace 2 intentos rápidos en el canal y BSSID conocidos y una búsqueda completa del SSID en ≤ 45 s. Después sigue con búsquedas espaciadas de 60 s a 10 min. C no se reinicia por falta de Wi-Fi, DNS, NTP ni Azure.
   - **`{"action":"status"}`:**
     - `wifi_state`, `wifi_search_state`, `wifi_channel`, `espnow_channel`;
     - `wifi_ms_since_connected`, `espnow_channel_changes`, `wifi_next_full_scan_s`;
     - `wifi_attempts`, `wifi_full_scans`;
     - `wifi_credential_source` (`stored_nvs`/`default_gateway_secrets`, nunca el SSID);
     - `wifi_portal_open`, `wifi_portal_remaining_s`.
     
     La telemetría (`gateway.*`) incluye `wifi_search_state`, `wifi_ms_since_connected` y `espnow_channel_changes`. Ningún registro muestra el SSID, la contraseña ni las claves.

## 4. Emparejamiento paso a paso

```powershell
node tools\generate-secrets.mjs --gateway-mac 24:6F:28:AA:BB:CC `
     --node-a-mac 24:6F:28:11:22:33 --node-b-mac 5C:CF:7F:44:55:66 `
     --channel 6 --ssid "RedLab-2.4GHz" --password "********"
```

- Crea `node_secrets.h` en A y B y `gateway_secrets.h` en C, con `systemId` y PMK comunes y LMK más clave HMAC **propias de cada enlace**. No imprime claves y no sobrescribe sin `--force`.
- Use como `--channel` el canal fijo de su router de 2,4 GHz (recomendado). Si el router cambia de canal, los nodos lo buscan solos.
- Los archivos están en `.gitignore`. Guárdelos fuera del repositorio (gestor de secretos). Para retirar un nodo o una clave comprometida, regenere todo con `--force` y vuelva a cargar los tres.

**Verificación** (monitor serie):
1. Cada placa muestra `Autoprueba de protocolo: OK` y `ESP-NOW cifrado activo en canal N`.
2. A y B muestran `Sesion con gateway confirmada`.
3. `{"action":"status"}` en C muestra `node_a.online=true` y `node_b.online=true`.

## 5. Cableado por nodo

No hay conexiones UART ni de datos entre nodos: solo radio. Cada nodo necesita alimentación propia o compartida, con **GND común con sus sensores** y niveles de 3,3 V en las señales.

### Nodo A — ESP32 WROOM

| Componente | Pin ESP32 | Nota |
| --- | --- | --- |
| CD74HC4067 SIG | GPIO36 | ADC1, 0–3,3 V |
| CD74HC4067 S0 / S1 / S2 / S3 | GPIO25 / 26 / 27 / 14 | |
| CD74HC4067 EN | **GND** | no es un GPIO |
| pH / CO2 #1 / CO2 #2 / Turbidez / DO / TDS | canales 15 / 14 / 13 / 3 / 12 / 11 | perfil por defecto; alternativa 0..5 ([DISCREPANCIAS.md](DISCREPANCIAS.md)) |
| TCS3200 S0 / S1 / S2 / S3 (compartidos) | GPIO16 / 17 / 18 / 19 | S0=H, S1=L (escala 20 %) |
| TCS3200 OUT #1 / OUT #2 | GPIO34 / 35 | solo entrada |
| DS18B20 datos | **GPIO22** | pull-up de 4,7 kΩ a 3,3 V (v4 usaba GPIO23; `ONE_WIRE_BUS` en `node_a_config.h`) |

El acondicionamiento eléctrico (divisor del CO2 de 12 kΩ/22 kΩ, ganancia 8,5) y la escala ADC no cambian: las curvas siguen interpretando el mismo voltaje.

### Nodo B — ESP8266 (NodeMCU v2 o Wemos D1 mini)

| Componente | Pin | Nota |
| --- | --- | --- |
| SDA | GPIO4 (D2) | pull-up de 3,3 V en el módulo |
| SCL | GPIO5 (D1) | |
| PCA9548A/TCA9548A (DFR0576) | 0x77 (DIP A2/A1/A0=111) | o 0x72 con el perfil alternativo |
| Canal 0 / 1 | BH1750 #1 / #2 (0x23) | |
| Canal 2 / 3 | SEN0322 #1 / #2 (0x73) | **verificar los selectores de cada sensor** |

`O2_SENSOR_SHARED_SUPPLY=1` supone que los SEN0322 se alimentan del mismo 3,3 V que el ESP8266. Si no es así, ponga 0: el warm-up será completo en cada arranque. No use ESP-01: no expone GPIO4 ni GPIO5 (error de compilación).

### Nodo B — ESP32 WROOM (`BioIoT_NodeB_ESP32`, alternativa al ESP8266)

Mismo nodo B para C (misma identidad, protocolo, calibraciones O2 y warm-up). Se carga **uno** de los dos firmwares de B, nunca ambos a la vez. Placa: **ESP32 Dev Module**, partición por defecto. Calibraciones en NVS de doble ranura (namespace `bioiot_b`): no hay LittleFS ni procedimiento de formateo.

| Componente | Pin ESP32 | Nota |
| --- | --- | --- |
| SDA | **GPIO21** | el mismo I2C que el firmware v4 en ESP32 |
| SCL | **GPIO22** | |
| DFR0576 (TCA9548A) VCC / GND | 3V3 / GND | GND común con los sensores |
| DFR0576 (TCA9548A) | 0x77 (DIP A2/A1/A0=111) | |
| Canal 0 / 1 | BH1750 #1 / #2 (0x23) | sin cambios |
| Canal 2 / 3 | SEN0322 #1 / #2 (0x73) | sin cambios |

**Cambio de placa B sin regenerar claves:** las claves no dependen de la MAC.
1. `BioIoT_NodeB_ESP32/node_secrets.h` es el mismo archivo que el de `BioIoT_NodeB_I2C` (claves del enlace B↔C y MAC del gateway, que no cambian).
2. Cargar B (ESP32) y leer su MAC con `{"action":"pairing_info"}`.
3. En `BioIoT_NodeC_Gateway/gateway_secrets.h` cambiar **solo** `BIOIOT_NODE_B_MAC` por esa MAC y recargar C.
4. A no cambia. Regenerar con `--force` también funciona, pero cambia las claves de los tres enlaces y obliga a recargar A, B y C (el generador escribe ambas carpetas de B).

### Nodo C — ESP32 WROOM (perfil objetivo)

| Señal | GPIO | Destino | Estado por defecto |
| --- | --- | --- | --- |
| Compresor | 13 | entrada lógica de la etapa del compresor (pendiente) | alta impedancia (puesta en marcha) |
| LED R / G / B | 32 / 33 / 4 | entradas de las etapas MOSFET de 12 V (posible LR7843 + opto, pendiente) | alta impedancia |
| Bomba | 2 (reservado) | no implementada | sin usar |

En alta impedancia, cada etapa necesita su propia resistencia de reposo para quedar apagada. Los 12 V (o la red AC, si el compresor fuera de AC) alimentan las cargas, **nunca un GPIO**. GND de control común según el esquema del módulo; si el módulo tiene optoacoplador, ver su hoja de datos.

## 6. Pasar a producción (solo con cargas confirmadas)

En `gateway_config.h`:
1. Confirme la etapa, la polaridad y la frecuencia máxima ([PENDIENTES.md](PENDIENTES.md)).
2. Defina `COMPRESSOR_ACTIVE_LOW`, `LED_PWM_ACTIVE_LOW` y `LED_PWM_MAX_FREQUENCY_HZ`.
3. Ponga `*_ELECTRICAL_CONFIRMED 1` y por último `ACTUATOR_COMMISSIONING_MODE 0`.

La compilación falla si falta un parámetro requerido. Haga la primera prueba con cargas desconectadas y midiendo los niveles.
