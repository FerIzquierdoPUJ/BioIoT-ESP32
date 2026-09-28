# Diagnóstico de hardware BioIoT

Esta ampliación trabaja sobre el firmware existente. No cambia GPIO, direcciones, canales, fórmulas de calibración, NVS, SAS, certificado TLS ni control de actuadores. Mantiene la telemetría cada 120 s y el calentamiento de O2 de 180 s.

## Uso desde Azure o Serial

Enviar uno de estos cuerpos JSON como mensaje Cloud-to-Device al dispositivo, o escribirlo en Serial Monitor a 115200 baudios con terminación de línea:

    {"action":"diagnostics","scope":"full"}
    {"action":"diagnostics","scope":"i2c"}
    {"action":"diagnostics","scope":"analog"}
    {"action":"diagnostics","scope":"color"}
    {"action":"diagnostics","scope":"system"}
    {"action":"diagnostics","scope":"quick"}
    {"action":"diagnostics","scope":"i2c_recover"}

| scope | Trabajo realizado |
| --- | --- |
| full | Bus principal, ocho canales PCA/TCA, seis analógicos, dos TCS y DS18B20 |
| i2c | Bus principal y ocho canales, selección y errores exactos |
| analog | 16 muestras por canal analógico y estadísticas |
| color | Pulsos y timeouts de ambos TCS |
| system | Heap, mínimo heap, tiempo, WiFi y MQTT |
| quick | Comprobación breve de TCA y último estado analógico/color; no barrido |
| i2c_recover | Evidencia antes/después de Wire.end()/begin(21,22), apagado de canales y escaneo I2C |

Todos incluyen estado del sistema y contadores históricos en RAM. Solo full actualiza la temperatura dentro de este diagnóstico. quick reutiliza las últimas lecturas de color/analógicos y muestra si existen y cuándo se muestrearon.

El callback MQTT únicamente valida y encola la solicitud. loop() ejecuta el trabajo por etapas y publica fuera del callback. Se admite una solicitud a la vez; si ya hay una activa, la nueva se rechaza por Serial. Los comandos anteriores de calibración, control, all_off y reboot se conservan. La entrada Serial añadida acepta únicamente diagnósticos; no abre control de cargas por Serial.

El barrido avanza una dirección por vuelta. Entre etapas se atienden MQTT y las protecciones de actuadores. Si coincide la telemetría de 120 s, el escáner restaura el aislamiento o canal antes de continuar. No habilita dos canales simultáneamente. Al finalizar escribe 0x00 y registra si esa escritura tuvo éxito; no afirma que quedó apagado si no hubo ACK.

No es tiempo real estricto: las lecturas existentes, pulseIn, OneWire y las llamadas de red siguen teniendo latencia. Con timeouts de 50 ms, 1134 probes pueden sumar cerca de un minuto en un bus bloqueado. El firmware no introduce una espera bloqueante de todo ese barrido.

## Salida y ejemplos completos

- [Firmware completo](BioIoT_Azure_Integrated.ino).
- [Telemetría normal](examples/telemetry-v1.json): conserva los campos existentes y añade diagnostics compacto.
- [Informe completo](examples/diagnostic-report.json): type=diagnostic_report con sistema, evidencia I2C, analógicos, color, temperatura, assessment e history.

El informe de ejemplo es sintético: ilustra PCA ausente, analógicos en cero y TCS1 sin pulsos. No es una captura nueva de tu hardware. Los metadatos de diagnóstico añadidos al ejemplo normal también son ilustrativos; sus lecturas de sensores proceden del ejemplo anterior. El firmware calcula los valores reales, no incorpora estas cifras fijas.

Los informes utilizan el mismo telemetryTopic con application/json y utf-8 y la misma conexión TLS/SAS. No se codifica el body en Base64. El consumidor debe distinguir telemetría con sensors, eventos actuator_state e informes diagnostic_report. Si Cosmos conserva el envoltorio, consultar c.Body.type, c.Body.assessment y c.Body.i2c; si almacena el payload en la raíz, omitir Body.

El monitor muestra un resumen humano, errores, estadísticas y el JSON. Si no se logra publicar, conserva el informe en RAM e intenta aproximadamente cada segundo durante hasta 60 s desde el fin del diagnóstico; después lo descarta con aviso. QoS 0 no confirma almacenamiento en Azure. Sin red aún puede verse el resultado por Serial.

## Cómo interpretar los tres problemas observados

### A. Todos los sensores I2C aparecen desconectados

- Si 0x72 no responde: tca9548a_not_detected; downstream not_testable y expected_found=null. No se atribuye daño a los cuatro sensores.
- Si 0x72 responde pero seleccionar un canal falla: tca_channel_select_failed con el código exacto de esa escritura.
- Si seleccionar funciona y falta la dirección esperada: expected_device_missing y tca_chN_expected_device_missing.
- Si faltan los cuatro esperados, después del warm-up: multiple_tca_channels_missing_devices.
- CH0/CH1 esperan 0x23 y CH2/CH3 esperan 0x73. CH4–CH7 libres pueden dar channel_empty o unexpected_device_found.

