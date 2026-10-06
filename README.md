# BioIoT Azure Integrated — v4

Firmware para ESP32-WROOM: sensores de fotobiorreactor, calibración persistente y telemetría a Azure IoT Hub por MQTT con TLS. Incluye control manual remoto de un compresor de 12 V mediante relé y una tira RGB analógica de 12 V mediante tres etapas LR7843.

## Diagnóstico de hardware remoto y por Serial

La ampliación actual añade diagnóstico bajo demanda del PCA/TCA9548A y sus ocho canales, multiplexor analógico, TCS3200, DS18B20 y estado ESP32/WiFi/MQTT. Cada telemetría incluye un resumen pequeño, sin escaneo completo periódico. Se conservan GPIO, direcciones, calibraciones, control de actuadores, TLS/SAS, telemetría de 120 s y warm-up de 180 s.

Enviar por C2D o Serial Monitor (115200 baudios, nueva línea):

    {"action":"diagnostics","scope":"full"}

También se admiten i2c, analog, color, system, quick e i2c_recover. El callback solo encola: ejecución y publicación ocurren después en loop().

La [guía de diagnóstico](DIAGNOSTICS.md) incluye todos los comandos exactos, interpretación de errores, los tres problemas observados, funciones añadidas/modificadas, límites de memoria y pruebas. Ejemplos completos: [telemetría normal](examples/telemetry-v1.json) e [informe de diagnóstico](examples/diagnostic-report.json). Los diagnósticos de esos ejemplos son ilustrativos, no una captura del hardware.

Versión actual compilada con Arduino-ESP32 3.3.11: 1.252.084 bytes de programa (95 %) y 56.852 bytes de RAM estática (17 %). Buffer MQTT actual: 32.768 bytes con comprobación de tamaño antes de publicar y protección de heap. No se ha cargado esta ampliación en la placa.

## Cambios realizados

- Sustituido azure_ca.h por el certificado DigiCert Global Root G2 adjunto. Eliminados los certificados corruptos y setInsecure.
- Activada la validación TLS del servidor por CA y nombre de host en el puerto 8883.
- Propiedades de sistema MQTT mediante las macros oficiales AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE y AZ_IOT_MESSAGE_PROPERTIES_CONTENT_ENCODING, para JSON UTF-8 en la ruta nativa IoT Hub → Cosmos DB. El topic se imprime una vez al iniciar.
- Contrato JSON con schema_version="1.0", experiment_id configurable y quality en cada sensor; se conservan las lecturas, calibraciones y null.
- Conservado timestampUtc ISO 8601, además de uptimeMs. Marca el inicio del ciclo de lectura, no la llegada a Azure.
- Añadidos control C2D del compresor y RGB, validación de órdenes, vencimiento de encendidos, reposo mínimo y estados en telemetría.
- GPIO13 reasignado al azul: EN del CD74HC4067 debe conectarse a GND.
- Generación SAS mediante la API pública del SDK, con comprobaciones de errores, tamaños y Base64; renovación dos minutos antes de expirar.
- Reintentos MQTT cada cinco segundos; NTP con límite de 60 segundos y reinicio si falla.
- Comprobación de espacio del paquete MQTT y reporte breve de actuadores después de cambios.

## Archivos

| Archivo | Función |
| --- | --- |
| BioIoT_Azure_Integrated.ino | Sensores, reloj NTP, MQTT, telemetría y despacho C2D |
| azure_ca.h | Certificado raíz TLS |
| AzureIoTSasToken.h / .cpp | Autenticación mediante SAS |
| iot_configs.example.h | Plantilla con valores ficticios; copiar como iot_configs.h (host, identidad y clave privada; excluido de Git) |
| actuator_configs.h | Pines, polaridades, PWM y límites |
| ActuatorControl.h / .cpp | Salidas, validación y estados de actuadores |
| examples/telemetry-v1.json | Ejemplo completo del contrato JSON |
| tests/telemetry-contract.mjs | Pruebas del contrato con entradas simuladas, sin red |

## Preparación

