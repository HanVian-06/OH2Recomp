<#
.SYNOPSIS
    Repeatable Gate M-1: Verifies all Android NDK, CMake, Ninja, Vulkan SDK,
    JDK, Python, Git, validation layer, and Galaxy A56 adb prerequisites.
#>
$ErrorActionPreference = 'Continue'

$CheckEnv = Join-Path $PSScriptRoot "check_env.ps1"
if (-not (Test-Path $CheckEnv)) {
    Write-Host "[FAIL] Gate M-1: missing $CheckEnv" -ForegroundColor Red
    exit 1
}

& $CheckEnv
$EnvExit = $LASTEXITCODE
if ($EnvExit -ne 0) {
    Write-Host "[FAIL] Gate M-1: check_env.ps1 failed (exit=$EnvExit)" -ForegroundColor Red
    exit $EnvExit
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ValSo = Join-Path $RepoRoot "third_party\vulkan_validation\arm64-v8a\libVkLayer_khronos_validation.so"
if (-not (Test-Path $ValSo)) {
    Write-Host "[FAIL] Gate M-1: missing $ValSo" -ForegroundColor Red
    exit 1
}

$ValItem = Get-Item $ValSo
if ($ValItem.Length -le 0) {
    Write-Host "[FAIL] Gate M-1: validation layer is 0 bytes ($ValSo)" -ForegroundColor Red
    exit 1
}

$ElfMagic = New-Object byte[] 4
$Fs = [System.IO.File]::OpenRead($ValSo)
try {
    $BytesRead = $Fs.Read($ElfMagic, 0, 4)
} finally {
    $Fs.Dispose()
}
if ($BytesRead -lt 4 -or $ElfMagic[0] -ne 0x7F -or $ElfMagic[1] -ne 0x45 -or $ElfMagic[2] -ne 0x4C -or $ElfMagic[3] -ne 0x46) {
    Write-Host "[FAIL] Gate M-1: invalid ELF header in $ValSo" -ForegroundColor Red
    exit 1
}

Write-Host "[PASS] Gate M-1: Validation layer present ($($ValItem.Length) bytes, valid ELF)" -ForegroundColor Green
Write-Host "GATE M-1: PASS" -ForegroundColor Green
exit 0
