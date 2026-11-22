param(
    [string]$Serial = "RRCY40086KE",
    [switch]$SkipBuild,
    [string]$BuildType = "RelWithDebInfo"
)

$ErrorActionPreference = "Continue"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $RepoRoot "scripts\env.ps1")

$Adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
$BuildDir = Join-Path $RepoRoot "build\m1"
$CmakeBuildDir = Join-Path $BuildDir "cmake"
$LoaderBin = Join-Path $BuildDir "oh2_m1_loader"
$LoaderSo = Join-Path $BuildDir "liboh2_loader.so"
$LogcatPath = Join-Path $BuildDir "m1_logcat.txt"
$StdoutPath = Join-Path $BuildDir "m1_stdout.txt"
$PayloadOh2 = Join-Path $RepoRoot "Payload\OH2.app\OH2"

function Fail-Gate([string]$Reason) {
    Write-Host "[FAIL] $Reason" -ForegroundColor Red
    Write-Host "GATE M1: FAIL" -ForegroundColor Red
    exit 1
}

Write-Host "=== Milestone M1 Verification Gate (M1_LOADER) ==="

if (-not (Test-Path $PayloadOh2)) {
    Fail-Gate "Read-only Mach-O payload not found at $PayloadOh2"
}
$ExpectedOh2Size = (Get-Item $PayloadOh2).Length
if ($ExpectedOh2Size -ne 53729072) {
    Fail-Gate "Unexpected Payload/OH2.app/OH2 size: $ExpectedOh2Size (expected 53729072)"
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $CmakeBuildDir | Out-Null

# Step 1: Configure & Build oh2_m1_loader and liboh2_loader.so via CMake + Ninja + NDK r28c
if (-not $SkipBuild) {
    $CmakeExe = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\cmake.exe"
    $NinjaExe = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\ninja.exe"
    $Toolchain = Join-Path $env:ANDROID_NDK_HOME "build\cmake\android.toolchain.cmake"
    $M1SrcDir = Join-Path $RepoRoot "android\m1_headless"

    Write-Host "[M1 Build] Configuring CMake (arm64-v8a, android-29, c++_static)..."
    & $CmakeExe `
        -S $M1SrcDir `
        -B $CmakeBuildDir `
        -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
        -DANDROID_ABI=arm64-v8a `
        -DANDROID_PLATFORM=android-29 `
        -DANDROID_STL=c++_static `
        "-DCMAKE_BUILD_TYPE=$BuildType"
    if ($LASTEXITCODE -ne 0) {
        Fail-Gate "CMake configure failed"
    }

    Write-Host "[M1 Build] Building oh2_m1_loader and liboh2_loader.so..."
    & $CmakeExe --build $CmakeBuildDir --config $BuildType
    if ($LASTEXITCODE -ne 0) {
        Fail-Gate "CMake build failed"
    }

    $BuiltBin = Join-Path $CmakeBuildDir "oh2_m1_loader"
    $BuiltSo = Join-Path $CmakeBuildDir "loader_build\liboh2_loader.so"
    if (-not (Test-Path $BuiltBin)) { Fail-Gate "Missing built binary $BuiltBin" }
    if (-not (Test-Path $BuiltSo)) { Fail-Gate "Missing built library $BuiltSo" }

    Copy-Item -Force $BuiltBin $LoaderBin
    Copy-Item -Force $BuiltSo $LoaderSo
}

if (-not (Test-Path $LoaderBin)) { Fail-Gate "Loader binary not found at $LoaderBin" }
if (-not (Test-Path $LoaderSo)) { Fail-Gate "Loader shared library not found at $LoaderSo" }
Write-Host "[PASS] Built M1 artifacts: $LoaderBin , $LoaderSo"

# Step 2: Check Device Connectivity
& $Adb start-server 2>$null | Out-Null
$DevState = (& $Adb -s $Serial get-state 2>$null | Out-String).Trim()
if ($DevState -ne "device") {
    Fail-Gate "Device $Serial is not online (state='$DevState')"
}
$Model = (& $Adb -s $Serial shell getprop ro.product.model 2>$null | Out-String).Trim()
$SdkVer = (& $Adb -s $Serial shell getprop ro.build.version.sdk 2>$null | Out-String).Trim()
Write-Host "[PASS] Target device online: serial=$Serial model=$Model sdk=$SdkVer"

# Step 3: Stage Payload/OH2.app/OH2 (if not already present) and push oh2_m1_loader
$RemoteSize = (& $Adb -s $Serial shell "stat -c %s /data/local/tmp/OH2 2>/dev/null" 2>$null | Out-String).Trim()
if ($RemoteSize -ne "$ExpectedOh2Size") {
    Write-Host "[M1 Deploy] Staging Payload/OH2.app/OH2 (53.7 MB) to /data/local/tmp/OH2..."
    & $Adb -s $Serial push $PayloadOh2 "/data/local/tmp/OH2" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Fail-Gate "Failed pushing Payload/OH2.app/OH2 to /data/local/tmp/OH2"
    }
} else {
    Write-Host "[PASS] /data/local/tmp/OH2 already staged ($RemoteSize bytes)"
}

