param([string]$Python="$PSScriptRoot/../build/gui-venv/Scripts/python.exe", [string]$CC='C:/MinGW/bin/gcc.exe')
$ErrorActionPreference='Stop'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    New-Item -ItemType Directory -Force build | Out-Null
    & $CC -std=c11 -O2 -Wall -Wextra -Werror -I . usb_link.c tests/test_usb_link.c -o build/test_usb_link.exe
    if($LASTEXITCODE -ne 0){throw 'USB link compilation failed'}
    & ./build/test_usb_link.exe build/usb_reference.bin
    if($LASTEXITCODE -ne 0){throw 'USB link test failed'}
    & $CC -std=c11 -O2 -Wall -Wextra -Werror -I . usb_link.c ads1299_format.c tests/test_usb_app.c -o build/test_usb_app.exe
    if($LASTEXITCODE -ne 0){throw 'USB app test compilation failed'}
    & ./build/test_usb_app.exe
    if($LASTEXITCODE -ne 0){throw 'USB app test failed'}
    foreach($profile in @(0,1)) {
        & $CC -std=c11 -O2 -Wall -Wextra -Werror "-DENABLE_ACQ_PROFILE=$profile" -I . tests/test_adc_driver.c ads1299_format.c -o build/test_adc_driver.exe
        if($LASTEXITCODE -ne 0){throw 'ADC driver test compilation failed'}
        & ./build/test_adc_driver.exe
        if($LASTEXITCODE -ne 0){throw 'ADC driver test failed'}
    }
    & $Python -m unittest discover -s tests -p test_usb_host.py -v
    if($LASTEXITCODE -ne 0){throw 'USB host tests failed'}
    & $Python -m unittest discover -s tests -p test_usb_gui.py -v
    if($LASTEXITCODE -ne 0){throw 'USB GUI tests failed'}
    & $Python -m unittest discover -s tests -p test_usb_process.py -v
    if($LASTEXITCODE -ne 0){throw 'USB process tests failed'}
    & $Python -m unittest discover -s tests -p test_usb_features.py -v
    if($LASTEXITCODE -ne 0){throw 'USB filter/trigger/BDF tests failed'}
} finally { Pop-Location }
