# Ejemplos de telemetría, comandos y resultados

Los JSON de [examples/](examples) los genera el propio código del gateway (`tests/host/test_examples.cpp` con `BIOIOT_WRITE_EXAMPLES=docs/examples`) a partir de **datos sintéticos**. No son capturas de hardware y no contienen secretos.

| Archivo | Contenido |
| --- | --- |
| [telemetry-v1.1.json](examples/telemetry-v1.1.json) | Telemetría 1.1 completa: 13 sensores, A en línea, B con O2 en calentamiento, color_1 sin pulsos, puesta en marcha (7,3 KB compactos) |
| [calibration-ack-pending.json](examples/calibration-ack-pending.json) | Calibración aceptada por el gateway y aún no confirmada por el nodo |
| [calibration-ack-applied.json](examples/calibration-ack-applied.json) | Aplicada **y** guardada (`status: saved`, `nvs_ok: true`) |
| [calibration-ack-timeout.json](examples/calibration-ack-timeout.json) | Sin respuesta del nodo B dentro del plazo |
| [reset-all-partial.json](examples/reset-all-partial.json) | `reset_all` aplicado en un nodo de dos: `partial` |
| [actuator-state-commissioning.json](examples/actuator-state-commissioning.json) | Encendido aceptado y **simulado** (`outputs_driven: false`) |
| [calibration-export.json](examples/calibration-export.json) | Respaldo de las copias confirmadas, con `_f32` |
| [diagnostic-report-aggregated.json](examples/diagnostic-report-aggregated.json) | Diagnóstico `full`: informe de A integrado y B `node_offline` |

## Comandos C2D (cuerpo JSON; `commandId` opcional pero recomendado)

Los nombres y validaciones de v4 se mantienen. El gateway los enruta al nodo propietario.

```json
{"action":"set","sensor":"ph","m":-6.1125,"b":15.013,"commandId":"cal-ph-001"}
{"action":"set","sensor":"co2_2","zero_point_v":0.31,"reaction_voltage_v":0.025,"commandId":"cal-co2-2"}
{"action":"set","sensor":"o2_gas_1","gain":1.02,"offset":-0.1}
{"action":"reset","sensor":"turb"}
{"action":"reset_all","commandId":"reset-all-001"}
{"action":"calibrate","sensor":"o2_gas_1","reference_vol":20.9,"commandId":"o2-air-1"}
{"action":"set","sensor":"ph","m":-6.1,"b":15.0,"expiresAt":1790000600}
```

- `expiresAt` (UTC en segundos) en una calibración es opcional. Si vence antes de entregarse, el estado es `expired` y no se envía.
- `reset_all` se aplica por nodo y se resume como `applied`, `partial` o `rejected`, sin falsa atomicidad.

Actuadores (locales en C; `expiresAt` como máximo 900 s en el futuro):

```powershell
$e = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 300
@{action="control"; target="compressor"; on=$true; expiresAt=$e; commandId="compresor-001"} | ConvertTo-Json -Compress
@{action="control"; target="led"; r=255; g=0; b=255; brightness=128; expiresAt=$e; commandId="rgb-001"} | ConvertTo-Json -Compress
```

```json
{"action":"control","target":"compressor","on":false,"commandId":"compresor-off"}
{"action":"all_off","commandId":"todo-off"}
```

Sistema, diagnóstico y configuración:

```json
{"action":"diagnostics","scope":"full"}
{"action":"diagnostics","scope":"i2c_recover"}
{"action":"diagnostics","scope":"quick"}
{"action":"reboot"}
{"action":"reboot","node":"node_b"}
{"action":"reboot","node":"all"}
{"action":"config","node":"node_a","report_interval_s":5,"expected":{"turbidity":true}}
{"action":"calibration_export","commandId":"export-001"}
{"action":"calibration_import","commandId":"migr-001","force":false,"export":{ "...": "..." }}
```

| Ámbito del diagnóstico | Nodos consultados |
| --- | --- |
| `full` | C + A (analógico, color, temperatura, sistema) + B (I2C completo, O2, luz, sistema) |
| `i2c`, `i2c_recover`, `o2` | B |
| `analog`, `color`, `temperature` | A |
| `system` | C + A + B |
| `quick` | Solo C, con el último STATUS de cada nodo, sin sondeos |

Una petición de diagnóstico a la vez; otra simultánea se rechaza con `diagnostics_busy`. `reboot` sin `node` reinicia solo el gateway, tras el PUBACK, como en v4. Con `all` el gateway reinicia 5 s después de enviar las órdenes a A y B.

## Serial (115200, una línea por comando)

| Nodo | Acciones |
| --- | --- |
| A | `diagnostics` (`full`, `analog`, `color`, `temperature`, `system`, `quick`), `status`, `pairing_info`, `calibration_export`, `calibration_import` |
| B | `diagnostics` (`full`, `i2c`, `i2c_recover`, `o2`, `system`, `quick`), `status`, `pairing_info`, `calibration_export`, `calibration_import` |
| C | `diagnostics`, `status`, `pairing_info`, `wifi_portal` |

Como en v4, el serial del gateway no controla cargas ni calibraciones.
