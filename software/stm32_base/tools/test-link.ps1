param([string]$Zig, [string]$Python)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Zig) { $Zig = Join-Path $root 'tmp/stm32-test-tools/ziglang/zig.exe' }
if (!$Python) { $Python = Join-Path $root 'software/rpi/.venv/Scripts/python.exe' }
if (!(Test-Path -LiteralPath $Zig)) { throw 'Pass -Zig with a local Zig compiler path.' }
if (!(Test-Path -LiteralPath $Python)) { throw 'Pass -Python with an environment containing RPi requirements.' }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'tmp/zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'tmp/zig-local-cache'
$env:BASE_LINK_FIXTURE = Join-Path $root 'tmp/base-link-fixture.exe'
$env:ARM_LINK_FIXTURE = Join-Path $root 'tmp/base-tests-arm-link-fixture.exe'
$base = Join-Path $root 'software/stm32_base'
$arm = Join-Path $root 'software/stm32_arm'
$sources = @('tests/link_fixture.c', 'src/base_link.c', 'src/base_boot.c', 'src/base_drive.c') |
    ForEach-Object { Join-Path $base $_ }
$sources += Join-Path $arm 'vendor/coreJSON/core_json.c'
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 ('-I' + (Join-Path $base 'include')) ('-I' + (Join-Path $arm 'vendor/coreJSON')) @sources -o $env:BASE_LINK_FIXTURE
if ($LASTEXITCODE) { throw 'Base native compilation failed.' }
$sources = @('tests/link_fixture.c', 'src/arm_link.c', 'src/arm_motion.c', 'vendor/coreJSON/core_json.c') |
    ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 ('-I' + (Join-Path $arm 'include')) @sources -o $env:ARM_LINK_FIXTURE
if ($LASTEXITCODE) { throw 'Arm native compilation failed.' }
& $Python -m unittest discover -s (Join-Path $base 'tests') -v
if ($LASTEXITCODE) { throw 'Base/gateway contract tests failed.' }
