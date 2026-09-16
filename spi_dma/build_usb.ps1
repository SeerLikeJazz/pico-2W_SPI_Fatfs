# USB isolation build, never flashes a device.
param([switch]$Profile, [string]$PicoRoot="$env:USERPROFILE/.pico-sdk",
      [string]$Python='C:/ncs/toolchains/2d382dcd92/opt/bin/python.exe')
$ErrorActionPreference='Stop'
$cmake=Join-Path $PicoRoot 'cmake/v3.31.5/bin/cmake.exe'
$ninja=Join-Path $PicoRoot 'ninja/v1.12.1/ninja.exe'
$buildDir=Join-Path $PSScriptRoot $(if($Profile){'build/usb-profile'}else{'build'})
$profileValue=if($Profile){'ON'}else{'OFF'}
& $cmake -S $PSScriptRoot -B $buildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DPython3_EXECUTABLE=$Python" -DCMAKE_BUILD_TYPE=Release -DENABLE_WIFI_STREAM=OFF -DENABLE_SD_CARD=OFF -DENABLE_UART_LOG=OFF "-DENABLE_ACQ_PROFILE=$profileValue"
if($LASTEXITCODE -ne 0){throw 'Configure failed'}
& $cmake --build $buildDir -j 8
if($LASTEXITCODE -ne 0){throw 'Build failed'}
Write-Output "UF2 (not flashed): $buildDir/spi_dma.uf2"
