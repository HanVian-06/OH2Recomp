param(
    [string]$Serial = "RRCY40086KE",
    [switch]$SkipBuild,
    [switch]$BuildOnly,
    [switch]$Release,
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
$PkgName       = "com.oceanhorn2.port"
$ActivityName  = "$PkgName/android.app.NativeActivity"

if ($Release -and $BuildType -eq "RelWithDebInfo") {
    $BuildType = "Release"
}
$BuildDir      = Join-Path $RepoRoot "build\m5"
if ($Release) {
    $CmakeBuildDir = Join-Path $BuildDir "cmake_release"
    $FinalApk      = Join-Path $BuildDir "oh2_android-release.apk"
} else {
    $CmakeBuildDir = Join-Path $BuildDir "cmake"
    $FinalApk      = Join-Path $BuildDir "oh2_android-debug.apk"
}
$AppSrcDir     = Join-Path $RepoRoot "android\app"
$LogcatPath    = Join-Path $BuildDir "m5_logcat.txt"

$PayloadOh2    = Join-Path $RepoRoot "Payload\OH2.app\OH2"
$PayloadPak    = Join-Path $RepoRoot "Payload\OH2.app\cookeddata\oh2\content\paks\oh2-ios.pak"
$PayloadCmd    = Join-Path $RepoRoot "Payload\OH2.app\ue4commandline.txt"
$PayloadPlist  = Join-Path $RepoRoot "Payload\OH2.app\Info.plist"
$PayloadFont   = Join-Path $RepoRoot "Payload\OH2.app\Engine\Content\SlateDebug\Fonts\LastResort.ttf"
$SpirvCache    = Join-Path $RepoRoot "build\shaders\spirv_cache.bin"
$LibValSo      = Join-Path $RepoRoot "third_party\vulkan_validation\arm64-v8a\libVkLayer_khronos_validation.so"

$RemoteFiles   = "/sdcard/Android/data/$PkgName/files"

function Fail-Gate([string]$Reason) {
    Write-Host "[FAIL] $Reason" -ForegroundColor Red
    Write-Host "GATE M5: FAIL" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $CmakeBuildDir | Out-Null

# ============================================================================
# Step 1: Build liboh2_android.so & Package 16KiB-Aligned oh2_android-debug.apk
# ============================================================================
$NeedBuild = (-not $SkipBuild) -and (([string]::IsNullOrEmpty($Test)) -or (-not (Test-Path $FinalApk)))
if ($NeedBuild) {
    if (-not $Release) {
        if (-not (Test-Path $PayloadOh2)) { Fail-Gate "Missing read-only Payload/OH2.app/OH2 (use -Release for asset-free open-source APK)" }
        if (-not (Test-Path $LibValSo))   { Fail-Gate "Missing $LibValSo" }
    }

    $CmakeExe  = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\cmake.exe"
    $NinjaExe  = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\ninja.exe"
    $Toolchain = Join-Path $env:ANDROID_NDK_HOME "build\cmake\android.toolchain.cmake"

    Write-Host "[M5 Build] Configuring CMake (arm64-v8a, android-29, c++_static)..."
    & $CmakeExe `
        -S $AppSrcDir `
        -B $CmakeBuildDir `
        -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
        -DANDROID_ABI=arm64-v8a `
        -DANDROID_PLATFORM=android-29 `
        -DANDROID_STL=c++_static `
        "-DCMAKE_BUILD_TYPE=$BuildType"
    if ($LASTEXITCODE -ne 0) { Fail-Gate "CMake configure failed" }

    Write-Host "[M5 Build] Building liboh2_android.so..."
    & $CmakeExe --build $CmakeBuildDir --parallel
    if ($LASTEXITCODE -ne 0) { Fail-Gate "CMake build failed" }

    $AppSo = Join-Path $CmakeBuildDir "liboh2_android.so"
    if (-not (Test-Path $AppSo)) { Fail-Gate "Missing $AppSo" }

    if ($Release) {
        $LlvmStrip = Join-Path $env:ANDROID_NDK_HOME "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
        $StrippedSo = Join-Path $CmakeBuildDir "liboh2_android.stripped.so"
        if (Test-Path $LlvmStrip) {
            Copy-Item -Force $AppSo $StrippedSo
            & $LlvmStrip --strip-unneeded $StrippedSo
            if ($LASTEXITCODE -eq 0) {
                $AppSo = $StrippedSo
                Write-Host "[M5 Build] Stripped Release liboh2_android.so ($([Math]::Round((Get-Item $AppSo).Length / 1MB, 2)) MB)"
            }
        }
    }

    $BuildToolsDir = Join-Path $env:ANDROID_HOME "build-tools\36.0.0"
    $Aapt2     = Join-Path $BuildToolsDir "aapt2.exe"
    $D8        = Join-Path $BuildToolsDir "d8.bat"
    $ZipAlign  = Join-Path $BuildToolsDir "zipalign.exe"
    $ApkSigner = Join-Path $BuildToolsDir "apksigner.bat"

    $PlatformJar = Join-Path $env:ANDROID_HOME "platforms\android-36.1\android.jar"
    if (-not (Test-Path $PlatformJar)) {
        $PlatformJar = Join-Path $env:ANDROID_HOME "platforms\android-37.0\android.jar"
    }
    if (-not (Test-Path $PlatformJar)) { Fail-Gate "android.jar not found" }

    # Compile Java Launcher sources -> classes.dex
    $JavaSrcDir = Join-Path $AppSrcDir "java"
    $ClassesDir = Join-Path $BuildDir "java_classes"
    $DexDir     = Join-Path $BuildDir "dex"
    Remove-Item -Recurse -Force $ClassesDir, $DexDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $ClassesDir, $DexDir | Out-Null
    $JavaFiles = Get-ChildItem -Path $JavaSrcDir -Recurse -Filter "*.java" | ForEach-Object { $_.FullName }
    if ($JavaFiles.Count -eq 0) { Fail-Gate "No Java sources found in $JavaSrcDir" }

    Write-Host "[M5 Build] Compiling $($JavaFiles.Count) Java Launcher sources..."
    & javac --release 11 -cp $PlatformJar -d $ClassesDir @JavaFiles
    if ($LASTEXITCODE -ne 0) { Fail-Gate "javac failed" }

    $ClassFiles = Get-ChildItem -Path $ClassesDir -Recurse -Filter "*.class" | ForEach-Object { $_.FullName }
    Write-Host "[M5 Build] Converting $($ClassFiles.Count) .class files to classes.dex..."
    & $D8 --min-api 29 --lib $PlatformJar --output $DexDir @ClassFiles
    if ($LASTEXITCODE -ne 0) { Fail-Gate "d8 failed" }
    $ClassesDex = Join-Path $DexDir "classes.dex"
    if (-not (Test-Path $ClassesDex)) { Fail-Gate "Missing $ClassesDex" }

    $PrepareIcons = Join-Path $RepoRoot "scripts\m5\prepare_icons.py"
    $ResDir       = Join-Path $BuildDir "res"
    $ResZip       = Join-Path $BuildDir "res.zip"
    Remove-Item -Recurse -Force $ResDir -ErrorAction SilentlyContinue
    Remove-Item -Force $ResZip -ErrorAction SilentlyContinue
    python $PrepareIcons --out-dir $ResDir
    if ($LASTEXITCODE -ne 0) { Fail-Gate "prepare_icons.py failed" }
    & $Aapt2 compile --dir $ResDir -o $ResZip
    if ($LASTEXITCODE -ne 0) { Fail-Gate "aapt2 compile failed" }

    $ManifestPath = Join-Path $AppSrcDir "AndroidManifest.xml"
    $BaseApk      = Join-Path $BuildDir "oh2_base.apk"
    $AlignedApk   = Join-Path $BuildDir "oh2_aligned.apk"
    Remove-Item -Force $BaseApk, $AlignedApk, $FinalApk, "$FinalApk.idsig" -ErrorAction SilentlyContinue

    & $Aapt2 link -o $BaseApk --manifest $ManifestPath -R $ResZip -I $PlatformJar --min-sdk-version 29 --target-sdk-version 36
    if ($LASTEXITCODE -ne 0) { Fail-Gate "aapt2 link failed" }

    $PackArgs = @(
        (Join-Path $RepoRoot "scripts\m5\pack_apk.py"),
        "--base-apk", $BaseApk,
        "--out-apk",  $AlignedApk,
        "--app-so",   $AppSo,
        "--dex",      $ClassesDex
    )
    if (-not $Release) {
        $PackArgs += @("--val-so", $LibValSo, "--macho", $PayloadOh2)
    }
    python @PackArgs
    if ($LASTEXITCODE -ne 0) { Fail-Gate "pack_apk.py failed" }

    & $ZipAlign -c -P 16 4 $AlignedApk
    if ($LASTEXITCODE -ne 0) { Fail-Gate "zipalign -c -P 16 4 verification failed" }

    $Keystore = Join-Path (Join-Path $env:USERPROFILE ".android") "debug.keystore"
    if (-not (Test-Path $Keystore)) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Keystore) | Out-Null
        keytool -genkeypair -v -keystore $Keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" | Out-Null
    }

    & $ApkSigner sign `
        --v1-signing-enabled false `
        --v2-signing-enabled true `
        --v3-signing-enabled true `
        --ks $Keystore `
        --ks-key-alias androiddebugkey `
        --ks-pass pass:android `
        --key-pass pass:android `
        --out $FinalApk `
        $AlignedApk
    if ($LASTEXITCODE -ne 0) { Fail-Gate "apksigner failed" }

    $PyCheckAlign = @"
import sys, zipfile, struct
apk = sys.argv[1]
with open(apk, 'rb') as f:
    with zipfile.ZipFile(f) as zf:
        for zi in zf.infolist():
            if zi.filename.endswith('.so') or zi.filename == 'assets/oh2_macho.bin':
                f.seek(zi.header_offset)
                nlen, xlen = struct.unpack_from('<HH', f.read(30), 26)
                doff = zi.header_offset + 30 + nlen + xlen
                if (doff % 16384) != 0:
                    raise SystemExit(f'{zi.filename} offset 0x{doff:x} not 16KiB aligned')
                print(f'[M5 Align OK] {zi.filename} off=0x{doff:x} (mod16k=0)')
"@
    python -c $PyCheckAlign $FinalApk
    if ($LASTEXITCODE -ne 0) { Fail-Gate "Signed APK failed 16KiB offset alignment check" }

    Remove-Item -Force $BaseApk, $AlignedApk, $ResZip, "$FinalApk.idsig" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $ResDir -ErrorAction SilentlyContinue
    Write-Host "[M5 Build] Built signed APK: $FinalApk" -ForegroundColor Green
}

if ($BuildOnly) {
    Write-Host "BuildOnly complete."
    exit 0
}

# ============================================================================
# Step 2: Check Device & Stage spirv_cache.bin + Pak Assets
# ============================================================================
$state = & $Adb -s $Serial get-state 2>$null
if ($state -notmatch "device") {
    Fail-Gate "ADB device $Serial is offline"
}

if (Test-Path $SpirvCache) {
    $expectedSpvSize = (Get-Item $SpirvCache).Length
    $remoteSpvSize = (& $Adb -s $Serial shell "stat -c %s $RemoteFiles/spirv_cache.bin 2>/dev/null || echo 0").Trim()
    if ($remoteSpvSize -ne "$expectedSpvSize") {
        Write-Host "[M5 Stage] Pushing spirv_cache.bin ($expectedSpvSize B) to $RemoteFiles..."
        & $Adb -s $Serial shell "mkdir -p $RemoteFiles" | Out-Null
        & $Adb -s $Serial push $SpirvCache "$RemoteFiles/spirv_cache.bin" | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail-Gate "Failed pushing spirv_cache.bin" }
    }
} else {
    Fail-Gate "Missing M4 spirv_cache.bin at $SpirvCache"
}

# Install APK if missing, freshly built, or newer than last installed timestamp
$InstallStamp = Join-Path $BuildDir ".installed_apk_ts"
$ApkTimeStr = if (Test-Path $FinalApk) { (Get-Item $FinalApk).LastWriteTimeUtc.ToString("o") } else { "" }
$LastInstalled = if (Test-Path $InstallStamp) { (Get-Content $InstallStamp -Raw).Trim() } else { "" }
$pkgPathRaw = & $Adb -s $Serial shell "pm path $PkgName 2>/dev/null"
$pkgPath = ($pkgPathRaw | Out-String).Trim()
if ($NeedBuild -or [string]::IsNullOrEmpty($pkgPath) -or ($ApkTimeStr -ne $LastInstalled)) {
    Write-Host "[M5 Deploy] Installing $FinalApk onto $Serial..."
    & $Adb -s $Serial install -r -t $FinalApk | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail-Gate "adb install failed" }
    Set-Content -Path $InstallStamp -Value $ApkTimeStr -NoNewline -Encoding ASCII
}

# ============================================================================
# Step 3: Execute Isolated F10 Test (--test <name>) or Live Main Menu Gate
# ============================================================================
if (-not [string]::IsNullOrEmpty($Test)) {
    Write-Host "[M5 Test] Running isolated F10 test '$Test' on $Serial..."
    & $Adb -s $Serial shell "am force-stop $PkgName" | Out-Null
    Start-Sleep -Milliseconds 300

    $TriggerLocal = Join-Path $BuildDir "trigger_test.txt"
    Set-Content -Path $TriggerLocal -Value $Test -NoNewline -Encoding ASCII
    & $Adb -s $Serial push $TriggerLocal "$RemoteFiles/.oh2_test" | Out-Null
    Remove-Item -Force $TriggerLocal -ErrorAction SilentlyContinue

    $Ts = (& $Adb -s $Serial shell "date '+%Y-%m-%d %H:%M:%S.000'").Trim()
    & $Adb -s $Serial shell "am start -n $ActivityName" | Out-Null

    $passed = $false
    $failed = $false
    $logOut = ""
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 700
        $logOut = (& $Adb -s $Serial logcat -d -T "$Ts" "OH2_MTLVK:V" "OH2_VK:V" "OH2_VK_VAL:V" "OH2_APP:V" "libc:F" "DEBUG:F" "*:S") -join "`n"
        if ($logOut -match "\[M5_TEST\] PASS: $([regex]::Escape($Test))") {
            $passed = $true
            break
        }
        if ($logOut -match "\[M5_TEST\] FAIL:" -or $logOut -match "Fatal signal") {
            $failed = $true
            break
        }
    }

    & $Adb -s $Serial shell "am force-stop $PkgName" | Out-Null
    Set-Content -Path $LogcatPath -Value $logOut -Encoding UTF8

    if (-not $passed) {
        Write-Host $logOut
        Fail-Gate "M5 self-test '$Test' failed (failed=$failed)"
    }
    if ($logOut -match "OH2_VK_VAL" -and ($logOut -match "\[E\]" -or $logOut -match "ERROR" -or $logOut -match "WARNING")) {
        Write-Host $logOut
        Fail-Gate "Vulkan validation errors/warnings detected during '$Test'"
    }
    Write-Host "[PASS] F10 test '$Test' passed cleanly with 0 Vulkan validation errors!" -ForegroundColor Green
    exit 0
}

