param(
    [string]$Serial = "RRCY40086KE",
    [switch]$SkipBuild,
    [int]$TimeoutSec = 30,
    [switch]$AllowDynamicRenderingSkip
)

$ErrorActionPreference = "Continue"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $RepoRoot "scripts\env.ps1")

$Adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
$BuildDir = Join-Path $RepoRoot "build\m3"
$ApkPath = Join-Path $BuildDir "m3_vk_sandbox-debug.apk"
$ScreencapPath = Join-Path $BuildDir "m3_screencap.png"
$LogcatPath = Join-Path $BuildDir "m3_logcat.txt"
$PackageName = "com.oceanhorn2.port.m3vulkan"
$ActivityName = "$PackageName/android.app.NativeActivity"

function Fail-Gate([string]$Reason) {
    Write-Host "[FAIL] $Reason" -ForegroundColor Red
    Write-Host "GATE M3: FAIL" -ForegroundColor Red
    exit 1
}

Write-Host "=== Milestone M3 Verification Gate (M3_GRAPHICS_FOUNDATION) ==="

# Step 0: Env & Device Probe
$ValidationSo = Join-Path $RepoRoot "third_party\vulkan_validation\arm64-v8a\libVkLayer_khronos_validation.so"
$VmaHeader = Join-Path $RepoRoot "third_party\vma\vk_mem_alloc.h"
if (-not (Test-Path $ValidationSo)) { Fail-Gate "Missing $ValidationSo" }
if (-not (Test-Path $VmaHeader)) { Fail-Gate "Missing $VmaHeader" }

& $Adb start-server 2>$null | Out-Null
$DevState = (& $Adb -s $Serial get-state 2>$null | Out-String).Trim()
if ($DevState -ne "device") {
    Fail-Gate "Device $Serial is not online (state='$DevState')"
}
$Model = (& $Adb -s $Serial shell getprop ro.product.model 2>$null | Out-String).Trim()
$SdkVer = (& $Adb -s $Serial shell getprop ro.build.version.sdk 2>$null | Out-String).Trim()
Write-Host "[PASS] Target device online: serial=$Serial model=$Model sdk=$SdkVer"

# Step 1: SPIR-V & APK Build
if (-not $SkipBuild) {
    & (Join-Path $RepoRoot "scripts\m3\build_apk.ps1")
    if ($LASTEXITCODE -ne 0) {
        Fail-Gate "scripts/m3/build_apk.ps1 failed"
    }
}
if (-not (Test-Path $ApkPath)) {
    Fail-Gate "APK not found at $ApkPath"
}

$PyCheckZip = @"
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'r') as zf:
    names = set(zf.namelist())
required = {
    'lib/arm64-v8a/libm3_vk_sandbox.so',
    'lib/arm64-v8a/libVkLayer_khronos_validation.so',
    'assets/shaders/triangle.vert.spv',
    'assets/shaders/triangle.frag.spv',
}
missing = required - names
if missing:
    print('Missing APK entries:', missing)
    sys.exit(1)
"@
python -c $PyCheckZip $ApkPath
if ($LASTEXITCODE -ne 0) {
    Fail-Gate "APK missing required native libraries or SPIR-V assets"
}
Write-Host "[PASS] APK built and verified: $ApkPath"

# Step 2: Parallel-Safe Pre-Launch Prep (NEVER run adb logcat -c)
& $Adb -s $Serial shell am force-stop $PackageName 2>$null | Out-Null

$PowerDump = (& $Adb -s $Serial shell dumpsys power 2>$null | Out-String)
if ($PowerDump -notmatch "mWakefulness=Awake") {
    & $Adb -s $Serial shell input keyevent KEYCODE_WAKEUP 2>$null | Out-Null
    Start-Sleep -Milliseconds 400
    & $Adb -s $Serial shell wm dismiss-keyguard 2>$null | Out-Null
}

$PreLaunchTs = (& $Adb -s $Serial shell "date '+%Y-%m-%d %H:%M:%S.000'" 2>$null | Out-String).Trim()
if ($PreLaunchTs -notmatch "^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}$") {
    Fail-Gate "Invalid device timestamp format: '$PreLaunchTs'"
}
Write-Host "[PASS] Captured pre-launch device timestamp: $PreLaunchTs"

# Step 3: Install & Launch
$InstallOut = (& $Adb -s $Serial install -r -t $ApkPath 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $InstallOut -notmatch "Success") {
    Fail-Gate "APK install failed: $InstallOut"
}

$StartOut = (& $Adb -s $Serial shell am start -W -S -n $ActivityName 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $StartOut -match "Error:") {
    Fail-Gate "am start failed: $StartOut"
}
Write-Host "[PASS] Installed and launched $ActivityName"

# Step 4: Wait for Self-Test & Live Rendering
$Deadline = (Get-Date).AddSeconds($TimeoutSec)
$SelftestDone = $false
$FrameSeen = $false
$Logs = ""

