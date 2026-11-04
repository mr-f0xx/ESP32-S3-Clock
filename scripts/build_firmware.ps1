$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $projectRoot "arduino\08_LVGL_Clock"
$libraries = Join-Path $projectRoot "arduino\libraries"
$build = Join-Path $projectRoot "build"
$cli = Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$config = Join-Path $env:USERPROFILE ".arduinoIDE\arduino-cli.yaml"

if (-not (Test-Path -LiteralPath $cli)) {
    throw "Arduino CLI not found. Install Arduino IDE 2.x first."
}

# clock_weather_config.h is git-ignored because it normally holds a private
# QWeather key. Fall back to the public template when it is missing.
$weatherConfig = Join-Path $sketch "clock_weather_config.h"
if (-not (Test-Path -LiteralPath $weatherConfig)) {
    Copy-Item -LiteralPath (Join-Path $sketch "clock_weather_config.example.h") -Destination $weatherConfig
    Write-Host "Created clock_weather_config.h from clock_weather_config.example.h"
}

& $cli compile `
    --jobs 2 `
    -b "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CDCOnBoot=cdc" `
    --libraries $libraries `
    --build-path $build `
    --config-file $config `
    $sketch

if ($LASTEXITCODE -ne 0) {
    throw "Arduino compilation failed with exit code $LASTEXITCODE"
}

$merged = Get-ChildItem -LiteralPath $build -Filter "*.merged.bin" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $merged) {
    throw "Merged firmware not found in $build"
}

$hash = (Get-FileHash -LiteralPath $merged.FullName -Algorithm SHA256).Hash
Write-Host "Build complete"
Write-Host "  merged firmware: $($merged.FullName)"
Write-Host "  size:            $($merged.Length) bytes"
Write-Host "  SHA256:          $hash"
