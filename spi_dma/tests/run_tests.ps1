param(
    [string]$Python = 'C:/ncs/toolchains/2d382dcd92/opt/bin/python.exe',
    [string]$CC = 'C:/MinGW/bin/gcc.exe'
)
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    New-Item -ItemType Directory -Force build | Out-Null
    & $CC -std=c11 -O2 -Wall -Wextra -Werror -I . net/eeg_stream.c tests/test_eeg_stream.c -o build/test_eeg_stream.exe
    if ($LASTEXITCODE -ne 0) { throw 'Stream C test compilation failed' }
    & ./build/test_eeg_stream.exe build/eeg_reference.bin
    if ($LASTEXITCODE -ne 0) { throw 'Stream C tests failed' }
    & $CC -std=c11 -O2 -Wall -Wextra -Werror -I . ads1299_format.c tests/test_ads1299_format.c -o build/test_ads1299_format.exe
    if ($LASTEXITCODE -ne 0) { throw 'ADC format test compilation failed' }
    & ./build/test_ads1299_format.exe
    if ($LASTEXITCODE -ne 0) { throw 'ADC format tests failed' }
    & $Python -m unittest discover -s tests -p 'test_eeg_receiver.py' -v
    if ($LASTEXITCODE -ne 0) { throw 'Python receiver tests failed' }
} finally { Pop-Location }