1. Abrir la carpeta como sketch en Arduino IDE y seleccionar ESP32 Dev Module para ESP32-WROOM.
2. Instalar el paquete ESP32 de Espressif y WiFiManager, PubSubClient, Azure SDK for C, OneWire, DallasTemperature, BH1750, DFRobot_OxygenSensor y ArduinoJson. WiFi, Wire, Preferences y WiFiClientSecure vienen con el paquete ESP32.
3. Copiar iot_configs.example.h como iot_configs.h (excluido de Git) y configurar IOT_HUB_HOSTNAME, DEVICE_ID, DEVICE_KEY y EXPERIMENT_ID en iot_configs.h. EXPERIMENT_ID tiene el valor de ejemplo EXP-001: cambiarlo para identificar el ensayo real. La clave es la del dispositivo, no una cadena de conexión completa; mantenerla privada.
4. Opcionalmente definir WIFI_SSID y WIFI_PASSWORD. Sin ellos, WiFiManager utiliza el portal BioIoT-AP.
5. Revisar cableado y polaridades en actuator_configs.h antes de cargar. Todos los .h y .cpp deben permanecer junto al .ino.
6. Abrir el monitor serie a 115200 baudios.

El PWM tiene ramas para Arduino-ESP32 2.x y 3.x. La ampliación actual se ha compilado con 3.3.11; la rama 2.x no ha sido probada.

## Cableado: cambio necesario antes de cargar

El compresor y la tira necesitan una fuente externa de 12 V dimensionada para sus corrientes. Los pines elegidos no son pines de selección de arranque del ESP32-WROOM.

| Señal | GPIO | Destino |
| --- | --- | --- |
| Relé compresor | 32 | Entrada lógica de módulo de relé compatible con 3,3 V |
| Rojo | 33 | Entrada LR7843 del canal R |
| Verde | 4 | Entrada LR7843 del canal G |
| Azul | 13 | Entrada LR7843 del canal B |
| EN del CD74HC4067 | Sin GPIO | Conectar directamente a GND |

**Desconectar EN del GPIO13 y llevarlo a GND antes de cargar v4.** Mantenerlo en GPIO13 haría que el PWM azul habilitara/deshabilitara el multiplexor y alterara las mediciones.

Para una tira RGB analógica de ánodo común se necesitan **tres LR7843, uno por color**. Un solo MOSFET permite regular un canal o el conjunto, pero no tres colores independientes. El común de la tira va a +12 V; R/G/B, a sus respectivas etapas de conmutación hacia GND. Confirmar el pinout de los módulos concretos y compartir la referencia GND entre ESP32 y entradas de control según el esquema del módulo. Un transistor LR7843 suelto no se conecta igual que un módulo con optoacoplador.

Usar COM/NO del relé para que el compresor quede desconectado con el relé desenergizado. Confirmar contactos aptos para la corriente de arranque de la carga DC y protección frente al transitorio inductivo. Los 12 V alimentan las cargas, **nunca un GPIO**. La alimentación/bobina del módulo de relé y su entrada lógica tienen especificaciones distintas.

Configuración predeterminada en actuator_configs.h:

- COMPRESSOR_RELAY_ACTIVE_LOW=true: relé activo en LOW; cambiar a false si activa en HIGH.
- LED_PWM_ACTIVE_LOW=false: etapas activas en HIGH; cambiar si invierten la entrada.
- PWM: 1 kHz, 8 bits, rango 0–255.
- COMPRESSOR_MIN_OFF_MS=180000: tres minutos de reposo, también desde el arranque. Es un valor inicial configurable, **no confirmado por el fabricante**.
- ACTUATOR_MAX_LEASE_SECONDS=900: encendidos con vencimiento máximo a 15 minutos.
- ACTUATOR_DISCONNECT_GRACE_MS=60000: sin conexión con IoT Hub las salidas se mantienen hasta 60 s (renovación horaria del SAS token, cortes WiFi breves) y después se apagan con lastStopReason="cloud_disconnected". Durante ese margen expiresAt se sigue aplicando con el reloj local.
- OFF siempre permitido; no se impone tiempo mínimo de encendido ni se regula la velocidad del compresor.

El programa fija OFF al arrancar. Durante reset/boot los GPIO aún pueden estar en alta impedancia: las etapas deben tener resistencias o estados de reposo adecuados para permanecer apagadas.

## Salidas esperadas

| Orden | Resultado lógico con polaridades predeterminadas |
| --- | --- |
| Compresor OFF | GPIO32 HIGH; relé desenergizado |
| Compresor ON aceptado | GPIO32 LOW; relé energizado |
| RGB apagado | Ciclo útil efectivo 0 en los tres colores |
| R=255, G=0, B=0, brightness=255 | Rojo 100 %; verde y azul 0 % |
| R=255, G=0, B=255, brightness=128 | Rojo y azul aproximadamente 50,2 %; verde 0 % |

