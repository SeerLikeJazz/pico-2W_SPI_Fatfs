param(
    [string]$PicoRoot = "$env:USERPROFILE/.pico-sdk",
    [switch]$NoWifi
)
$ErrorActionPreference = 'Stop'
$cmakeExe = Join-Path $PicoRoot 'cmake/v3.31.5/bin/cmake.exe'
$ninjaExe = Join-Path $PicoRoot 'ninja/v1.12.1/ninja.exe'
$env:PICO_SDK_PATH = Join-Path $PicoRoot 'sdk/2.2.0'
$env:PICO_TOOLCHAIN_PATH = Join-Path $PicoRoot 'toolchain/14_2_Rel1'
$buildDir = Join-Path $PSScriptRoot $(if ($NoWifi) { 'build-no-wifi' } else { 'build-firmware' })
$wifi = if ($NoWifi) { 'OFF' } else { 'ON' }
& $cmakeExe -S (Join-Path $PSScriptRoot '../spi_dma') -B $buildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DENABLE_WIFI_STREAM=$wifi"
if ($LASTEXITCODE -ne 0) { throw 'Firmware configuration failed' }
& $cmakeExe --build $buildDir -j 8
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed' }
Write-Output "Firmware: $buildDir/spi_dma.uf2"