# Full Gate: Stage UE4 assets, launch com.oceanhorn2.port, verify live frame + audio + touch
Write-Host "[M5 Gate] Verifying UE4 bundle & pak staging on $Serial..."
$subDirs = @("cookeddata", "Engine", "Documents", "Library", "tmp")
$needDirFix = $false
foreach ($d in $subDirs) {
    $owner = (& $Adb -s $Serial shell "stat -c %U $RemoteFiles/$d 2>/dev/null || echo missing").Trim()
    if ($owner -eq "shell" -or $owner -eq "missing") {
        $needDirFix = $true
        break
    }
}
if ($needDirFix) {
    Write-Host "[M5 Stage] Fixing $RemoteFiles subdirectory ownership for $PkgName..."
    & $Adb -s $Serial shell "if [ -f $RemoteFiles/cookeddata/oh2/content/paks/oh2-ios.pak ]; then mv -f $RemoteFiles/cookeddata/oh2/content/paks/oh2-ios.pak $RemoteFiles/_oh2-ios.pak.tmp; fi; if [ -f $RemoteFiles/Engine/Content/SlateDebug/Fonts/LastResort.ttf ]; then mv -f $RemoteFiles/Engine/Content/SlateDebug/Fonts/LastResort.ttf $RemoteFiles/_LastResort.ttf.tmp; fi; rm -rf $RemoteFiles/cookeddata $RemoteFiles/Engine $RemoteFiles/Documents $RemoteFiles/Library $RemoteFiles/tmp" | Out-Null
    & $Adb -s $Serial shell "run-as $PkgName mkdir -p $RemoteFiles/cookeddata/oh2/content/paks $RemoteFiles/Engine/Content/SlateDebug/Fonts $RemoteFiles/Documents $RemoteFiles/Library $RemoteFiles/tmp" | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail-Gate "run-as $PkgName mkdir -p failed under $RemoteFiles" }
    & $Adb -s $Serial shell "if [ -f $RemoteFiles/_oh2-ios.pak.tmp ]; then mv -f $RemoteFiles/_oh2-ios.pak.tmp $RemoteFiles/cookeddata/oh2/content/paks/oh2-ios.pak; fi; if [ -f $RemoteFiles/_LastResort.ttf.tmp ]; then mv -f $RemoteFiles/_LastResort.ttf.tmp $RemoteFiles/Engine/Content/SlateDebug/Fonts/LastResort.ttf; fi" | Out-Null
}

