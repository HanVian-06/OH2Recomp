<#
.SYNOPSIS
    Verifies all M-1 toolchain prerequisites and Galaxy A56 adb connection.
#>
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Continue'
$EnvScript = Join-Path $PSScriptRoot "env.ps1"
if (-not (Test-Path $EnvScript)) {
    Write-Host "[FAIL] Missing env.ps1 at $EnvScript" -ForegroundColor Red
    exit 1
}
. $EnvScript

$script:Failures = 0

function Test-Check {
    param(
        [string]$Name,
        [scriptblock]$Probe
    )
    try {
        $Result = & $Probe
        if ($Result.Ok) {
            if (-not $Quiet) {
                Write-Host "[PASS] $Name : $($Result.Detail)" -ForegroundColor Green
            }
        } else {
            $script:Failures++
            Write-Host "[FAIL] $Name : $($Result.Detail)" -ForegroundColor Red
        }
    } catch {
        $script:Failures++
        Write-Host "[FAIL] $Name : $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Invoke-CliFirstLine {
    param(
        [string]$Cmd,
        [string[]]$CmdArgs
    )
    $Found = Get-Command $Cmd -ErrorAction SilentlyContinue
    if (-not $Found) {
        return @{ Ok = $false; Detail = "$Cmd not found in PATH" }
    }
    $Out = & $Found.Source @CmdArgs 2>&1
    $Code = $LASTEXITCODE
    $Lines = @($Out | ForEach-Object { $_.ToString().Trim() } | Where-Object { $_ -ne '' })
    $First = if ($Lines.Count -gt 0) { $Lines[0] } else { "(no output)" }
    if ($Code -eq 0) {
        return @{ Ok = $true; Detail = "$First [$($Found.Source)]" }
    } else {
        return @{ Ok = $false; Detail = "exit=$Code ($First)" }
    }
}

Test-Check "ANDROID_HOME" {
    if ($env:ANDROID_HOME -and (Test-Path $env:ANDROID_HOME)) {
        @{ Ok = $true; Detail = $env:ANDROID_HOME }
    } else {
        @{ Ok = $false; Detail = "Unset or missing: $($env:ANDROID_HOME)" }
    }
}

Test-Check "ANDROID_NDK_HOME" {
    if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) {
        @{ Ok = $true; Detail = $env:ANDROID_NDK_HOME }
    } else {
        @{ Ok = $false; Detail = "Unset or missing: $($env:ANDROID_NDK_HOME)" }
    }
}

Test-Check "NDK clang++ (aarch64-linux-android29)" {
    Invoke-CliFirstLine "aarch64-linux-android29-clang++.cmd" @("--version")
}

Test-Check "CMake" {
    Invoke-CliFirstLine "cmake" @("--version")
}

Test-Check "Ninja" {
    Invoke-CliFirstLine "ninja" @("--version")
}

Test-Check "spirv-val" {
    Invoke-CliFirstLine "spirv-val" @("--version")
}

Test-Check "glslc" {
    Invoke-CliFirstLine "glslc" @("--version")
}

Test-Check "spirv-cross" {
    Invoke-CliFirstLine "spirv-cross" @("--help")
}

Test-Check "Java" {
    Invoke-CliFirstLine "java" @("-version")
}

Test-Check "Python" {
    Invoke-CliFirstLine "python" @("--version")
}

Test-Check "Git" {
    Invoke-CliFirstLine "git" @("--version")
}

Test-Check "ADB Galaxy A56 (SM-A566B / RRCY40086KE)" {
    $Adb = Get-Command "adb" -ErrorAction SilentlyContinue
    if (-not $Adb) {
        return @{ Ok = $false; Detail = "adb not found in PATH" }
    }
    $Out = & $Adb.Source devices -l 2>&1
    $Code = $LASTEXITCODE
    if ($Code -ne 0) {
        return @{ Ok = $false; Detail = "adb devices failed (exit=$Code)" }
    }
    $Lines = @($Out | ForEach-Object { $_.ToString() })
    $Match = @($Lines | Where-Object {
        ($_ -match '\bdevice\b') -and (($_ -match 'SM_A566B') -or ($_ -match 'RRCY40086KE') -or ($_ -match 'a56x'))
    })
    if ($Match.Count -gt 0) {
        $Model = (& $Adb.Source shell getprop ro.product.model 2>&1 | Out-String).Trim()
        $Sdk = (& $Adb.Source shell getprop ro.build.version.sdk 2>&1 | Out-String).Trim()
        if ($Model -eq 'SM-A566B') {
            return @{ Ok = $true; Detail = "Online ($Model, API $Sdk) [$($Match[0].Trim())]" }
        }
        return @{ Ok = $false; Detail = "Connected device model '$Model' != 'SM-A566B'" }
    }
    return @{ Ok = $false; Detail = "Galaxy A56 not connected/authorized in 'adb devices -l'" }
}

if ($script:Failures -gt 0) {
    Write-Host "`nM-1 Environment Check FAILED ($($script:Failures) failure(s))." -ForegroundColor Red
    exit 1
}

Write-Host "`nM-1 Environment Check PASSED (all checks green)." -ForegroundColor Green
exit 0
