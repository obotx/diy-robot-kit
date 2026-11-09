param([string]$Zig)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Zig) { $Zig = Join-Path $root 'tmp/stm32-test-tools/ziglang/zig.exe' }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'tmp/zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $root 'tmp/zig-local-cache'
$arm = Join-Path $root 'software/stm32_arm'
$exe = Join-Path $root 'tmp/arm-motion-tests.exe'
$sources = @('tests/motion_fixture.c','src/arm_motion.c','src/arm_debug_protocol.c','src/arm_step_plan.c') | ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 ('-I' + (Join-Path $arm 'include')) @sources -o $exe
if ($LASTEXITCODE) { throw 'Motion test compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'Motion tests failed.' }
$exe = Join-Path $root 'tmp/arm-step-hw-tests.exe'
$sources = @('tests/step_hw_fixture.c','firmware/obotx_arm_f446re/Core/Src/arm_step_hw.c',
    'src/arm_motion.c','src/arm_step_plan.c') | ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 -DARM_STEP_ACCOUNT_CHECK=1 ('-I' + (Join-Path $arm 'tests/hw_mock')) ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $exe
if ($LASTEXITCODE) { throw 'Timer mock compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'Timer mock tests failed.' }
$exe = Join-Path $root 'tmp/arm-standalone-tests.exe'
$sources = @('tests/standalone_fixture.c','firmware/obotx_arm_f446re/Core/Src/arm_step_hw.c',
    'src/arm_motion.c','src/arm_step_plan.c','src/arm_debug_protocol.c') | ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 -DARM_STANDALONE_ZL=1 -DARM_SERIAL_SERVICE_MODE=1 -DARM_BENCH_OUTPUTS=1 ('-I' + (Join-Path $arm 'tests/hw_mock')) ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $exe
if ($LASTEXITCODE) { throw 'Standalone test compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'Standalone tests failed.' }
$exe = Join-Path $root 'tmp/arm-step-burst-tests.exe'
$sources = @('tests/step_burst_fixture.c','firmware/obotx_arm_f446re/Core/Src/arm_step_burst.c',
    'src/arm_step_plan.c') | ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 ('-I' + (Join-Path $arm 'tests/hw_mock')) ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $exe
if ($LASTEXITCODE) { throw 'DMA burst mock compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'DMA burst mock tests failed.' }
$exe = Join-Path $root 'tmp/arm-step-dma-motion-tests.exe'
$sources = @('tests/step_dma_motion_fixture.c','firmware/obotx_arm_f446re/Core/Src/arm_step_hw.c',
    'src/arm_motion.c','src/arm_step_plan.c') | ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 -DARM_DMA_START_CHECK=1 ('-I' + (Join-Path $arm 'tests/hw_mock')) ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $exe
if ($LASTEXITCODE) { throw 'Motion DMA mock compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'Motion DMA mock tests failed.' }
$exe = Join-Path $root 'tmp/arm-uart-tx-tests.exe'
$sources = @('tests/uart_tx_fixture.c','firmware/obotx_arm_f446re/Core/Src/arm_uart_tx.c') | ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 ('-I' + (Join-Path $arm 'tests/uart_mock')) ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $exe
if ($LASTEXITCODE) { throw 'UART TX test compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'UART TX tests failed.' }
$exe = Join-Path $root 'tmp/gripper-hw-tests.exe'
$sources = @('tests/gripper_hw_fixture.c','firmware/obotx_arm_f446re/Core/Src/arm_gripper.c') | ForEach-Object { Join-Path $arm $_ }
& $Zig cc -std=c11 -Wall -Wextra -Werror -O1 ('-I' + (Join-Path $arm 'tests/hw_mock')) ('-I' + (Join-Path $arm 'firmware/obotx_arm_f446re/Core/Inc')) @sources -o $exe
if ($LASTEXITCODE) { throw 'Gripper mock compilation failed.' }
& $exe
if ($LASTEXITCODE) { throw 'Gripper mock tests failed.' }
