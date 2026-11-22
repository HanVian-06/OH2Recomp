param(
    [string]$BuildType = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $RepoRoot "scripts\env.ps1")

$BuildDir = Join-Path $RepoRoot "build\m3"
$CmakeBuildDir = Join-Path $BuildDir "cmake"
$AssetsShaderDir = Join-Path $BuildDir "assets\shaders"

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $CmakeBuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $AssetsShaderDir | Out-Null

# 1. Compile and validate Vulkan 1.1 SPIR-V shaders
$Glslc = Join-Path $env:VULKAN_SDK "Bin\glslc.exe"
if (-not (Test-Path $Glslc)) {
    $Glslc = Join-Path $env:ANDROID_NDK_HOME "shader-tools\windows-x86_64\glslc.exe"
}
$SpirvVal = Join-Path $env:VULKAN_SDK "Bin\spirv-val.exe"
if (-not (Test-Path $Glslc)) { throw "glslc.exe not found" }
if (-not (Test-Path $SpirvVal)) { throw "spirv-val.exe not found" }

$VertSrc = Join-Path $RepoRoot "src\vk_foundation\shaders\triangle.vert"
$FragSrc = Join-Path $RepoRoot "src\vk_foundation\shaders\triangle.frag"
$VertSpv = Join-Path $AssetsShaderDir "triangle.vert.spv"
$FragSpv = Join-Path $AssetsShaderDir "triangle.frag.spv"

Write-Host "[M3 Build] Compiling GLSL -> SPIR-V (target-env=vulkan1.1)..."
& $Glslc --target-env=vulkan1.1 -O $VertSrc -o $VertSpv
if ($LASTEXITCODE -ne 0) { throw "glslc failed on triangle.vert" }
& $Glslc --target-env=vulkan1.1 -O $FragSrc -o $FragSpv
if ($LASTEXITCODE -ne 0) { throw "glslc failed on triangle.frag" }

Write-Host "[M3 Build] Validating SPIR-V with spirv-val..."
& $SpirvVal --target-env vulkan1.1 $VertSpv
if ($LASTEXITCODE -ne 0) { throw "spirv-val failed on triangle.vert.spv" }
& $SpirvVal --target-env vulkan1.1 $FragSpv
if ($LASTEXITCODE -ne 0) { throw "spirv-val failed on triangle.frag.spv" }

# 2. Configure & build native library via CMake + Ninja + NDK r28c
$CmakeExe = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\cmake.exe"
$NinjaExe = Join-Path $env:ANDROID_HOME "cmake\3.22.1\bin\ninja.exe"
$Toolchain = Join-Path $env:ANDROID_NDK_HOME "build\cmake\android.toolchain.cmake"
$SandboxSrc = Join-Path $RepoRoot "android\m3_vk_sandbox"

Write-Host "[M3 Build] Configuring CMake (arm64-v8a, android-29)..."
& $CmakeExe `
    -S $SandboxSrc `
    -B $CmakeBuildDir `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    -DANDROID_ABI=arm64-v8a `
    -DANDROID_PLATFORM=android-29 `
    -DANDROID_STL=c++_static `
    "-DCMAKE_BUILD_TYPE=$BuildType"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "[M3 Build] Building libm3_vk_sandbox.so..."
& $CmakeExe --build $CmakeBuildDir --config $BuildType
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

$LibSandboxSo = Join-Path $CmakeBuildDir "libm3_vk_sandbox.so"
$LibValidationSo = Join-Path $RepoRoot "third_party\vulkan_validation\arm64-v8a\libVkLayer_khronos_validation.so"
if (-not (Test-Path $LibSandboxSo)) { throw "Missing built $LibSandboxSo" }
if (-not (Test-Path $LibValidationSo)) { throw "Missing validation layer $LibValidationSo" }

# 3. Package APK with aapt2 + zipalign + apksigner
$BuildToolsDir = Join-Path $env:ANDROID_HOME "build-tools\36.0.0"
$Aapt2 = Join-Path $BuildToolsDir "aapt2.exe"
$ZipAlign = Join-Path $BuildToolsDir "zipalign.exe"
$ApkSigner = Join-Path $BuildToolsDir "apksigner.bat"

$PlatformJar = Join-Path $env:ANDROID_HOME "platforms\android-36.1\android.jar"
if (-not (Test-Path $PlatformJar)) {
    $PlatformJar = Join-Path $env:ANDROID_HOME "platforms\android-37.0\android.jar"
}
if (-not (Test-Path $PlatformJar)) { throw "android.jar not found in platforms" }

$ManifestPath = Join-Path $SandboxSrc "AndroidManifest.xml"
$AssetsRoot = Join-Path $BuildDir "assets"
$UnalignedApk = Join-Path $BuildDir "m3_vk_sandbox-unaligned.apk"
$AlignedApk = Join-Path $BuildDir "m3_vk_sandbox-aligned.apk"
$FinalApk = Join-Path $BuildDir "m3_vk_sandbox-debug.apk"

if (Test-Path $UnalignedApk) { Remove-Item -Force $UnalignedApk }
if (Test-Path $AlignedApk) { Remove-Item -Force $AlignedApk }
if (Test-Path $FinalApk) { Remove-Item -Force $FinalApk }

Write-Host "[M3 Build] Linking base APK via aapt2..."
& $Aapt2 link `
    -o $UnalignedApk `
    --manifest $ManifestPath `
    -I $PlatformJar `
    -A $AssetsRoot `
    --min-sdk-version 29 `
    --target-sdk-version 36
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

Write-Host "[M3 Build] Injecting arm64-v8a native libraries (ZIP_STORED)..."
$PyInject = @"
import sys, zipfile
apk_path, sandbox_so, val_so = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(apk_path, 'a') as zf:
    zf.write(sandbox_so, 'lib/arm64-v8a/libm3_vk_sandbox.so', compress_type=zipfile.ZIP_STORED)
    zf.write(val_so, 'lib/arm64-v8a/libVkLayer_khronos_validation.so', compress_type=zipfile.ZIP_STORED)
"@
python -c $PyInject $UnalignedApk $LibSandboxSo $LibValidationSo
if ($LASTEXITCODE -ne 0) { throw "Python zipfile library injection failed" }

Write-Host "[M3 Build] Running zipalign (-P 16 4)..."
& $ZipAlign -f -P 16 4 $UnalignedApk $AlignedApk
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

$KeystoreDir = Join-Path $env:USERPROFILE ".android"
$Keystore = Join-Path $KeystoreDir "debug.keystore"
if (-not (Test-Path $Keystore)) {
    New-Item -ItemType Directory -Force -Path $KeystoreDir | Out-Null
    Write-Host "[M3 Build] Generating debug.keystore..."
    keytool -genkeypair -v -keystore $Keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
    if ($LASTEXITCODE -ne 0) { throw "keytool failed generating debug.keystore" }
}

Write-Host "[M3 Build] Signing APK with apksigner..."
& $ApkSigner sign `
    --ks $Keystore `
    --ks-key-alias androiddebugkey `
    --ks-pass pass:android `
    --key-pass pass:android `
    --out $FinalApk `
    $AlignedApk
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Remove-Item -Force $UnalignedApk -ErrorAction SilentlyContinue
Remove-Item -Force $AlignedApk -ErrorAction SilentlyContinue

Write-Host "[M3 Build] Built signed APK: $FinalApk"
