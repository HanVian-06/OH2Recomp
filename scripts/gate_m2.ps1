param(
    [string]$Serial = "RRCY40086KE",
    [switch]$SkipBuild,
    [switch]$BuildOnly,
    [string]$BuildType = "RelWithDebInfo",
    [string]$Test = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs
)

$ErrorActionPreference = "Continue"

for ($i = 0; $i -lt $RestArgs.Count; $i++) {
    if ($RestArgs[$i] -eq "--test" -and ($i + 1) -lt $RestArgs.Count) {
        $Test = $RestArgs[$i + 1]
        $i++
    } elseif ($RestArgs[$i] -match "^--test=(.+)$") {
        $Test = $Matches[1]
    }
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $RepoRoot "scripts\env.ps1")

$Adb           = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
$BuildDir      = Join-Path $RepoRoot "build\m2"
$CmakeBuildDir = Join-Path $BuildDir "cmake"
$RunnerBin     = Join-Path $BuildDir "oh2_m2_runner"
$RuntimeSo     = Join-Path $BuildDir "liboh2_runtime.so"
$AppSo         = Join-Path $BuildDir "libm2_runtime_app.so"
$FinalApk      = Join-Path $BuildDir "m2_runtime-debug.apk"
$LogcatPath    = Join-Path $BuildDir "m2_logcat.txt"
$StdoutPath    = Join-Path $BuildDir "m2_stdout.txt"

$PayloadOh2    = Join-Path $RepoRoot "Payload\OH2.app\OH2"
$PayloadPak    = Join-Path $RepoRoot "Payload\OH2.app\cookeddata\oh2\content\paks\oh2-ios.pak"
$PayloadCmd    = Join-Path $RepoRoot "Payload\OH2.app\ue4commandline.txt"
$PayloadPlist  = Join-Path $RepoRoot "Payload\OH2.app\Info.plist"
$PayloadFont   = Join-Path $RepoRoot "Payload\OH2.app\Engine\Content\SlateDebug\Fonts\LastResort.ttf"

$BundleRemote  = "/sdcard/Android/data/com.oceanhorn2.port/files"
$PakRemote     = "$BundleRemote/cookeddata/oh2/content/paks/oh2-ios.pak"

