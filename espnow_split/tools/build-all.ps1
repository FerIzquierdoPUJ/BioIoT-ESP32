# Compila los tres firmwares (y opcionalmente el original v4) con arduino-cli.
#   powershell -ExecutionPolicy Bypass -File tools\build-all.ps1 [-Profiles] [-Original]
# -Profiles: usa sketch.yaml (nucleos y bibliotecas fijados; descarga lo necesario).
# Sin -Profiles: usa los nucleos/bibliotecas instalados (rapido) con las mismas FQBN.
# Requiere el nucleo ESP8266 3.1.2 instalado para el nodo B (ver README).
param(
  [switch]$Profiles,
  [switch]$Original,
  [string]$Cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
  [string]$Config = "",
  [string]$Config8266 = "",
  [string]$BuildRoot = "$env:TEMP\bioiot_build"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$lib = Join-Path $root "libraries\BioIoTCommon"
$cfg = @(); if ($Config) { $cfg = @("--config-file", $Config) }
$cfg8266 = $cfg; if ($Config8266) { $cfg8266 = @("--config-file", $Config8266) }
$jobs = @(
  @{ Name = "BioIoT_NodeA_Sensors"; Fqbn = "esp32:esp32:esp32"; Profile = "nodo_a"; Cfg = $cfg },
  @{ Name = "BioIoT_NodeB_I2C"; Fqbn = "esp8266:esp8266:nodemcuv2:eesz=4M1M"; Profile = "nodo_b_nodemcu"; Cfg = $cfg8266 },
  @{ Name = "BioIoT_NodeB_ESP32"; Fqbn = "esp32:esp32:esp32"; Profile = "nodo_b_esp32"; Cfg = $cfg },
  @{ Name = "BioIoT_NodeC_Gateway"; Fqbn = "esp32:esp32:esp32:PartitionScheme=min_spiffs"; Profile = "nodo_c"; Cfg = $cfg }
)
$failed = 0
foreach ($j in $jobs) {
  $sketch = Join-Path $root $j.Name
  $out = Join-Path $BuildRoot $j.Name
  Write-Host "=== $($j.Name) ==="
  if ($Profiles) { & $Cli @($j.Cfg) compile --profile $j.Profile --build-path $out $sketch }
  else { & $Cli @($j.Cfg) compile --fqbn $j.Fqbn --library $lib --build-path $out $sketch }
  if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($Original) {
  $orig = Split-Path -Parent $root
  Write-Host "=== BioIoT_Azure_Integrated (original v4, sin cambios) ==="
  & $Cli @cfg compile --fqbn esp32:esp32:esp32 --build-path (Join-Path $BuildRoot "original") $orig
  if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed) { throw "$failed compilacion(es) fallida(s)" }
Write-Host "Todas las compilaciones terminaron correctamente."