& $Adb -s $Serial shell "mkdir -p /data/local/tmp/OH2.app" 2>$null | Out-Null
& $Adb -s $Serial push $LoaderBin "/data/local/tmp/oh2_m1_loader" | Out-Null
if ($LASTEXITCODE -ne 0) { Fail-Gate "Failed pushing oh2_m1_loader" }
& $Adb -s $Serial push $LoaderSo "/data/local/tmp/liboh2_loader.so" | Out-Null
if ($LASTEXITCODE -ne 0) { Fail-Gate "Failed pushing liboh2_loader.so" }
& $Adb -s $Serial shell "chmod 755 /data/local/tmp/oh2_m1_loader /data/local/tmp/OH2" 2>$null | Out-Null
Write-Host "[PASS] Deployed /data/local/tmp/oh2_m1_loader and /data/local/tmp/liboh2_loader.so"

# Step 4: Capture Device Timestamp (NEVER run adb logcat -c)
$PreLaunchTs = (& $Adb -s $Serial shell "date '+%Y-%m-%d %H:%M:%S.000'" 2>$null | Out-String).Trim()
if ($PreLaunchTs -notmatch "^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}$") {
    Fail-Gate "Invalid device timestamp format: '$PreLaunchTs'"
}
Write-Host "[PASS] Captured pre-launch device timestamp: $PreLaunchTs"

# Step 5: Execute Headless Loader on Device
Write-Host "[M1 Run] Executing /data/local/tmp/oh2_m1_loader /data/local/tmp/OH2..."
$RunOutput = (& $Adb -s $Serial shell "/data/local/tmp/oh2_m1_loader /data/local/tmp/OH2; echo EXIT_CODE=`$?" 2>&1 | Out-String)
Set-Content -Path $StdoutPath -Value $RunOutput -Encoding UTF8

# Step 6: Collect Logcat Output Since $PreLaunchTs
$Logs = (& $Adb -s $Serial logcat -d -T $PreLaunchTs -s OH2_M1:V OH2_UE4:V libc:F DEBUG:F 2>$null | Out-String)
Set-Content -Path $LogcatPath -Value $Logs -Encoding UTF8

if ($RunOutput -notmatch "EXIT_CODE=0") {
    Write-Host $RunOutput
    Fail-Gate "oh2_m1_loader exited with non-zero status (see $StdoutPath and $LogcatPath)"
}
Write-Host "[PASS] oh2_m1_loader exited cleanly with code 0"

# Step 7: Assert All Milestone M1 Gate Criteria in Logcat
$RequiredPatterns = @(
    "\[M1_PASS\] 1_FAIRPLAY_AND_MMAP: cryptid=0 base=0x100000000 span=0x353c000 tpidrro_el0_reads=0",
    "\[M1_PASS\] 2_DYLD_FIXUPS: rebase=520119 bind=4040 addend16=219 lazy_bind=539 weak_bind=672 missing_symbols=0",
    "\[M1_PASS\] 3_UNWIND_AND_OBJC: eh_frame_fdes=19 synth_fdes=3951 lsdas=838 objc_classes=60 objc_cats=2 selrefs=1491 cfstrings=245",
    "\[M1_PASS\] 4_MOD_INIT_FUNCS: executed=466/466",
    "========= This app is in (LANDSCAPE|PORTRAIT) mode",
    "\[UE4\] LogInit: Oceanhorn 2 UE4\.23 Mach-O M1 Runtime Active on Android ARM64",
    "\[M1_PASS\] 5_LC_MAIN_UE4_STARTUP: entryoff=0x58e3bc return_code=0",
    "\[M1_STATE\] M1_LOADER_VERIFIED_OK"
)

foreach ($Pat in $RequiredPatterns) {
    if ($Logs -notmatch $Pat) {
        Fail-Gate "Missing required M1 verification pattern in logcat: '$Pat' (see $LogcatPath)"
    }
}

if ($Logs -match "Fatal signal|SIGSEGV|SIGABRT|SIGBUS|SIGILL|\[M1_FAIL\]") {
    Fail-Gate "Crash or failure marker detected in logcat (see $LogcatPath)"
}

Write-Host "[PASS] Verified all M1 loader metrics and first UE4 engine log lines in logcat ($LogcatPath)"
Write-Host "GATE M1: PASS" -ForegroundColor Green
exit 0