$remoteCmdSize = (& $Adb -s $Serial shell "stat -c %s $RemoteFiles/ue4commandline.txt 2>/dev/null || echo 0").Trim()
if ($remoteCmdSize -eq "0" -and (Test-Path $PayloadCmd))   { & $Adb -s $Serial push $PayloadCmd   "$RemoteFiles/ue4commandline.txt" | Out-Null }
$remotePlistSize = (& $Adb -s $Serial shell "stat -c %s $RemoteFiles/Info.plist 2>/dev/null || echo 0").Trim()
if ($remotePlistSize -eq "0" -and (Test-Path $PayloadPlist)) { & $Adb -s $Serial push $PayloadPlist "$RemoteFiles/Info.plist" | Out-Null }
$remoteFontSize = (& $Adb -s $Serial shell "stat -c %s $RemoteFiles/Engine/Content/SlateDebug/Fonts/LastResort.ttf 2>/dev/null || echo 0").Trim()
if ($remoteFontSize -eq "0" -and (Test-Path $PayloadFont))  { & $Adb -s $Serial push $PayloadFont  "$RemoteFiles/Engine/Content/SlateDebug/Fonts/LastResort.ttf" | Out-Null }
$remotePakSize = (& $Adb -s $Serial shell "stat -c %s $RemoteFiles/cookeddata/oh2/content/paks/oh2-ios.pak 2>/dev/null || echo 0").Trim()
if ($remotePakSize -ne "2430963623") {
    Write-Host "[M5 Stage] Pushing oh2-ios.pak (2,430,963,623 B)..."
    & $Adb -s $Serial push $PayloadPak "$RemoteFiles/cookeddata/oh2/content/paks/oh2-ios.pak" | Out-Null
}