while ((Get-Date) -lt $Deadline) {
    $Logs = (& $Adb -s $Serial logcat -d -T $PreLaunchTs -s OH2_M3_VK:V VulkanValidation:V 2>$null | Out-String)
    if ($Logs -match "M3_SELFTEST_COMPLETE") { $SelftestDone = $true }
    if ($Logs -match "\[M3_FRAME\]") { $FrameSeen = $true }
    if ($SelftestDone -and $FrameSeen) { break }
    Start-Sleep -Milliseconds 500
}

Set-Content -Path $LogcatPath -Value $Logs -Encoding UTF8

if (-not $SelftestDone) {
    Fail-Gate "Timed out waiting for M3_SELFTEST_COMPLETE in logcat (see $LogcatPath)"
}
if (-not $FrameSeen) {
    Fail-Gate "Timed out waiting for [M3_FRAME] in logcat (see $LogcatPath)"
}

$AppPid = (& $Adb -s $Serial shell pidof $PackageName 2>$null | Out-String).Trim()
if ([string]::IsNullOrWhiteSpace($AppPid)) {
    Fail-Gate "Process $PackageName is not running after self-test"
}
Write-Host "[PASS] Self-test complete and steady-state rendering active (PID=$AppPid)"

# Allow steady-state frame presentation to settle before screenshot
Start-Sleep -Milliseconds 500

# Step 5: Screencap & PNG Pixel Verification
$VerifyScript = Join-Path $RepoRoot "scripts\m3\verify_screencap.py"
python $VerifyScript --adb $Adb --serial $Serial --out $ScreencapPath --clear-rgb "16,48,80"
if ($LASTEXITCODE -ne 0) {
    Fail-Gate "Screenshot verification failed (see $ScreencapPath)"
}

# Step 6: Logcat Validation & Self-Test Assertions
$Logs = (& $Adb -s $Serial logcat -d -T $PreLaunchTs -s OH2_M3_VK:V VulkanValidation:V 2>$null | Out-String)
Set-Content -Path $LogcatPath -Value $Logs -Encoding UTF8

$RequiredPasses = @(
    "\[M3_PASS\] 1_INSTANCE_VALIDATION: layer=VK_LAYER_KHRONOS_validation debug_utils=1",
    "\[M3_PASS\] 2_RUNTIME_FEATURES: dynamic_rendering=[01] descriptor_indexing=[01] astc_4x4=1 astc_6x6=1 astc_8x8=1",
    "\[M3_PASS\] 3_RENDERPASS_FALLBACK: status=VERIFIED",
    "\[M3_PASS\] 4_DYNAMIC_RENDERING: status=(VERIFIED|SKIPPED_UNSUPPORTED)",
    "\[M3_PASS\] 5_VMA_STAGING_RING: capacity_mb=\d+ wrap_around=PASS fence_sync=PASS",
    "\[M3_PASS\] 6_ASTC_DIRECT_UPLOAD: format=VK_FORMAT_ASTC_4x4_UNORM_BLOCK rgba8_decomp=0 upload_sample=PASS",
    "\[M3_PASS\] 7_YFLIP_VIEWPORT_WINDING: viewport_height_neg=1 front_face=(CW|CCW) cull=BACK status=PASS"
)

foreach ($Pat in $RequiredPasses) {
    if ($Logs -notmatch $Pat) {
        Fail-Gate "Missing required self-test pass pattern: $Pat"
    }
}

if (-not $AllowDynamicRenderingSkip -and $Model -eq "SM-A566B") {
    if ($Logs -notmatch "\[M3_PASS\] 4_DYNAMIC_RENDERING: status=VERIFIED") {
        Fail-Gate "SM-A566B must verify VK_KHR_dynamic_rendering (status=VERIFIED)"
    }
}

if ($Logs -notmatch "\[M3_STATE\] M3_SELFTEST_COMPLETE validation_errors=0 validation_warnings=0") {
    Fail-Gate "M3_SELFTEST_COMPLETE did not report validation_errors=0 validation_warnings=0"
}

if ($Logs -match "VUID-|Validation Error|Validation Warning|UNASSIGNED-|\bE VulkanValidation\b|\bW VulkanValidation\b") {
    Fail-Gate "VulkanValidation errors or warnings detected in logcat (see $LogcatPath)"
}

$CrashLogs = (& $Adb -s $Serial logcat -d -T $PreLaunchTs --pid=$AppPid libc:F DEBUG:F AndroidRuntime:E 2>$null | Out-String)
if ($CrashLogs -match "FATAL EXCEPTION|Fatal signal|SIGSEGV|SIGABRT") {
    Fail-Gate "Native crash detected for PID $AppPid"
}

Write-Host "[PASS] All 7 M3 self-test passes verified with 0 Vulkan validation errors/warnings"
Write-Host "[PASS] Artifacts: $ScreencapPath , $LogcatPath"
Write-Host "GATE M3: PASS" -ForegroundColor Green
exit 0