Ciclo útil de cada color: round(color × brightness / 255), entre 0 y 255. Si la etapa invierte, el nivel eléctrico se invierte manteniendo el ciclo útil lógico solicitado.

Los estados de telemetría son **valores ordenados a las salidas**. No confirman cierre del relé, giro del motor ni iluminación real: no hay sensores de realimentación de actuadores.

## Comandos Cloud-to-Device

Enviar JSON desde Azure IoT Explorer u otra aplicación C2D a la identidad del dispositivo. Topic de suscripción:

    devices/<DEVICE_ID>/messages/devicebound/#

Acción control, con target igual a compressor o led. commandId es opcional: string de máximo 64 caracteres para correlacionar el último resultado, no un registro de deduplicación.

Todo encendido necesita expiresAt: número entero de segundos Unix UTC futuros, máximo 900 segundos por delante del reloj del ESP32. Las órdenes ya vencidas que quedaron en cola se rechazan al reconectar. Los apagados no requieren expiresAt.

Generar una orden de compresor vigente por cinco minutos en PowerShell; copiar el JSON resultante y enviarlo como cuerpo C2D:

    $expiresAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 300
    @{
        action = "control"
        target = "compressor"
        on = $true
        expiresAt = $expiresAt
        commandId = "compresor-001"
    } | ConvertTo-Json -Compress

Si no terminó el reposo, se rechaza con compressor_min_off_time. **No se programa un arranque diferido**: enviar una nueva orden después del reposo. Una orden válida mientras ya está encendido puede renovar el vencimiento.

Para RGB, enviar siempre r, g, b y brightness como enteros 0–255:

    $expiresAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 300
    @{
        action = "control"
        target = "led"
        r = 255
        g = 0
        b = 255
        brightness = 128
        expiresAt = $expiresAt
        commandId = "rgb-001"
    } | ConvertTo-Json -Compress

Apagar compresor:

    {"action":"control","target":"compressor","on":false,"commandId":"compresor-off"}

Apagar RGB:

    {"action":"control","target":"led","r":0,"g":0,"b":0,"brightness":0,"commandId":"rgb-off"}

Apagar ambos:

    {"action":"all_off","commandId":"todo-off"}

Los valores incorrectos, destinos desconocidos y encendidos vencidos se rechazan sin aplicar la orden. Un JSON que no puede parsearse se registra en el monitor serie.

Después de una orden o un apagado por protección se publica un evento type="actuator_state". actuators.lastCommand contiene commandId, accepted y reason. Es el último estado; puede agrupar cambios muy próximos y no es un historial. PubSubClient publica con QoS 0, por lo que el backend debe comprobar estados posteriores si requiere confirmar la entrega.

## Protecciones y límites

- Arranque: compresor y RGB apagados; no se restauran encendidos desde NVS.
- Vencimiento: se apaga el actuador correspondiente.
- Pérdida de WiFi/MQTT detectada: se apagan ambos. Reconectar no restaura los estados previos; una nueva orden o una orden encolada aún vigente sí puede activarlos.
- Renovación SAS, aproximadamente cada 58 minutos: se apagan ambos antes de reconectar. Para operación continua el backend debe enviar nuevamente consignas vigentes.
- Reboot C2D: apagado antes de reiniciar.
- Fallo al iniciar PWM: ready=false y comandos de encendido bloqueados.
- Las protecciones se revisan en el bucle y después de leer sensores. No son una parada de emergencia de tiempo real: la detección MQTT, las lecturas y llamadas de red pueden retrasar la reacción.

No se han definido umbrales automáticos de oxígeno, horarios de luz, PID ni fotoperiodos. Esta versión ofrece control manual remoto; esos criterios requieren las consignas del proceso.

## Telemetría y Body codificado

Envío de sensores cada 120 segundos. Campos: schema_version, deviceId, experiment_id, timestampUtc, uptimeMs, wifiRssi, status, alerts, calibration, actuators y sensors. EXPERIMENT_ID se toma de iot_configs.h y se escapa como string JSON para conservar comillas, barras y texto UTF-8.

