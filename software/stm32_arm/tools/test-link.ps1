param([string]$Zig, [string]$Python)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Zig) { $Zig = Join-Path $root 'tmp/stm32-test-tools/ziglang/zig.exe' }
if (!$Python) { $Python = Join-Path $root 'software/rpi/.venv/Scripts/python.exe' }
if (!(Test-Path -LiteralPath $Zig)) { throw 'Pass -Zig with a local Zig compiler path.' }
if (!(Test-Path -LiteralPath $Python)) { throw 'Pass -Python with a Python environment containing RPi requirements.' }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'tmp/zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'tmp/zig-local-cache'
$env:ARM_LINK_FIXTURE = Join-Path $root 'tmp/arm-link-fixture.exe'
$arm = Join-Path $root 'software/stm32_arm'
$sources = @('tests/link_fixture.c','firmware/obotx_arm_f446re/Core/Src/arm_link.c',
    'firmware/obotx_arm_f446re/Core/Src/arm_motion.c', 'firmware/obotx_arm_f446re/Core/Src/core_json.c') |
    ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $env:ARM_LINK_FIXTURE
if ($LASTEXITCODE) { throw 'Native C compilation failed.' }
$env:ARM_ACTION_ZL_FIXTURE = Join-Path $root 'tmp/arm-link-zl-fixture.exe'
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 -DARM_JSON_ZL_ONLY=1 ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $env:ARM_ACTION_ZL_FIXTURE
if ($LASTEXITCODE) { throw 'Native JSON ZL compilation failed.' }
& $Python -m unittest discover -s (Join-Path $arm 'tests') -v
if ($LASTEXITCODE) { throw 'C/RPi contract tests failed.' }