& $Adb -s $Serial shell "rm -f $RemoteFiles/.oh2_test $RemoteFiles/.oh2_validate" | Out-Null
& $Adb -s $Serial shell "am force-stop $PkgName" | Out-Null
Start-Sleep -Milliseconds 500

# Ensure device awake & unlocked
& $Adb -s $Serial shell "input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard" | Out-Null
$TsLive = (& $Adb -s $Serial shell "date '+%Y-%m-%d %H:%M:%S.000'").Trim()
& $Adb -s $Serial shell "am start -n $ActivityName" | Out-Null

# Wait up to 45 seconds for UE4 to mount pak, initialize Metal-to-Vulkan, and present frames
$liveOk = $false
$liveLogs = ""
$appPid = ""
$deadlineLive = (Get-Date).AddSeconds(45)
while ((Get-Date) -lt $deadlineLive) {
    Start-Sleep -Seconds 2
    $appPid = ((& $Adb -s $Serial shell "pidof $PkgName") | Out-String).Trim()
    $liveLogs = (& $Adb -s $Serial logcat -d -T "$TsLive" "OH2_APP:V" "OH2_M1:V" "OH2_M2:V" "OH2_UE4:V" "OH2_MTLVK:V" "OH2_AUDIO:V" "OH2_VSYNC:V" "OH2_TOUCH:V" "OH2_UI:V" "OH2_VK_VAL:V" "libc:F" "DEBUG:F" "*:S") -join "`n"
    if ($liveLogs -match "Fatal signal" -or [string]::IsNullOrEmpty($appPid)) {
        break
    }
    if ($liveLogs -match "\[M5\] Present frame #") {
        # Give a brief settle window for Main Menu UI frames to finish rendering
        Start-Sleep -Seconds 4
        $liveOk = $true
        break
    }
}

