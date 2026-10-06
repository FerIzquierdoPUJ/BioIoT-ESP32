# Migración de calibraciones, respaldo y regreso a v4

Las placas nuevas **no heredan** la NVS del ESP32 v4: arrancan con las curvas iniciales vigentes de v4 (las mismas constantes). Ninguna calibración se borra ni se reinicia al cambiar de arquitectura: la NVS v4 queda intacta en su placa.

## 1. Exportar desde la placa v4 (exacto, recomendado)

1. Conecte la placa que ejecuta v4. Aún no conecte los nodos nuevos.
2. Compile y cargue `tools/BioIoT_CalibrationExport` (ESP32 Dev Module, biblioteca `libraries/BioIoTCommon`). La herramienta:
   - abre la NVS `biocal` **solo en lectura**;
   - reproduce en memoria la migración de esquema de v4;
   - no toca GPIO, Wi-Fi ni Azure.
3. En el monitor serie (115200), copie la línea JSON entre `COPIAR DESDE…` y `FIN` a un archivo, por ejemplo `calibracion-v4.export.json`. Ese patrón de nombre está excluido de Git.
4. Revise `export_meta`: `valid_node_a`, `valid_node_b`, `future_schema` y `co2_*_blob_ok`.
5. Vuelva a cargar `BioIoT_Azure_Integrated.ino` si la placa debe seguir con v4. La NVS sigue intacta, salvo que use *Erase All Flash*.

Cada coeficiente viaja en decimal y como `"<campo>_f32"` (patrón IEEE-754). El importador usa el patrón y verifica que coincide con el decimal, así que la migración es **bit a bit**. ArduinoJson pierde precisión con los decimales ([DISCREPANCIAS.md](DISCREPANCIAS.md) §6).

**Alternativa sin cargar nada en la placa:** `node tools/telemetry-to-calibration-export.mjs documento.json` convierte un documento de telemetría v4 (Cosmos o Serial). Es solo un **respaldo aproximado**: decimales de 6–7 cifras, sin historial de calibración en aire del O2 y sin flags de turbidez ni temperatura (se marca en `export_meta`).

## 2. Importar en los nodos nuevos

**Por Serial** (no requiere red; cada nodo toma su sección). Envíe en una sola línea:

```json
{"action":"calibration_import","force":false,"export":{ …contenido del archivo… }}
```

**Por C2D** a través del gateway (enruta `node_a` y `node_b`; publica un `calibration_ack` por nodo y un resumen `applied|partial|rejected`):

```json
{"action":"calibration_import","commandId":"migr-001","force":false,"export":{ … }}
```

Reglas:
- Un nodo con calibraciones de usuario o importadas rechaza la importación (`rejected_import_not_allowed`) salvo con `"force": true`. Así se evita pisar ajustes más nuevos con una exportación vieja.
- Cualquier campo inválido rechaza la sección completa: no hay importaciones a medias.
- La candidata se guarda y se relee antes de activarse. Si la escritura falla, `rejected_storage_failed` y nada cambia.
- **Nodo B sin LittleFS montado** (placa nueva sin inicializar o fallo de montaje): la importación se rechaza con `rejected_storage_failed` y nada se borra. Inicialice antes la placa nueva ([INSTALACION.md](INSTALACION.md) §3.1).
- Quedan registrados `importedMask`, la revisión por sensor (+1), la revisión del nodo y `legacy_v4_version` (el `ver` de v4).

**Verificación:**
- `{"action":"calibration_export"}` por Serial en cada nodo, o por C2D en el gateway, que publica `type=calibration_export` con las copias confirmadas.
- Compare los campos `_f32` con el archivo exportado.

## 3. Respaldo continuo

- Tras cualquier cambio, el gateway recibe `CALIBRATION_STATE` y lo refleja en `calibration` de cada telemetría, con la revisión por sensor y por nodo.
- `{"action":"calibration_export"}` por C2D produce un respaldo en Cosmos. Guárdelo como archivo y reimpórtelo con `force:true` si sustituye una placa.

## 4. Regreso al firmware anterior (rollback)

