param(
    [string]$Gcc,
    [string]$OutputDirectory,
    [switch]$ServiceMode,
    [switch]$BenchOutputs,
    [switch]$StandaloneZL,
    [switch]$DmaScope,
    [switch]$JsonZl
)
$ErrorActionPreference = 'Stop'
if ($JsonZl -and ($ServiceMode -or $BenchOutputs -or $StandaloneZL -or $DmaScope -or !$OutputDirectory)) {
    throw '-JsonZl requires a separate -OutputDirectory and no service/bench flags.'
}
if ($DmaScope -and !$OutputDirectory) { throw 'Specify a separate -OutputDirectory for DMA scope firmware.' }
$armRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $armRoot 'firmware/obotx_arm_f446re'
if (!$Gcc) {
    $Gcc = Get-ChildItem -Path 'C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins/*/tools/bin/arm-none-eabi-gcc.exe' |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$Gcc -or !(Test-Path -LiteralPath $Gcc)) { throw 'Pass -Gcc with the ARM compiler path.' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $armRoot '../../tmp/stm32-link-build' }
$out = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($out) | Out-Null
$flags = @('-mcpu=cortex-m4','-mthumb','-mfpu=fpv4-sp-d16','-mfloat-abi=hard',
    '-std=gnu11','-Og','-g3','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
    '-DUSE_HAL_DRIVER','-DSTM32F446xx','--specs=nano.specs')
if ($ServiceMode) { $flags += '-DARM_SERIAL_SERVICE_MODE=1' }
if ($JsonZl) { $flags += '-DARM_JSON_ZL_ONLY=1' }
if ($DmaScope) {
    if (!$ServiceMode -or $BenchOutputs -or $StandaloneZL) { throw '-DmaScope requires -ServiceMode without -BenchOutputs/-StandaloneZL.' }
    $flags += '-DARM_STEP_SCOPE_DMA=1'
}
if ($BenchOutputs) {
    if (!$ServiceMode) { throw '-BenchOutputs requires -ServiceMode (oscilloscope bench only).' }
    $flags += '-DARM_BENCH_OUTPUTS=1'
}
foreach ($path in @('Core/Inc','Drivers/STM32F4xx_HAL_Driver/Inc',
                   'Drivers/CMSIS/Device/ST/STM32F4xx/Include','Drivers/CMSIS/Include')) {
    $flags += '-I' + (Join-Path $project $path)
}
if ($StandaloneZL) {
    if (!$ServiceMode -or !$BenchOutputs) { throw '-StandaloneZL requires -ServiceMode -BenchOutputs.' }
    $flags += '-DARM_STANDALONE_ZL=1'
}
$sources = @(Get-ChildItem -LiteralPath (Join-Path $project 'Core/Src') -Filter '*.c')
$sources += @(Get-ChildItem -LiteralPath (Join-Path $project 'Drivers/STM32F4xx_HAL_Driver/Src') -Filter '*.c')
$sources += Get-Item -LiteralPath (Join-Path $project 'Core/Startup/startup_stm32f446retx.s')
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $out ($source.BaseName + '.o')
    $sourceFlags = @()
    if ($source.FullName.Contains('Drivers')) { $sourceFlags += '-Wno-unused-parameter' }
    & $Gcc @flags @sourceFlags -c $source.FullName -o $object
    if ($LASTEXITCODE) { throw "Compilation failed: $($source.Name)" }
    $objects += $object
}
$elf = Join-Path $out 'obotx_arm_f446re.elf'
$linkFlags = @('-T', (Join-Path $project 'STM32F446RETX_FLASH.ld'), '--specs=nosys.specs',
    '-Wl,--gc-sections', ('-Wl,-Map=' + (Join-Path $out 'obotx_arm_f446re.map')),
    '-Wl,--start-group', '-lc', '-lm', '-Wl,--end-group', '-o', $elf)
& $Gcc @flags @objects @linkFlags
if ($LASTEXITCODE) { throw 'Link failed.' }
$bin = Split-Path -Parent $Gcc
& (Join-Path $bin 'arm-none-eabi-size.exe') $elf
& (Join-Path $bin 'arm-none-eabi-objcopy.exe') '-O' 'binary' $elf (Join-Path $out 'obotx_arm_f446re.bin')
if ($LASTEXITCODE) { throw 'Binary export failed.' }
Write-Output "Built $elf (no device was flashed)"