El [ejemplo completo](examples/telemetry-v1.json) contiene los 13 sensores y todos sus datos crudos/calibraciones. Sus valores de sensores proceden del primer mensaje facilitado el 26 de septiembre; la fecha, identificación de experimento y estado apagado de actuadores son ilustrativos, no una nueva captura. Ejemplo parcial:

    {
      "schema_version": "1.0",
      "deviceId": "esp32-bioiot-01",
      "experiment_id": "EXP-001",
      "timestampUtc": "2026-09-26T22:40:00Z",
      "uptimeMs": 240000,
      "actuators": {
        "mode": "manual_c2d",
        "compressor": {"on": false, "expiresAt": 0},
        "led": {"on": true, "r": 255, "g": 0, "b": 255, "brightness": 128,
                "dutyR": 128, "dutyG": 0, "dutyB": 128}
      }
    }

Z indica UTC; 22:40 UTC corresponde a 17:40 en Colombia. uptimeMs mide tiempo desde arranque, no una fecha, y desborda aproximadamente cada 49,7 días. timestampUtc usa el reloj NTP y continúa indicando la fecha real.

Los documentos históricos proporcionados tenían JSON válido en Base64 y no mostraban propiedades de tipo/encoding de contenido. El topic se genera una sola vez con Azure SDK for C y un buffer de 256 bytes. Usa nombres percent-encoded oficiales y **propiedades de sistema**, no campos extra del cuerpo:

    devices/esp32-bioiot-01/messages/events/%24.ct=application%2Fjson&%24.ce=utf-8

Content-Type corresponde a application/json y Content-Encoding a utf-8. Los porcentajes escapan exclusivamente el topic; buildTelemetryJson() se publica directamente mediante payload.c_str(), sin Base64 ni transformación del cuerpo. publishActuatorReport() utiliza exactamente el mismo telemetryTopic.

Cada error de inicialización de properties, cada append de Content-Type/Content-Encoding y la generación del topic tienen un diagnóstico específico. El monitor muestra una sola vez "Telemetry MQTT topic: ...", sin SAS, claves ni cadenas de conexión.