function Fail-Gate([string]$Reason) {
    Write-Host "[FAIL] $Reason" -ForegroundColor Red
    Write-Host "GATE M2: FAIL" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $CmakeBuildDir | Out-Null

# ============================================================================
# Step 1: Build Native Binaries + Package m2_runtime-debug.apk
# ============================================================================
$NeedBuild = (-not $SkipBuild) -and (
    ([string]::IsNullOrEmpty($Test)) -or
    (-not (Test-Path $RunnerBin)) -or
    (-not (Test-Path $RuntimeSo))
)

if ($NeedBuild) {
    $CmakeExe  = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\cmake.exe"
    $NinjaExe  = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\ninja.exe"
    $Toolchain = Join-Path $env:ANDROID_NDK_HOME "build\cmake\android.toolchain.cmake"
    $M2SrcDir  = Join-Path $RepoRoot "android\m2_runtime"

    Write-Host "[M2 Build] Configuring CMake (arm64-v8a, android-29, c++_static)..."
    & $CmakeExe `
        -S $M2SrcDir `
        -B $CmakeBuildDir `
        -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
        -DANDROID_ABI=arm64-v8a `
        -DANDROID_PLATFORM=android-29 `
        -DANDROID_STL=c++_static `
        "-DCMAKE_BUILD_TYPE=$BuildType"
    if ($LASTEXITCODE -ne 0) { Fail-Gate "CMake configure failed" }

    Write-Host "[M2 Build] Building oh2_m2_runner, liboh2_runtime.so, libm2_runtime_app.so..."
    & $CmakeExe --build $CmakeBuildDir --parallel
    if ($LASTEXITCODE -ne 0) { Fail-Gate "CMake build failed" }

    $BuiltRunner  = Join-Path $CmakeBuildDir "oh2_m2_runner"
    $BuiltRunSo   = Join-Path $CmakeBuildDir "runtime_build\liboh2_runtime.so"
    $BuiltAppSo   = Join-Path $CmakeBuildDir "libm2_runtime_app.so"
    if (-not (Test-Path $BuiltRunner)) { Fail-Gate "Missing $BuiltRunner" }
    if (-not (Test-Path $BuiltRunSo))  { Fail-Gate "Missing $BuiltRunSo" }
    if (-not (Test-Path $BuiltAppSo))  { Fail-Gate "Missing $BuiltAppSo" }

    Copy-Item -Force $BuiltRunner $RunnerBin
    Copy-Item -Force $BuiltRunSo  $RuntimeSo
    Copy-Item -Force $BuiltAppSo  $AppSo

    # Package signed APK with ZIP_STORED arm64-v8a libs & 16 KB alignment
    $BuildToolsDir = Join-Path $env:ANDROID_HOME "build-tools\36.0.0"
    $Aapt2     = Join-Path $BuildToolsDir "aapt2.exe"
    $ZipAlign  = Join-Path $BuildToolsDir "zipalign.exe"
    $ApkSigner = Join-Path $BuildToolsDir "apksigner.bat"

    $PlatformJar = Join-Path $env:ANDROID_HOME "platforms\android-36.1\android.jar"
    if (-not (Test-Path $PlatformJar)) {
        $PlatformJar = Join-Path $env:ANDROID_HOME "platforms\android-37.0\android.jar"
    }
    if (-not (Test-Path $PlatformJar)) { Fail-Gate "android.jar not found" }

    $ManifestPath = Join-Path $M2SrcDir "AndroidManifest.xml"
    $UnalignedApk = Join-Path $BuildDir "m2_runtime-unaligned.apk"
    $AlignedApk   = Join-Path $BuildDir "m2_runtime-aligned.apk"
    Remove-Item -Force $UnalignedApk, $AlignedApk, $FinalApk -ErrorAction SilentlyContinue

    & $Aapt2 link -o $UnalignedApk --manifest $ManifestPath -I $PlatformJar --min-sdk-version 29 --target-sdk-version 36
    if ($LASTEXITCODE -ne 0) { Fail-Gate "aapt2 link failed" }

    $PyInject = @"
import sys, zipfile
apk_path, app_so, rt_so = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(apk_path, 'a') as zf:
    zf.write(app_so, 'lib/arm64-v8a/libm2_runtime_app.so', compress_type=zipfile.ZIP_STORED)
    zf.write(rt_so, 'lib/arm64-v8a/liboh2_runtime.so', compress_type=zipfile.ZIP_STORED)
"@
    python -c $PyInject $UnalignedApk $AppSo $RuntimeSo
    if ($LASTEXITCODE -ne 0) { Fail-Gate "APK native library injection failed" }

    & $ZipAlign -f -P 16 4 $UnalignedApk $AlignedApk
    if ($LASTEXITCODE -ne 0) { Fail-Gate "zipalign failed" }

    $Keystore = Join-Path $env:USERPROFILE ".android\debug.keystore"
    if (-not (Test-Path $Keystore)) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Keystore) | Out-Null
        keytool -genkeypair -v -keystore $Keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" | Out-Null
    }
    & $ApkSigner sign --ks $Keystore --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android --out $FinalApk $AlignedApk
    if ($LASTEXITCODE -ne 0) { Fail-Gate "apksigner failed" }

    Remove-Item -Force $UnalignedApk, $AlignedApk -ErrorAction SilentlyContinue
    Write-Host "[PASS] Built M2 artifacts: $RunnerBin , $RuntimeSo , $FinalApk"
}

if ($BuildOnly) {
    Write-Host "M2 BUILD ONLY: PASS" -ForegroundColor Green
    exit 0
}

# ============================================================================
# Step 2: Check ADB Device Connectivity
# ============================================================================
& $Adb start-server 2>$null | Out-Null
$DevState = (& $Adb -s $Serial get-state 2>$null | Out-String).Trim()
if ($DevState -ne "device") {
    Fail-Gate "Device $Serial is not online (state='$DevState')"
}

function Stage-FileIfChanged([string]$LocalPath, [string]$RemotePath, [bool]$CheckMd5 = $false) {
    if (-not (Test-Path $LocalPath)) { return }
    $ExpectedSz = (Get-Item $LocalPath).Length
    $RemoteSz = (& $Adb -s $Serial shell "stat -c %s '$RemotePath' 2>/dev/null" 2>$null | Out-String).Trim()
    $NeedsPush = ($RemoteSz -ne "$ExpectedSz")
    if (-not $NeedsPush -and $CheckMd5) {
        $LocalMd5 = (Get-FileHash -Path $LocalPath -Algorithm MD5).Hash.ToLower()
        $RemoteMd5Raw = (& $Adb -s $Serial shell "md5sum '$RemotePath' 2>/dev/null" 2>$null | Out-String).Trim()
        if ($RemoteMd5Raw -match "^([0-9a-fA-F]{32})") {
            if ($Matches[1].ToLower() -ne $LocalMd5) { $NeedsPush = $true }
        } else {
            $NeedsPush = $true
        }
    }
    if ($NeedsPush) {
        Write-Host "[M2 Deploy] Pushing $(Split-Path $LocalPath -Leaf) ($ExpectedSz bytes) -> $RemotePath..."
        & $Adb -s $Serial push $LocalPath $RemotePath
        if ($LASTEXITCODE -ne 0) { Fail-Gate "Failed pushing $LocalPath to $RemotePath" }
    }
}

