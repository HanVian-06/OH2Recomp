<#
.SYNOPSIS
    Dot-sourceable environment setup script for Oceanhorn 2 iOS-to-Android Porting.
.EXAMPLE
    . .\scripts\env.ps1
#>

$SdkRoot = "C:\Users\HanVian\AppData\Local\Android\Sdk"
$NdkVersion = "28.2.13676358"
$NdkRoot = Join-Path $SdkRoot "ndk\$NdkVersion"
$VulkanSdk = "C:\VulkanSDK\1.4.350.0"

$env:ANDROID_HOME = $SdkRoot
$env:ANDROID_SDK_ROOT = $SdkRoot
$env:ANDROID_NDK_HOME = $NdkRoot
$env:ANDROID_NDK_ROOT = $NdkRoot

if (-not $env:VULKAN_SDK -and (Test-Path $VulkanSdk)) {
    $env:VULKAN_SDK = $VulkanSdk
}

$RequiredPathDirs = @(
    (Join-Path $SdkRoot "platform-tools"),
    (Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"),
    (Join-Path $SdkRoot "cmake\3.22.1\bin")
)

if ($env:VULKAN_SDK) {
    $VulkanBin = Join-Path $env:VULKAN_SDK "Bin"
    if (Test-Path $VulkanBin) {
        $RequiredPathDirs += $VulkanBin
    }
}

$CurrentParts = @($env:PATH -split ';' | Where-Object { $_ -and $_.Trim() -ne '' })
$ToPrepend = @()

foreach ($Dir in $RequiredPathDirs) {
    if (Test-Path $Dir) {
        $AlreadyPresent = $false
        foreach ($Existing in $CurrentParts) {
            if ($Existing.TrimEnd('\') -ieq $Dir.TrimEnd('\')) {
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
