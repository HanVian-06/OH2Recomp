<#
.SYNOPSIS
    Captures verified host toolchain versions, Python Mach-O libs, validation layer ELF info,
    and Galaxy A56 adb device / Vulkan capabilities.
#>
$ErrorActionPreference = 'Continue'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $RepoRoot "scripts\env.ps1")

Write-Output "=== HOST TOOLCHAIN ==="
Write-Output "ANDROID_HOME=$env:ANDROID_HOME"
Write-Output "ANDROID_NDK_HOME=$env:ANDROID_NDK_HOME"
Write-Output "VULKAN_SDK=$env:VULKAN_SDK"
Write-Output "JAVA_HOME=$env:JAVA_HOME"

$Tools = @(
    @{ Name = "clang++"; Cmd = "aarch64-linux-android29-clang++.cmd"; Args = @("--version") },
    @{ Name = "cmake"; Cmd = "cmake"; Args = @("--version") },
    @{ Name = "ninja"; Cmd = "ninja"; Args = @("--version") },
    @{ Name = "spirv-val"; Cmd = "spirv-val"; Args = @("--version") },
    @{ Name = "glslc"; Cmd = "glslc"; Args = @("--version") },
    @{ Name = "spirv-cross"; Cmd = "spirv-cross"; Args = @("--help") },
    @{ Name = "java"; Cmd = "java"; Args = @("-version") },
    @{ Name = "python"; Cmd = "python"; Args = @("--version") },
    @{ Name = "git"; Cmd = "git"; Args = @("--version") }
)

foreach ($t in $Tools) {
    $out = & $t.Cmd @($t.Args) 2>&1 | Select-Object -First 2
    Write-Output "$($t.Name): $($out -join ' | ')"
}

Write-Output "`n=== PYTHON PACKAGES ==="
python -c "import macholib, capstone, lief, elftools; print('macholib=' + macholib.__version__, 'capstone=' + capstone.__version__, 'lief=' + lief.__version__, 'pyelftools=' + elftools.__version__)"

$ValSo = Join-Path $RepoRoot "third_party\vulkan_validation\arm64-v8a\libVkLayer_khronos_validation.so"
Write-Output "`n=== VULKAN VALIDATION LAYER ==="
if (Test-Path $ValSo) {
    $item = Get-Item $ValSo
    $sha = (Get-FileHash $ValSo -Algorithm SHA256).Hash
    Write-Output "Path: $ValSo"
    Write-Output "Size: $($item.Length) bytes | SHA256: $sha"
    llvm-readelf -h $ValSo | Select-String "Class:|Machine:|Type:"
} else {
    Write-Output "MISSING: $ValSo"
}

Write-Output "`n=== TARGET DEVICE (ADB) ==="
adb devices -l
$Props = @(
    "ro.product.model",
    "ro.product.device",
    "ro.board.platform",
    "ro.hardware",
    "ro.soc.manufacturer",
    "ro.soc.model",
    "ro.build.version.release",
    "ro.build.version.sdk",
    "ro.product.cpu.abilist"
)
foreach ($p in $Props) {
    $val = (adb shell getprop $p 2>&1 | Out-String).Trim()
    Write-Output "$p=$val"
}
Write-Output "--- /proc/meminfo ---"
adb shell head -n 3 /proc/meminfo
Write-Output "--- Vulkan features ---"
adb shell pm list features | Select-String "vulkan"
Write-Output "--- cmd gpu vkjson (key fields) ---"
$vk = (adb shell cmd gpu vkjson 2>&1 | Out-String) | ConvertFrom-Json
$dev = $vk.devices[0]
$exts = @($dev.extensions | ForEach-Object { $_.extensionName })
Write-Output ("deviceName: " + $dev.properties.deviceName)
Write-Output ("apiVersion: " + [uint32]$dev.properties.apiVersion)
Write-Output ("vendorID: 0x{0:x4}" -f [uint32]$dev.properties.vendorID)
Write-Output ("deviceID: 0x{0:x8}" -f [uint32]$dev.properties.deviceID)
Write-Output ("textureCompressionASTC_LDR: " + $dev.features.textureCompressionASTC_LDR)
Write-Output ("VK_KHR_dynamic_rendering: " + ($exts -contains "VK_KHR_dynamic_rendering"))
Write-Output ("VK_EXT_descriptor_indexing: " + ($exts -contains "VK_EXT_descriptor_indexing"))