1. Desconecte o apague el gateway C. IoT Hub no admite dos sesiones con la misma identidad.
2. Traslade de nuevo sensores y actuadores a la placa v4 según su cableado original: perfil de actuadores v4 32/33/4/13, EN del mux a GND e I2C en 21/22.
3. Cargue `BioIoT_Azure_Integrated.ino` (sin cambios en esta entrega). Si no se borró la flash, su NVS sigue con las calibraciones previas a la migración.
4. Los ajustes hechos en los nodos nuevos **no** vuelven solos a v4: aplíquelos con los comandos C2D `set` de v4 a partir de un `calibration_export`.

## 5. Contrato de telemetría 1.1 (Cosmos DB y aplicación)

Se conservan todos los campos y unidades de 1.0, el transporte JSON UTF-8, las propiedades `$.ct`/`$.ce`, el `telemetryTopic` y la identidad. Los cambios son aditivos salvo tres, inevitables, por los que se versiona `schema_version: "1.1"`:

| Cambio | Motivo | Migración del consumidor |
| --- | --- | --- |
| `calibration.version` = `null` | No existe una revisión global única: cada nodo y sensor tiene la suya (`calibration.<sensor>.revision`, `calibration.nodes.node_x.revision`, `legacy_v4_version`) | Usar las revisiones por sensor |
| `connected` puede ser `null` y `quality` admite `stale` y `node_offline` | Un dato vencido o un nodo sin enlace no prueban desconexión del sensor; nunca se publica un valor viejo como actual (`value=null`) | Tratar `null` como desconocido; alertas `node_x_offline` y `<sensor>_stale` |
| `calibration.<sensor>` puede ser `null` | El gateway aún no recibió la copia confirmada del nodo | Mostrar «sin confirmar» |

Campos nuevos:
- **Por sensor:** `node`, `data_state`, `stale`, `age_ms`, `sampledAtUtc`, `calibration_revision`; en luz, `driver_code`.
- **En `actuators`:** `actuation`, `outputs_driven`, `pin_profile`, `source`, `locked`, `local_program`.
- **Temperatura con dos sondas (2026-10-06):** `sensors.temperature` es la temperatura del sistema: el **promedio** de A (GPIO23) y B en ESP32 (GPIO18) si ambas son actuales y válidas; si solo una lo es, esa; si ninguna, `value=null` con el estado de A como antes. Campos nuevos: `source` (`node_a`/`node_b`/`average`/null) y `sources` (`{"node_a": °C|null, "node_b": °C|null}`); con promedio, `node` vale `node_a+node_b` y `raw` es null. `sensors.temperature_b` publica la sonda de B por separado (sin calibración propia). Alertas nuevas posibles: `temperature_b_disconnected`, `temperature_b_stale`. `{"action":"config","node":"node_b","expected":{"temperature_b":false}}` desactiva sus alertas. C y B (ESP32) deben actualizarse juntos: un C anterior rechaza toda la telemetría de un B que envíe `temperature_b`.
- **Raíz:** `messageId` (deduplicable), `gateway` (`time_status`, `replayed`, `published_delay_ms`, `buffered_unpublished`, `snapshots_dropped`, canales, `wifi_search_state`, `wifi_ms_since_connected` (null si nunca conectó), `espnow_channel_changes`) y `nodes` (enlace y STATUS de A y B).

Consultas Cosmos: los filtros de v4 por `schema_version = "1.0"` deben aceptar `IN ("1.0","1.1")`. Ejemplo con envoltorio `Body`:

```sql
SELECT TOP 10 c.Body.timestampUtc, c.Body.sensors.ph.value, c.Body.sensors.ph.data_state,
       c.Body.sensors.o2_gas_1.quality, c.Body.messageId
FROM c
WHERE IS_OBJECT(c.Body) AND c.Body.schema_version IN ("1.0", "1.1")
  AND c.Body.deviceId = "esp32-bioiot-01" AND IS_DEFINED(c.Body.sensors)
ORDER BY c._ts DESC
```

Con instantáneas reenviadas tras un corte:
- `timestampUtc` y `sensors.*` corresponden al instante original;
- `nodes`, `calibration` y `diagnostics` reflejan el momento de publicación;
- `gateway.replayed=true`.

Para deduplicar en Cosmos, use `messageId`. La garantía es QoS 0, sin *exactly-once*.

Los tipos de evento conservan sus nombres (`actuator_state`, `calibration_ack`, `diagnostic_report`) y se añaden `calibration_export` y `command_result`. Los cambios de *routing* o almacenamiento en Azure quedan fuera de esta entrega.
