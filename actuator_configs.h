#pragma once

// ESP32-WROOM: RGB analogica de 12 V (anodo comun), un LR7843 por color.
// IMPORTANTE: mover EN del CD74HC4067 de GPIO13 a GND antes de cargar v4.
// No conectar las cargas de 12 V directamente a los GPIO.
#define COMPRESSOR_RELAY_PIN       32
#define COMPRESSOR_RELAY_ACTIVE_LOW true // Cambiar a false si el rele activa con HIGH.
#define LED_R_PIN                  33
#define LED_G_PIN                  4
#define LED_B_PIN                  13
#define LED_PWM_ACTIVE_LOW         false // Cambiar si el modulo invierte su entrada.
#define LED_PWM_FREQUENCY_HZ       1000
#define LED_PWM_RESOLUTION_BITS    8

// Valor inicial conservador; ajustar segun la ficha tecnica del compresor.
// Aplica tambien al primer encendido despues de arrancar. OFF siempre es inmediato.
#define COMPRESSOR_MIN_OFF_MS      180000UL
// Todo comando de encendido debe traer expiresAt (Unix UTC) dentro de este plazo.
#define ACTUATOR_MAX_LEASE_SECONDS 900UL
// Sin enlace con IoT Hub las salidas se mantienen este tiempo (renovacion SAS horaria,
// cortes WiFi breves); despues se apagan. El vencimiento (expiresAt) se aplica igual.
#define ACTUATOR_DISCONNECT_GRACE_MS 60000UL