# Ensure base target directories and core binaries exist on device
& $Adb -s $Serial shell "mkdir -p '$BundleRemote/cookeddata/oh2/content/paks' '$BundleRemote/Engine/Content/SlateDebug/Fonts' '$BundleRemote/Documents' '$BundleRemote/Library/Caches' '$BundleRemote/tmp'" 2>$null | Out-Null
Stage-FileIfChanged $PayloadOh2 "/data/local/tmp/OH2" $false
Stage-FileIfChanged $PayloadCmd "$BundleRemote/ue4commandline.txt" $false
Stage-FileIfChanged $PayloadPlist "$BundleRemote/Info.plist" $false
Stage-FileIfChanged $PayloadFont "$BundleRemote/Engine/Content/SlateDebug/Fonts/LastResort.ttf" $false

# Sync oh2_m2_runner (MD5 check guarantees freshly built binaries are always deployed even on matching sizes)
Stage-FileIfChanged $RunnerBin "/data/local/tmp/oh2_m2_runner" $true
if ([string]::IsNullOrEmpty($Test)) {
    Stage-FileIfChanged $RuntimeSo "/data/local/tmp/liboh2_runtime.so" $true
}
& $Adb -s $Serial shell "chmod 755 /data/local/tmp/oh2_m2_runner /data/local/tmp/OH2" 2>$null | Out-Null

# ============================================================================
# Step 3: Granular Sub-Test Execution Mode (--test <name>)
# ============================================================================
if (-not [string]::IsNullOrEmpty($Test)) {
    if ($Test -eq "pak_extract_verify") {
        Stage-FileIfChanged $PayloadPak $PakRemote
    }
    $Out = (& $Adb -s $Serial shell "/data/local/tmp/oh2_m2_runner --test $Test /data/local/tmp/OH2; echo __EXIT_CODE__=`$?" 2>&1 | Out-String)
    Write-Host $Out
    if ($Out -match "__EXIT_CODE__=0") {
        exit 0
    }
    exit 1
}

# ============================================================================
# Step 4: Full Milestone M2 Gate Execution on Galaxy A56
# ============================================================================
Write-Host "=== Milestone M2 Verification Gate (M2_RUNTIME_SHIM) ==="
Stage-FileIfChanged $PayloadPak $PakRemote

Write-Host "[M2 Deploy] Installing m2_runtime-debug.apk..."
& $Adb -s $Serial install -r $FinalApk | Out-Null
if ($LASTEXITCODE -ne 0) { Fail-Gate "adb install m2_runtime-debug.apk failed" }

$PreLaunchTs = (& $Adb -s $Serial shell "date '+%Y-%m-%d %H:%M:%S.000'" 2>$null | Out-String).Trim()
if ($PreLaunchTs -notmatch "^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}$") {
    Fail-Gate "Invalid device timestamp: '$PreLaunchTs'"
}
Write-Host "[PASS] Pre-launch timestamp: $PreLaunchTs"

Write-Host "[M2 Run] Executing /data/local/tmp/oh2_m2_runner /data/local/tmp/OH2..."
$RunOutput = (& $Adb -s $Serial shell "/data/local/tmp/oh2_m2_runner /data/local/tmp/OH2; echo EXIT_CODE=`$?" 2>&1 | Out-String)
Set-Content -Path $StdoutPath -Value $RunOutput -Encoding UTF8

$Logs = (& $Adb -s $Serial logcat -d -T $PreLaunchTs -s OH2_M2:V OH2_M1:V OH2_UE4:V OH2_UIBridge:V OH2_NSBridge:V libc:F DEBUG:F 2>$null | Out-String)
Set-Content -Path $LogcatPath -Value $Logs -Encoding UTF8

Write-Host $RunOutput

if ($RunOutput -notmatch "EXIT_CODE=0") {
    Fail-Gate "oh2_m2_runner exited non-zero (see $StdoutPath and $LogcatPath)"
}

$Required = @(
    "\[M2_PASS\] 1_OBJC_MSGSSEND",
    "\[M2_PASS\] 2_LIBC_WRAPPER",
    "\[M2_PASS\] 3_NSBUNDLE",
    "\[M2_PASS\] 4_FRAMEWORK_STUBS",
    "\[M2_PASS\] 5_PAK_OPEN",
    "19920",
    "\.\./\.\./\.\./",
    "\[M2_STATE\] M2_RUNTIME_VERIFIED_OK"
)

$Combined = $RunOutput + "`n" + $Logs
foreach ($Pat in $Required) {
    if ($Combined -notmatch $Pat) {
        Fail-Gate "Missing required M2 verification marker: '$Pat' (see $LogcatPath)"
    }
}

if ($Logs -match "Fatal signal|SIGSEGV|SIGABRT|SIGBUS|SIGILL|\[M2_FAIL\]") {
    Fail-Gate "Crash or failure marker detected in logcat (see $LogcatPath)"
}

Write-Host "GATE M2: PASS" -ForegroundColor Green
exit 0