**JSON estructurado no implica aplanar el documento de Cosmos DB.** La ruta puede mantener el envoltorio con Body, SystemProperties, Properties, id y pk. En ese caso los campos se consultan como c.Body.deviceId o c.Body.sensors.temperature.value. Solo si el documento recibido sitúa el payload en la raíz se usa c.deviceId. El firmware controla el contenido y sus propiedades, no el envoltorio del servicio. Véase el [formato de los endpoints de enrutamiento](https://learn.microsoft.com/en-us/azure/iot-hub/iot-hub-devguide-messages-construct#message-schema-at-routing-endpoints).

Los registros históricos siguen intactos. La corrección ocurre en origen; no se añade una Azure Function, decodificación en Django ni migración de documentos.

Los eventos actuator_state incluyen schema_version, deviceId, experiment_id, timestampUtc, type y actuators, pero no sensors. El consumidor debe aceptar ambos tipos de mensaje. Si falta una hora válida, timestampUtc es JSON null en ambos tipos.

### Quality por sensor

| Condición, en orden de prioridad | quality |
| --- | --- |
| Sensor O2 en calentamiento | warming_up |
| connected=false, fuera de calentamiento | disconnected |
| connected=true y valor finito | good |
| connected=true sin valor convertido válido | uncalibrated |

uncalibrated agrupa la conversión deshabilitada (por ejemplo CO2) y valores que las funciones existentes devuelven como null, incluidos fuera de rango o falta de temperatura para convertir DO. Es una etiqueta del contrato solicitado, no una afirmación de que todos esos casos tengan la misma causa. No reemplaza null por cero ni cambia ecuaciones. good tampoco certifica exactitud de calibración.

En TCS3200, que no tiene un campo escalar value, quality usa connected y la validez de H/S/L, conservando pulsos, RGB y HSL. En O2, warming_up tiene prioridad aunque connected=false porque aún no se realizó la lectura.

### Validar en Azure IoT Hub y Cosmos DB

1. Configurar EXPERIMENT_ID y cargar el sketch compilado. Anotar la hora UTC de la carga para distinguir documentos nuevos.
2. En Serial, comprobar la línea única Telemetry MQTT topic con %24.ct=application%2Fjson y %24.ce=utf-8. Debe haber conexión MQTT y "Telemetría enviada.".
3. En IoT Hub, verificar que la ruta existente de mensajes de dispositivo apunta al contenedor Cosmos DB correcto. Al inspeccionar mensajes y sus propiedades, comprobar contentType=application/json y contentEncoding=utf-8 (el nombre mostrado puede variar según la herramienta).
4. Esperar un intervalo de 120 segundos y abrir un documento nuevo en Cosmos DB Data Explorer. Confirmar schema_version="1.0", experiment_id, timestampUtc y quality. Body debe ser un objeto JSON, no una cadena que empieza por eyJ..., si se mantiene el envoltorio.
5. Ejecutar una consulta sobre la estructura real. Con el envoltorio observado anteriormente:

       SELECT TOP 10 c.Body.deviceId AS deviceId,
           c.Body.experiment_id AS experiment_id,
           c.Body.timestampUtc AS timestampUtc,
           c.Body.status AS status,
           c.Body.sensors.temperature.value AS temperature,
           c.Body.sensors.dissolved_oxygen.value AS dissolved_oxygen
       FROM c
       WHERE IS_OBJECT(c.Body)
         AND c.Body.schema_version = "1.0"
         AND c.Body.deviceId = "esp32-bioiot-01"
         AND IS_DEFINED(c.Body.sensors)
       ORDER BY c._ts DESC

   Si el payload está en la raíz, usar c.deviceId, c.timestampUtc, c.status y c.sensors..., filtrando por c.schema_version="1.0". El filtro IS_DEFINED(...sensors) excluye los reportes breves de actuadores. No es necesario decodificar Base64 para consultar los nuevos documentos.

El ESP32 publica con QoS 0: el mensaje de éxito local no demuestra almacenamiento en Cosmos DB. La revisión del documento nuevo es la comprobación de extremo a extremo.

## TLS y autenticación

azure_ca.h contiene exactamente el certificado DigiCert Global Root G2 adjunto, válido hasta 2038-01-15 12:00 UTC. Huella SHA-256:

    CB3CCBB76031E5E0138F8DD39A23F9DE47FFC35E43C1144CEA27D46A5AB1CB5F

Se usa setCACert y el nombre IOT_HUB_HOSTNAME. No hay fallback a setInsecure. Un certificado no confiable, nombre incorrecto o fecha inválida debe impedir la conexión.

La CA autentica al servidor; el dispositivo continúa usando DEVICE_KEY y SAS de una hora. No es necesario migrar la identidad a X.509 para usar TLS con validación del servidor. No se ha cargado una CA en el portal ni modificado credenciales de Azure.

Microsoft recomienda también **Microsoft RSA Root Certificate Authority 2017** para cubrir cadenas alternativas y rotaciones de IoT Hub. Esta entrega integra la raíz G2 adjunta; si el endpoint presenta la otra cadena, añadir la raíz oficial al conjunto PEM, sin desactivar validación.

NTP consulta pool.ntp.org y time.nist.gov antes del primer SAS/TLS. Permitir DNS, NTP y TCP 8883. Los fallos muestran el código MQTT y el error TLS en el monitor serie.

## Sensores y calibración conservados

| Elemento | Conexión |
| --- | --- |
| CD74HC4067 SIG | GPIO36, ADC de 12 bits |
| CD74HC4067 S0 / S1 / S2 / S3 | GPIO25 / 26 / 27 / 14 |
| Canales analógicos 15 / 14 / 13 / 3 / 12 / 11 | pH, CO2 #1, CO2 #2, turbidez, DO, TDS |
| I2C | SDA GPIO21, SCL GPIO22 |
| DFR0576 (TCA9548A) | Dirección 0x77 (DIP A2/A1/A0 = 111) |
| TCA canales 0 / 1 | BH1750 #1 / #2, dirección 0x23 |
| TCA canales 2 / 3 | O2 gas #1 / #2, dirección 0x73 |
| DS18B20 | GPIO23 |
| TCS3200 S0 / S1 / S2 / S3 | GPIO16 / 17 / 18 / 19 |
| TCS3200 OUT #1 / #2 | GPIO34 / 35 |

O2 tiene tres minutos de calentamiento. pH, turbidez y temperatura sí tienen curvas lineales iniciales. DO usa su curva y compensación aproximada por temperatura; TDS usa una fórmula inicial. CO2 queda null hasta habilitar sus coeficientes. Validar las calibraciones experimentalmente.

Comandos C2D conservados, enviar cada objeto por separado:

    {"action":"set","sensor":"ph","m":-6.1125,"b":15.013}
    {"action":"set","sensor":"co2","a":12.34,"b":-0.5}
    {"action":"reset","sensor":"ph"}
    {"action":"reset_all"}
    {"action":"reboot"}

reset y reset_all afectan calibraciones, no apagan cargas; usar all_off para ello. Los coeficientes se guardan en NVS y se incluyen en calibration.

Los comandos de calibración (set, reset, reset_all, calibrate) aceptan un commandId opcional (string, se trunca a 64 caracteres) que se devuelve en el evento type="calibration_ack" para que la UI correlacione la respuesta; si no se envía, commandId es null. reboot se ejecuta un segundo después de confirmar (PUBACK) el mensaje C2D, para que IoT Hub no lo reentregue tras el reinicio.

Operación continua: un watchdog reinicia el equipo si loop() queda bloqueado más de 120 s (las salidas arrancan apagadas). diagnostics.system incluye min_free_heap, max_alloc_heap y reset_reason para vigilar en Cosmos DB la memoria y el motivo del último reinicio.

La detección analógica connected es heurística, basada en extremos del ADC: no garantiza conexión física ni exactitud. EXPECT_TURB=false suprime su alerta obligatoria, pero no omite la lectura.

## Validación y puesta en marcha

Se comprobó la igualdad binaria del certificado incorporado con el adjunto, su autofirma y su huella. También pasó una conexión TLS 1.2 desde el equipo de desarrollo al puerto 8883 del hub, utilizando solo esta CA y verificando el nombre del servidor; no se enviaron credenciales ni mensajes MQTT. Esto verifica el endpoint desde el PC, no sustituye la prueba en ESP32.

El contrato 1.0 compiló para esp32:esp32:esp32 con Arduino-ESP32 3.3.8, ArduinoJson 7.4.3 y Azure SDK for C 1.1.8: 1.222.292 bytes de programa (93 %) y 53.580 bytes de RAM estática (16 %). La partición predeterminada tiene poco margen para funciones adicionales.

Pruebas reproducibles con Node.js, sin instalar paquetes:

    node tests/telemetry-contract.mjs
    node tests/hardware-diagnostics.mjs
    node tests/diagnostic-size.mjs

Los tests evalúan ensamblado JSON y lógica de diagnóstico con entradas/buses simulados; no ejecutan C++ ni sensores. Se complementan con la compilación real. Comprueban contrato, 13 quality, null, escape, calentamiento, publicación diferida, casos de fallo y límites MQTT. El ejemplo normal actual ocupa 2.756 bytes de payload; con topic de 78 bytes, el paquete requiere hasta 2.841 bytes. El informe de ejemplo ocupa 14.281 bytes de payload; el modelo conservador requiere 26.387 bytes de paquete frente al buffer actual de 32.768. Los tamaños se comprueban además en ejecución. Consultar DIAGNOSTICS.md para los límites de estas mediciones y la compilación de la ampliación actual; la cifra de compilación 3.3.8 anterior corresponde a la versión previa sin diagnóstico.

No se ha flasheado la placa ni probado el cableado, las cargas, la recepción C2D o el almacenamiento nuevo en Cosmos DB. Verificación en banco:

1. Con cargas desconectadas, comprobar OFF y polaridad desde arranque.
2. Verificar EN a GND, tres LR7843 y referencia eléctrica de control.
3. Confirmar TLS y un documento nuevo con timestampUtc y Body JSON.
4. Probar rojo, verde, azul y brillo intermedio; comparar duty reportado con las salidas.
5. Probar rechazo de ON antes del reposo del compresor y aceptación después.
6. Probar OFF, all_off, comandos vencidos, valores fuera de rango y desconexión. Una orden rechazada conserva el estado previo salvo las protecciones independientes.
7. Confirmar vencimientos y arranque apagado tras reiniciar.
8. Conectar cargas después de verificar niveles y etapas de potencia.

## Referencias

Curva TCS3200 actualizada el 2026-10-05: referencias provisionales asumidas por el
usuario para ambos sensores, negro 15/15/15 µs y blanco 200/200/200 µs. Se aplica
interpolación lineal con redondeo y límites 0..255; los 10 ms de asentamiento
ocurren antes de medir cada filtro. Detalle y resultados de la captura en
[COLOR.md](espnow_split/docs/COLOR.md). Prueba: `node tests/color-calibration.mjs`.

- [MQTT y propiedades de contenido](https://learn.microsoft.com/en-us/azure/iot-hub/iot-mqtt-connect-to-iot-hub).
- [Formato de salida a Cosmos DB](https://learn.microsoft.com/en-us/azure/iot-hub/iot-hub-devguide-endpoints).
- [Raíces TLS recomendadas por Microsoft](https://learn.microsoft.com/en-us/azure/iot-hub/migrate-tls-certificate).
- [PWM LEDC de Arduino-ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/ledc.html).
