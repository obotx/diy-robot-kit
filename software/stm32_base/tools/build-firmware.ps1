param([string]$Gcc, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$baseRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $baseRoot 'firmware/obotx_base_f446re'
$armRoot = [IO.Path]::GetFullPath((Join-Path $baseRoot '../stm32_arm'))
$support = Join-Path $armRoot 'firmware/obotx_arm_f446re'
if (!$Gcc) {
    $Gcc = Get-ChildItem -Path 'C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins/*/tools/bin/arm-none-eabi-gcc.exe' |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$Gcc -or !(Test-Path -LiteralPath $Gcc)) { throw 'Pass -Gcc with the ARM compiler path.' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $baseRoot '../../tmp/stm32-base-build' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($out) | Out-Null
$flags = @('-mcpu=cortex-m4','-mthumb','-mfpu=fpv4-sp-d16','-mfloat-abi=hard',
    '-std=c11','-Os','-g3','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
    '-DUSE_HAL_DRIVER','-DSTM32F446xx','--specs=nano.specs')
foreach ($path in @($project, (Join-Path $baseRoot 'include'), (Join-Path $armRoot 'vendor/coreJSON'),
    (Join-Path $support 'Drivers/STM32F4xx_HAL_Driver/Inc'),
    (Join-Path $support 'Drivers/CMSIS/Device/ST/STM32F4xx/Include'),
    (Join-Path $support 'Drivers/CMSIS/Include'))) { $flags += '-I' + $path }
# Explicit sources: only common vendor/startup support is read from the arm tree.
# No arm application, service console, motion or timer implementation is linked.
$sources = @((Join-Path $project 'main.c'), (Join-Path $project 'board_identity.c'),
    (Join-Path $project 'syscalls.c'), (Join-Path $project 'base_board.c'),
    (Join-Path $baseRoot 'src/base_drive.c'),
    (Join-Path $baseRoot 'src/base_link.c'), (Join-Path $baseRoot 'src/base_boot.c'),
    (Join-Path $armRoot 'vendor/coreJSON/core_json.c'),
    (Join-Path $support 'Core/Src/system_stm32f4xx.c'),
    (Join-Path $support 'Core/Src/sysmem.c'),
    (Join-Path $support 'Core/Startup/startup_stm32f446retx.s'))
foreach ($module in @('', '_cortex', '_dma', '_dma_ex', '_flash', '_flash_ex',
                      '_gpio', '_pwr', '_pwr_ex', '_rcc', '_rcc_ex', '_uart')) {
    $sources += Join-Path $support ('Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal' + $module + '.c')
}
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
    $sourceFlags = @()
    if ($source.Contains('Drivers')) { $sourceFlags += '-Wno-unused-parameter' }
    & $Gcc @flags @sourceFlags -c $source -o $object
    if ($LASTEXITCODE) { throw "Compilation failed: $source" }
    $objects += $object
}
$elf = Join-Path $out 'obotx_base_f446re.elf'
$linkFlags = @('-T', (Join-Path $project 'STM32F446RETX_FLASH.ld'), '--specs=nosys.specs',
    '-Wl,--gc-sections', ('-Wl,-Map=' + (Join-Path $out 'obotx_base_f446re.map')),
    '-Wl,--start-group', '-lc', '-lm', '-Wl,--end-group', '-o', $elf)
& $Gcc @flags @objects @linkFlags
if ($LASTEXITCODE) { throw 'Link failed.' }
$bin = Split-Path -Parent $Gcc
& (Join-Path $bin 'arm-none-eabi-size.exe') $elf
if ($LASTEXITCODE) { throw 'Size inspection failed.' }
foreach ($format in @('binary', 'ihex')) {
    $extension = if ($format -eq 'binary') { 'bin' } else { 'hex' }
    & (Join-Path $bin 'arm-none-eabi-objcopy.exe') -O $format $elf (Join-Path $out "obotx_base_f446re.$extension")
    if ($LASTEXITCODE) { throw 'Firmware export failed.' }
}
Write-Output "Built $elf (no device was flashed)"
