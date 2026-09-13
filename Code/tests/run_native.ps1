param([string]$Zig = "$PSScriptRoot/../.venv/Lib/site-packages/ziglang/zig.exe")
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '../..')
try {
    New-Item -ItemType Directory -Force Code/build-tests | Out-Null
    & $Zig cc -target x86_64-windows-gnu -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror -DENABLE_WIFI_STREAM=1 -I Code/tests/stubs -I spi_dma Code/tests/test_firmware.c spi_dma/ads1299_control.c spi_dma/ads1299_format.c -o Code/build-tests/test_firmware.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native firmware test compilation failed' }
    & Code/build-tests/test_firmware.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native firmware tests failed' }
    & $Zig cc -target x86_64-windows-gnu -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror -I spi_dma spi_dma/usb_commands.c spi_dma/ads1299_control.c spi_dma/ads1299_format.c spi_dma/net/eeg_stream.c spi_dma/tests/test_usb_controls.c -o Code/build-tests/test_usb.exe
    if ($LASTEXITCODE -ne 0) { throw 'USB regression compilation failed' }
    & Code/build-tests/test_usb.exe
    if ($LASTEXITCODE -ne 0) { throw 'USB regression failed' }
} finally { Pop-Location }
