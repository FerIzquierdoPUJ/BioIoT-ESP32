# Discrepancias detectadas y cómo se resolvieron

Fuentes contrastadas:
- El mapa «acordado» del encargo.
- El commit `31f6a58`.
- La copia de trabajo sin commit del 2026-10-02 (`git diff`).
- Los README y DIAGNOSTICS de v4.

Ninguna discrepancia se resolvió mezclando perfiles: cada una queda como un perfil explícito y seleccionable.

## 1. Canales del CD74HC4067 (nodo A)

| Sensor | Encargo / commit 31f6a58 | Copia de trabajo v4 (2026-10-02) |
| --- | --- | --- |
| pH | CH0 | CH15 |
| CO2 #1 / #2 | CH1 / CH2 | CH14 / CH13 |
| Turbidez | CH3 | CH3 |
| DO | CH4 | CH12 |
| TDS | CH5 | CH11 |

Los GPIO coinciden en ambos: SIG=36, S0..S3=25/26/27/14, EN a GND, TCS3200 S0..S3=16..19, OUT 34/35, DS18B20 23.

**Resolución:** por defecto `NODE_A_MUX_PROFILE_WORKING_COPY` (15/14/13/3/12/11). Es el cambio más reciente, hecho expresamente después del diagnóstico «analógicos 113–118 frente a cero» de DIAGNOSTICS.md, y es la mejor evidencia disponible del cableado físico. Si el cableado real es CH0..CH5, defina `NODE_A_MUX_PROFILE=NODE_A_MUX_PROFILE_SEQUENTIAL` en `node_a_config.h`. El diagnóstico `analog` informa el perfil activo y la evidencia por canal. **Confirmar físicamente antes de usar los datos.**

## 2. Dirección del PCA9548A/TCA9548A (nodo B)

| Fuente | Dirección | Selectores |
| --- | --- | --- |
| Encargo / commit 31f6a58 | 0x72 | A0=GND, A1=3,3 V, A2=GND |
| Copia de trabajo v4 | 0x77 | DFR0576, DIP A2/A1/A0 = 111 |

v4 marcaba `address_0x72_conflicts_with_tca`: 0x72 cae en el rango 0x70–0x73 seleccionable por los SEN0322, y un O2 configurado en 0x72 chocaría con el multiplexor.

**Resolución:** por defecto 0x77 (`NODE_B_TCA_PROFILE_WORKING_COPY`). El diagnóstico `i2c` de B sondea también la dirección del otro perfil e informa `tca9548a_answers_at_alternate_profile_address` si el multiplexor responde allí. Para 0x72: `NODE_B_TCA_PROFILE=NODE_B_TCA_PROFILE_PROMPT`.

SDA/SCL pasan a GPIO4/GPIO5 del ESP8266 (D2/D1 en NodeMCU v2 y Wemos D1 mini). Los canales 0..3 y las direcciones 0x23/0x73 no cambian.

## 3. Pines de actuadores (nodo C)

| Actuador | Perfil objetivo para C (encargo) | v4 (repositorio) |
| --- | --- | --- |
| Compresor | 13 | 32 |
| Rojo | 32 | 33 |
| Verde | 33 | 4 |
| Azul | 4 | 13 |

**Resolución:** por defecto `ACTUATOR_PIN_PROFILE_TARGET`, como pide el encargo para C. El perfil v4 sigue disponible como `ACTUATOR_PIN_PROFILE_LEGACY_V4` y nunca se mezclan. En C, GPIO13 deja de tener el conflicto de v4 con EN del multiplexor, porque el mux está en A. La bomba (GPIO2) no existía en v4: queda reservada y sin implementar ([PENDIENTES.md](PENDIENTES.md)).

**Cables a trasladar del ESP32 v4 al nodo C (perfil objetivo):**
- Entrada del compresor: GPIO32 → GPIO13.
- Rojo: GPIO33 → GPIO32.
- Verde: GPIO4 → GPIO33.
- Azul: GPIO13 → GPIO4.
- GND común con las etapas de potencia.

Los sensores se trasladan a A o B ([INSTALACION.md](INSTALACION.md)).

## 4. Documentación v4 frente a código v4

| Tema | README/DIAGNOSTICS | Código | Se conservó |
| --- | --- | --- | --- |
| `quality` con valor nulo y conectado | «uncalibrated» | `out_of_range` (`sensorQuality`) | El código |
| Sonda de 0x73 durante el warm-up en el diagnóstico | «No se realiza el probe» | Se probaba (`warmupSkipped` nunca se activaba) | La intención documentada: B omite esa sonda y lo informa (`warming_up_not_tested`) |
| Apagado por SAS | README: se apagan ambos | Código: margen de 60 s | Ninguno: ver «Cambios deliberados» en [FUNCIONES.md](FUNCIONES.md) |

## 5. Hallazgo de seguridad: clave publicada

`iot_configs.h` (con `DEVICE_KEY`) está versionado y `main` se sincroniza con `origin` (`github.com/FerIzquierdoPUJ/BioIoT-ESP32`). Cualquiera con acceso al repositorio puede suplantar el dispositivo.

**Recomendación:**
1. Regenerar la clave primaria y la secundaria del dispositivo en IoT Hub.
2. Retirar el archivo del historial.
3. Añadirlo al `.gitignore` de la raíz.

No se hizo ninguna de estas acciones, porque cambiaría recursos cloud o archivos existentes. Los firmwares nuevos leen `iot_configs.h` de su propia carpeta, que está excluida por `espnow_split/.gitignore`.

## 6. Precisión de los JSON de calibración

ArduinoJson 7.4.3 serializa `float` con 6–7 cifras significativas (0,13351912 → 0,133519) y su parser no redondea exactamente (−6,1125 → −6,11249971). Comprobado con un programa de prueba. Por eso:
- El formato de exportación/importación añade `<campo>_f32` (patrón IEEE-754) y la migración es bit a bit (prueba PC).
- La telemetría muestra coeficientes con la misma precisión limitada que v4. No es una alteración de la curva activa, que reside en el nodo.
