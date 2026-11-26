<#
.SYNOPSIS
    Dot-sourceable environment setup script for OH2Recomp (Oceanhorn 2 Android Vulkan Runtime).
    Auto-detects Android SDK/NDK/CMake and Vulkan SDK, with optional local overrides via scripts/env.local.ps1.
.EXAMPLE
    . .\scripts\env.ps1
#>

$SdkRoot = $env:ANDROID_HOME
if (-not $SdkRoot -or -not (Test-Path $SdkRoot)) {
    $SdkRoot = $env:ANDROID_SDK_ROOT
}
if (-not $SdkRoot -or -not (Test-Path $SdkRoot)) {
    if ($env:LOCALAPPDATA) {
        $CandidateSdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"
        if (Test-Path $CandidateSdk) { $SdkRoot = $CandidateSdk }
    }
}
if (-not $SdkRoot -or -not (Test-Path $SdkRoot)) {
    if ($env:USERPROFILE) {
        $CandidateSdk = Join-Path $env:USERPROFILE "AppData\Local\Android\Sdk"
        if (Test-Path $CandidateSdk) { $SdkRoot = $CandidateSdk }
    }
}

$NdkRoot = $env:ANDROID_NDK_HOME
if (-not $NdkRoot -or -not (Test-Path $NdkRoot)) {
    $NdkRoot = $env:ANDROID_NDK_ROOT
}
if ((-not $NdkRoot -or -not (Test-Path $NdkRoot)) -and $SdkRoot -and (Test-Path (Join-Path $SdkRoot "ndk"))) {
    $PreferredNdk = Join-Path $SdkRoot "ndk\28.2.13676358"
    if (Test-Path $PreferredNdk) {
        $NdkRoot = $PreferredNdk
    } else {
        $LatestNdk = Get-ChildItem -Directory (Join-Path $SdkRoot "ndk") | Sort-Object Name -Descending | Select-Object -First 1
        if ($LatestNdk) { $NdkRoot = $LatestNdk.FullName }
    }
}

if (-not $env:VULKAN_SDK -or -not (Test-Path $env:VULKAN_SDK)) {
    if (Test-Path "C:\VulkanSDK") {
        $PreferredVk = "C:\VulkanSDK\1.4.350.0"
        if (Test-Path $PreferredVk) {
            $env:VULKAN_SDK = $PreferredVk
        } else {
            $LatestVk = Get-ChildItem -Directory "C:\VulkanSDK" | Sort-Object Name -Descending | Select-Object -First 1
            if ($LatestVk) { $env:VULKAN_SDK = $LatestVk.FullName }
        }
    }
}

$LocalEnvOverride = Join-Path $PSScriptRoot "env.local.ps1"
if (Test-Path $LocalEnvOverride) {
    . $LocalEnvOverride
}

if ($SdkRoot) {
    $env:ANDROID_HOME = $SdkRoot
    $env:ANDROID_SDK_ROOT = $SdkRoot
}
if ($NdkRoot) {
    $env:ANDROID_NDK_HOME = $NdkRoot
    $env:ANDROID_NDK_ROOT = $NdkRoot
}

$RequiredPathDirs = @()
if ($SdkRoot) {
    $RequiredPathDirs += (Join-Path $SdkRoot "platform-tools")
    $CmakeBase = Join-Path $SdkRoot "cmake"
    if (Test-Path $CmakeBase) {
        $PreferredCmake = Join-Path $CmakeBase "3.22.1\bin"
        if (Test-Path $PreferredCmake) {
            $RequiredPathDirs += $PreferredCmake
        } else {
            $LatestCmake = Get-ChildItem -Directory $CmakeBase | Sort-Object Name -Descending | Select-Object -First 1
            if ($LatestCmake) { $RequiredPathDirs += (Join-Path $LatestCmake.FullName "bin") }
        }
    }
}
if ($NdkRoot) {
    $RequiredPathDirs += (Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin")
}
if ($env:VULKAN_SDK) {
    $VulkanBin = Join-Path $env:VULKAN_SDK "Bin"
    if (Test-Path $VulkanBin) {
        $RequiredPathDirs += $VulkanBin
    }
}

$CurrentParts = @(
    $env:PATH -split ';' |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -ne '' }
)
$ToPrepend = @()

foreach ($Dir in $RequiredPathDirs) {
    if ($Dir -and (Test-Path $Dir)) {
        $NormDir = $Dir.Trim().TrimEnd('\')
        $AlreadyPresent = $false
        foreach ($Existing in $CurrentParts) {
            if ($Existing.Trim().TrimEnd('\') -ieq $NormDir) {
                $AlreadyPresent = $true
                break
            }
        }
        if (-not $AlreadyPresent) {
            $ToPrepend += $Dir
        }
    }
}

if ($ToPrepend.Count -gt 0) {
    $env:PATH = ($ToPrepend -join ';') + ';' + ($CurrentParts -join ';')
}