El bus principal se escanea de 0x01 a 0x7E con canales aislados cuando es posible. Las direcciones que ya respondían en el bus principal se excluyen de devices de los canales: ver 0x72 desde un canal no significa encontrar un sensor downstream. Si falla el aislamiento o la dirección esperada ya aparece upstream, la atribución queda ambigua y no se confirma presencia del sensor.

probe_codes conserva 126 resultados: índice 0 corresponde a 0x01, índice 125 a 0x7E. null significa no probado, no un código Wire inventado. Los ACK indican respuesta del bus, no exactitud de medición ni reparación física.

### B. Analógicos 113–118 frente a cero

Cada canal conserva samples, raw_min, raw_max, raw_avg, raw_span y voltage_avg de 16 muestras. La tensión es una estimación con la escala ADC existente, no una medición calibrada independiente.

all_near_zero requiere los seis promedios <=5. channels_too_similar exige que la diferencia entre máximo y mínimo de los seis promedios sea <=8 cuentas. Por ello [117,118,116,117,113,114] activa similitud, y seis ceros activan ambos indicadores. Son umbrales de diagnóstico, no cambios de calibración.

El resultado señala un posible camino común de alimentación/GND/EN/SIG/multiplexor, sin asegurar cuál falla. EN permanece físicamente a GND y no se mide con el ESP32. connection_confidence expresa evidencia débil o señal plausible, nunca presencia física confirmada. Se conserva connected basado en extremos ADC y la distinción entre desconectado y CO2 sin calibrar (value=null, quality=uncalibrated).

### C. TCS1 sin pulsos, TCS2 con pulsos

Se conservan pulseIn(LOW,30000), S0–S3 compartidos y OUT34/35. Se registran pulsos R/G/B, cada timeout y all_pulses_zero. Si TCS1 da tres ceros y TCS2 da tres pulsos válidos, se generan color_1_individual_path_suspected y tcs_shared_control_lines_likely_working.

La hipótesis apunta a alimentación/OE/OUT/GPIO34 del camino individual, no confirma un componente averiado. También se contempla el caso inverso.

## Errores Wire

Se conserva exactamente el retorno de Wire.endTransmission(), con estas etiquetas:

| Código | Etiqueta | Interpretación |
| --- | --- | --- |
| 0 | ok | Transacción completada/ACK |
| 1 | data_too_long | Convención Wire de longitud; no producido artificialmente |
| 2 | address_nack | NACK/fallo reportado por el core; no identifica inequívocamente la causa eléctrica |
| 3 | data_nack | Convención Wire de NACK de datos; no producido artificialmente |
| 4 | other_error | Otro error, incluyendo condiciones de inicialización |
| 5 | timeout | Se agotó el tiempo de espera |
| Otro | unknown | Se conserva el número aunque no exista etiqueta |

Se inspeccionó Wire del Arduino-ESP32 3.3.11 instalado: esta implementación mapea sus retornos a 0, 2, 4 o 5. En particular 2 también agrupa ESP_FAIL/ESP_ERR_NOT_FOUND; su nombre convencional no prueba por sí solo si falló dirección o datos. Si no se intentó una selección, el informe indica select_attempted=false y error=null.

Un timeout, NACK o ausencia de dispositivos permite acotar el tramo, pero no distinguir por software todos los fallos de alimentación, resistencia pull-up, cable, nivel eléctrico o chip.

## Warm-up, DS18B20 e historial

Durante los 180 s de O2 no se realiza el probe de 0x73 en CH2/CH3 ni se suman fallos de medición. El canal indica testable=false, reason=warming_up y tiempo restante. Se evalúa el calentamiento al llegar al probe: si ya acabó, se realiza; si se omitió, el informe conserva esa evidencia aunque termine el warm-up antes de publicar. Solicitar otro diagnóstico después para comprobarlo.

DS18B20 reporta GPIO23, device_count, temperatura cruda y detected. getDeviceCount() devuelve el conteo almacenado por DallasTemperature en su última enumeración (begin), no un nuevo inventario de conexiones en caliente. Un valor de error crudo se conserva como evidencia, sin convertirlo en temperatura válida.

history contiene éxitos, fallos, fallos consecutivos y últimos instantes de éxito/fallo por grupo. Para el TCA se cuentan las transacciones de probe/selección/apagado. Para BH/O2 se cuentan intentos de medición: un ACK del escáner no cuenta como medición correcta, y no poder seleccionar el canal no acusa al sensor. El multiplexor analógico registra cada lote completo y analog_mux_all_zero_count.

