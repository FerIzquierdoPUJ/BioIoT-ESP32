# Curva provisional de los dos TCS3200

Actualización 2026-10-05: el usuario indicó que COLOR #1 observa negro y COLOR #2
blanco, y pidió asumir **para ambos sensores y los tres canales** negro = 15 µs
y blanco = 200 µs. Estos extremos son asumidos, no una calibración física medida.

Se aplica una curva empírica lineal en el ancho del pulso:

```text
RGB = redondear(255 × (pulso − 15) / (200 − 15)), limitado a 0..255
```

Un pulso cero o ≥30000 µs conserva el tratamiento de fallo/timeout. La conexión
se determina con los pulsos crudos, no con el RGB: un negro válido no se considera
desconectado. HSL se calcula a partir del RGB corregido, con negro H/S/L = 0/0/0
y blanco = 0/0/1.

| Referencia o captura | Pulsos R/G/B (µs) | RGB corregido |
| --- | --- | --- |
| Negro asumido, cualquiera de los sensores | 15/15/15 | 0/0/0 |
| Blanco asumido, cualquiera de los sensores | 200/200/200 | 255/255/255 |
| Captura COLOR #1 | 23/22/16 | 11/10/1 |
| Captura COLOR #2 | 142/192/153 | 175/244/190 |

La captura #2 queda clara con tinte verde; con los extremos solicitados no resulta
blanco puro. Para corregir ese tinte hacen falta referencias medidas por canal
y por sensor bajo la iluminación y distancia de trabajo.

La curva anterior usaba 50..30000 µs → 255..0 y saturaba ambos sensores cerca de
blanco. Los parámetros nuevos están en `TCS_BLACK_PULSE_US` y
`TCS_WHITE_PULSE_US`, tanto en el sketch integrado como en `node_a_config.h`.
Se corrigió también la adquisición: los 10 ms de asentamiento ocurren **antes**
de `pulseIn`, después de seleccionar cada filtro. En A la espera sigue siendo
por estados, sin añadir `delay()` al planificador.

El TCS3200 entrega una frecuencia proporcional a la irradiancia; normalmente más
luz produce pulsos más cortos. La orientación 15 negro/200 blanco aplicada aquí
proviene de la suposición explícita del usuario, no de esa relación física.
Validar las referencias antes de usar RGB/HSL como medición cuantitativa del cultivo.
[Ficha técnica del fabricante](https://look.ams-osram.com/m/664723bdb31f55db/original/TCS3200-DS000107.pdf).

Se actualizan el integrado y el nodo A. B y C no convierten los pulsos de color;
no requieren un cambio de firmware por esta curva. Los registros históricos y
los ejemplos anteriores se conservan como capturas de sus versiones originales.

Prueba de cuerpos de código con adquisición simulada:

```powershell
node tests/color-calibration.mjs
```

Las pruebas C++ de A también incorporan los extremos nuevos y la captura.

Validación de esta corrección: pasan el test de color y los cinco scripts Node
preexistentes del integrado. Compilan con Arduino-ESP32 3.3.12 el nodo A
(964164 B de programa, 58160 B de RAM estática) y el integrado
(1277892 B, 58276 B de RAM estática). No se ejecutó la suite C++ de PC ni se cargó
ninguna placa. Los binarios de comprobación de A usan los secretos de ejemplo;
para desplegar el split se deben compilar con los secretos reales del sistema.
