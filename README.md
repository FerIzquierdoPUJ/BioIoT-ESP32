# BioIoT Azure Integrated

Sketch integrado para ESP32 WROOM con sensores de fotobiorreactor, CD74HC4067, TCA9548A y Azure IoT Hub por MQTT.

## Antes de compilar

1. Abre `iot_configs.h` y configura:
   - `IOT_HUB_HOSTNAME`
   - `DEVICE_ID`
   - `DEVICE_KEY`
   - opcionalmente `WIFI_SSID` y `WIFI_PASSWORD`

2. Instala librerías en Arduino IDE:
   - WiFiManager
   - PubSubClient
   - Azure SDK for C
   - OneWire
   - DallasTemperature
   - BH1750

3. Dirección del TCA9548A:
   - El código usa `TCA_ADDR 0x72` para evitar conflicto con sensores O2 SEN0322 si usan 0x70/0x71.
   - Si tu TCA está en 0x70, cambia `TCA_ADDR`, pero evita tener un sensor downstream con la misma dirección activa.

## Mapa sugerido

### CD74HC4067
- SIG -> GPIO36
- S0 -> GPIO25
- S1 -> GPIO26
- S2 -> GPIO27
- S3 -> GPIO14
- EN -> GPIO13

Canales:
- CH0 pH
- CH1 CO2 #1
- CH2 CO2 #2
- CH3 Turbidez
- CH4 Oxígeno disuelto
- CH5 TDS

### TCA9548A
- SDA -> GPIO21
- SCL -> GPIO22
- Canal 0: BH1750 #1
- Canal 1: BH1750 #2
- Canal 2: O2 gas #1
- Canal 3: O2 gas #2

### DS18B20
- DATA -> GPIO23

### TCS3200
- S0 -> GPIO16
- S1 -> GPIO17
- S2 -> GPIO18
- S3 -> GPIO19
- OUT #1 -> GPIO34
- OUT #2 -> GPIO35

## Nota sobre calibración

El sketch envía valores crudos y voltaje. Solo TDS y DO tienen estimación inicial. pH, CO2 y turbidez quedan como `null` hasta reemplazar las funciones placeholder por las curvas reales de calibración.

## Alertas

El JSON incluye:
- `status`: `ok` o `warning`
- `alerts`: sensores esperados que aparecen desconectados
- cada sensor incluye `expected`, `connected`, `raw/voltage/value/unit`
