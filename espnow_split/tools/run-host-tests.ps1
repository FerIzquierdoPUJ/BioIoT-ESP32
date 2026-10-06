# Compila y ejecuta las pruebas de PC (logica portable, sin placas).
# Requiere un compilador C++17: zig (pip install ziglang==0.13.0) o g++/clang++.
#   powershell -ExecutionPolicy Bypass -File tools\run-host-tests.ps1 [-Cxx "python -m ziglang c++"]
param(
  [string]$Cxx = "",
  [string]$ArduinoJson = "$env:USERPROFILE\OneDrive\Documents\Arduino\libraries\ArduinoJson\src",
  [string]$Filter = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root  # las pruebas comparan con ..\CalibrationModel.h (v4)
$common = Join-Path $root "libraries\BioIoTCommon\src"
$out = Join-Path $env:TEMP "bioiot_host_tests.exe"

$sources = @(
  (Get-ChildItem "$root\tests\host\*.cpp").FullName
  (Get-ChildItem "$common\*.cpp" | Where-Object { $_.Name -notlike "bioiot_espnow_*" }).FullName
  "$root\BioIoT_NodeA_Sensors\CalibrationLogicA.cpp"
  "$root\BioIoT_NodeA_Sensors\SensorMathA.cpp"
  "$root\BioIoT_NodeB_I2C\CalibrationLogicB.cpp"
  "$root\BioIoT_NodeB_I2C\O2Logic.cpp"
  "$root\BioIoT_NodeB_I2C\WarmupPolicy.cpp"
  "$root\BioIoT_NodeB_I2C\StorageGuardB.cpp"
  "$root\BioIoT_NodeC_Gateway\ActuatorCore.cpp"
  "$root\BioIoT_NodeC_Gateway\GatewayState.cpp"
  "$root\BioIoT_NodeC_Gateway\CloudCommands.cpp"
  "$root\BioIoT_NodeC_Gateway\TelemetryJson.cpp"
  "$root\BioIoT_NodeC_Gateway\WifiPolicy.cpp"
) | Where-Object { Test-Path $_ }

$includes = @(
  "-I$common", "-I$root\tests\host", "-I$ArduinoJson",
  "-I$root\BioIoT_NodeA_Sensors", "-I$root\BioIoT_NodeB_I2C", "-I$root\BioIoT_NodeC_Gateway"
)
if (-not $Cxx) {
  if (Get-Command g++ -ErrorAction SilentlyContinue) { $Cxx = "g++" }
  elseif (Get-Command clang++ -ErrorAction SilentlyContinue) { $Cxx = "clang++" }
  else { $Cxx = "python -m ziglang c++" }
}
$cxxParts = $Cxx -split " "
$exe = $cxxParts[0]
$pre = @()
if ($cxxParts.Length -gt 1) { $pre = $cxxParts[1..($cxxParts.Length - 1)] }
try {
  & $exe @pre -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -DBIOIOT_HOST_TEST=1 @includes @sources -o $out
  if ($LASTEXITCODE -ne 0) { throw "Compilacion de pruebas fallida" }
  & $out $Filter
  exit $LASTEXITCODE
} finally {
  Pop-Location
}