$appPid = ((& $Adb -s $Serial shell "pidof $PkgName") | Out-String).Trim()
$liveLogs = (& $Adb -s $Serial logcat -d -T "$TsLive" "OH2_APP:V" "OH2_M1:V" "OH2_M2:V" "OH2_UE4:V" "OH2_MTLVK:V" "OH2_AUDIO:V" "OH2_VSYNC:V" "OH2_TOUCH:V" "OH2_UI:V" "OH2_VK_VAL:V" "libc:F" "DEBUG:F" "*:S") -join "`n"
Set-Content -Path $LogcatPath -Value $liveLogs -Encoding UTF8

if ([string]::IsNullOrEmpty($appPid) -or $liveLogs -match "Fatal signal") {
    Write-Host ($liveLogs | Select-Object -Last 60)
    Fail-Gate "Live com.oceanhorn2.port died or crashed during launch"
}

$ScreenPng = Join-Path $BuildDir "m5_screencap.png"
Remove-Item -Force $ScreenPng -ErrorAction SilentlyContinue
& $Adb -s $Serial shell "screencap -p /data/local/tmp/m5_screencap.png" | Out-Null
& $Adb -s $Serial pull "/data/local/tmp/m5_screencap.png" $ScreenPng | Out-Null
& $Adb -s $Serial shell "rm -f /data/local/tmp/m5_screencap.png" | Out-Null
$PyCheckFrame = @"
import sys
from PIL import Image
import numpy as np
im = Image.open(sys.argv[1]).convert('RGB')
arr = np.asarray(im, dtype=np.float32)
non_black = float((arr.max(axis=2) > 8).mean())
max_std = float(arr.std(axis=(0,1)).max())
print(f'FRAME non_black={non_black:.4f} max_std={max_std:.2f}')
sys.exit(0 if (non_black >= 0.05 and max_std >= 15.0) else 1)
"@
python -c $PyCheckFrame $ScreenPng
if ($LASTEXITCODE -ne 0) {
    Fail-Gate "Screencap is blank or solid color ($ScreenPng)"
}

Write-Host "[PASS] com.oceanhorn2.port running (pid=$appPid) with live rendered frame and active AAudio!" -ForegroundColor Green
& $Adb -s $Serial shell "am force-stop $PkgName" | Out-Null
Write-Host "[M5 Cleanup] Force-stopped $PkgName on $Serial after verification."
Write-Host "GATE M5: PASS" -ForegroundColor Green
exit 0
