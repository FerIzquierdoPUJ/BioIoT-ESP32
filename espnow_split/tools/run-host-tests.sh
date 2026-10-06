#!/usr/bin/env bash
# Pruebas de PC de la logica portable (sin placas). Ejecutar desde espnow_split/.
#   CXX="python -m ziglang c++" ARDUINOJSON=~/Documents/Arduino/libraries/ArduinoJson/src tools/run-host-tests.sh
set -euo pipefail
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
ARDUINOJSON=${ARDUINOJSON:-"$HOME/OneDrive/Documents/Arduino/libraries/ArduinoJson/src"}
OUT=${OUT:-"${TMPDIR:-/tmp}/bioiot_host_tests"}
C=libraries/BioIoTCommon/src
SRCS=$(ls tests/host/*.cpp $C/*.cpp | grep -v 'bioiot_espnow_')
SRCS="$SRCS BioIoT_NodeA_Sensors/CalibrationLogicA.cpp BioIoT_NodeA_Sensors/SensorMathA.cpp"
SRCS="$SRCS BioIoT_NodeB_I2C/CalibrationLogicB.cpp BioIoT_NodeB_I2C/O2Logic.cpp BioIoT_NodeB_I2C/WarmupPolicy.cpp BioIoT_NodeB_I2C/StorageGuardB.cpp"
SRCS="$SRCS BioIoT_NodeC_Gateway/ActuatorCore.cpp BioIoT_NodeC_Gateway/GatewayState.cpp"
SRCS="$SRCS BioIoT_NodeC_Gateway/CloudCommands.cpp BioIoT_NodeC_Gateway/TelemetryJson.cpp BioIoT_NodeC_Gateway/WifiPolicy.cpp"
$CXX -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -DBIOIOT_HOST_TEST=1 \
  -I$C -Itests/host -I"$ARDUINOJSON" -IBioIoT_NodeA_Sensors -IBioIoT_NodeB_I2C -IBioIoT_NodeC_Gateway \
  $SRCS -o "$OUT"
"$OUT" "$@"