Los contadores residen solo en RAM y se reinician con el ESP32. Los tiempos UTC son null si no existía hora válida; uptime es relativo al arranque y desborda aproximadamente cada 49,7 días. Los contadores también tienen rango finito. El historial ayuda a reconocer intermitencia; no demuestra reparación ni continuidad eléctrica.

## Tamaño, memoria y validación

Compilación real completada para esp32:esp32:esp32 con Arduino-ESP32 3.3.11:

- Programa: 1.252.084 de 1.310.720 bytes (95 %).
- Variables globales: 56.852 bytes (17 %), dejando 270.828 para memoria dinámica/local.
- Buffer MQTT: 32.768 bytes, reservado dinámicamente; no está incluido como bloque estático en esa cifra.
- Ejemplo normal: 2.756 bytes de payload y 2.841 de paquete con topic de 78 bytes.
- Informe completo de ejemplo: 14.281 bytes de payload.
- Modelo conservador del test: 26.387 bytes de paquete, menor que 32.768.

El modelo conservador incluye listas de direcciones máximas, códigos, textos y contadores amplios; no es garantía universal para cualquier identidad o futura extensión. Antes de publicar se comprueba siempre 5 + 2 + strlen(telemetryTopic) + payload.length(). También se registra el tamaño por Serial. No se envía JSON truncado.

ArduinoJson usa un documento dinámico solo al completar el diagnóstico. Antes de construirlo se exige al menos 90.000 bytes de heap libre; se verifican overflowed(), reserva del String y tamaño del paquete. Si no hay recursos se intenta un informe breve de error. El documento se libera antes de publicar. La comprobación no elimina la posibilidad de fragmentación; los picos de heap con TLS requieren medición real en placa. Hay poco margen de flash para nuevas funciones.

Pruebas reproducibles, sin dependencias de Node externas:

    node tests/telemetry-contract.mjs
    node tests/hardware-diagnostics.mjs
    node tests/diagnostic-size.mjs

Las tres pasan. Las pruebas Node usan entradas/buses simulados y fragmentos restringidos del código; no ejecutan C++ ni hardware real. Cubren contrato JSON, casos A/B/C, PCA ausente, canales vacíos, fallo de selección, warm-up, recuperación, restauración del canal tras telemetría, callback que encola y tamaños MQTT. Se complementaron con la compilación real, sin errores.

No se flasheó el ESP32 ni se probó esta ampliación contra Azure o los sensores físicos. Para verificar: cargar con cargas desconectadas, revisar quick, esperar el warm-up y solicitar full; guardar JSON/Serial y comparar el informe nuevo en IoT Hub/Cosmos. Confirmar también las protecciones y comandos existentes antes de conectar cargas.

## Funciones existentes modificadas

- detectI2C(): delega en el probe detallado.
- tcaSelect(): conserva retorno booleano y delega la selección detallada.
- readAnalogMux(): añade estadísticas sin cambiar el cálculo previo ni connected.
- readRawTemperatureC(): captura evidencia DS18B20 e historial.
- readBh1750Lux(): captura selección, probe, lectura e historial.
- readO2Percent(): conserva warm-up y registra evidencia fuera de él.
- readTcs3200(): conserva adquisición/conversión y añade evidencia/historial.
- mqttCallback(): añade únicamente el encolado de diagnostics.
- buildTelemetryJson(): añade el resumen compacto y actualiza estadísticas de grupo.
- loop(): atiende Serial, máquina de estados y publicación diferida.

No se modificaron en esta ampliación los archivos de actuadores, credenciales/configuración, certificado ni SAS. Se verificaron sus hashes contra el estado previo.

## Funciones nuevas

- i2cErrorName()
- diagnosticAddressText()
- recordDiagnosticHistory()
- probeI2C()
- tcaSelectDetailed()
- tcaDisableAll()
- prepareI2CReading()
- probeI2CReading()
- finishI2CReading()
- updateAnalogDiagnostics()
- writeDiagnosticUtc()
- writeDiagnosticSystem()
- diagnosticColorPulseOk()
- diagnosticColorAllZero()
- writeQuickDiagnostics()
- quickDiagnosticsJson()
- diagnosticScopeName()
- requestDiagnostics()
- pollDiagnosticSerial()
- scanMainI2CBus()
- scanTcaChannels()
- diagnosticNextGroup()
- diagnosticExpectedAddress()
- processDiagnostics()
- writeDiagnosticHistory()
- writeDiagnosticHistories()
- writeScanDevices()
- writeScanErrors()
- writeI2CReadingEvidence()
- writeDiagnosticI2C()
- writeDiagnosticAnalog()
- writeDiagnosticColor()
- writeDiagnosticTemperature()
- printDiagnosticSummary()
- publishDiagnosticReport()

