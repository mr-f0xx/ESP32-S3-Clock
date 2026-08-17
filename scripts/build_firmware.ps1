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

Write-Host "Build complete: $build"
