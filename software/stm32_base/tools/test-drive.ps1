$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Push-Location -LiteralPath $root
try {
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'tmp/zig-global-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'tmp/zig-local-cache'
    $zig = Join-Path $root 'tmp/stm32-test-tools/ziglang/zig.exe'
    & $zig cc -std=c11 -Wall -Wextra -Werror -O1 -Isoftware/stm32_base/include -Isoftware/stm32_arm/vendor/coreJSON software/stm32_base/tests/link_fixture.c software/stm32_base/src/base_link.c software/stm32_base/src/base_boot.c software/stm32_base/src/base_drive.c software/stm32_arm/vendor/coreJSON/core_json.c -o tmp/base-link-fixture.exe
    if ($LASTEXITCODE) { throw 'Base fixture compile failed' }
    & $zig cc -std=c11 -Wall -Wextra -Werror -O1 -Isoftware/stm32_base/include software/stm32_base/tests/drive_test.c software/stm32_base/src/base_drive.c -o tmp/base-drive-test.exe
    if ($LASTEXITCODE) { throw 'Drive test compile failed' }
    & ./tmp/base-drive-test.exe
    if ($LASTEXITCODE) { throw 'Drive assertions failed' }
    $env:BASE_LINK_FIXTURE = Join-Path $root 'tmp/base-link-fixture.exe'
    $env:ARM_LINK_FIXTURE = Join-Path $root 'tmp/base-tests-arm-link-fixture.exe'
    if (!(Test-Path -LiteralPath $env:ARM_LINK_FIXTURE)) { throw 'Existing ARM fixture required; run test-link.ps1 first.' }
    $env:PYTHONPATH = Join-Path $root 'software/stm32_base/tests'
    & ./software/rpi/.venv/Scripts/python.exe -m unittest test_drive_link test_wheel_service test_link.LinkTests -v
    if ($LASTEXITCODE) { throw 'Protocol/gateway tests failed' }
} finally { Pop-Location }

