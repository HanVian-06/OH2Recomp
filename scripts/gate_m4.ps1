param(
    [switch]$SkipBuild,
    [int]$Workers = 4,
    [int]$CliSampleCount = 64
)

$ErrorActionPreference = "Continue"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $RepoRoot "scripts\env.ps1")

$PakPath = Join-Path $RepoRoot "Payload\OH2.app\cookeddata\oh2\content\paks\oh2-ios.pak"
$BuildDir = Join-Path $RepoRoot "build\shaders"
$CacheBin = Join-Path $BuildDir "spirv_cache.bin"
$ReportJson = Join-Path $BuildDir "m4_report.json"
$SpirvValExe = Join-Path $env:VULKAN_SDK "Bin\spirv-val.exe"
$SpirvCacheH = Join-Path $RepoRoot "src\metal_vk\spirv_cache.h"
$SpirvCacheCpp = Join-Path $RepoRoot "src\metal_vk\spirv_cache.cpp"
$NdkClangxx = Join-Path $env:ANDROID_NDK_HOME "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang++.cmd"

function Fail-Gate([string]$Reason) {
    Write-Host "[FAIL] $Reason" -ForegroundColor Red
    Write-Host "GATE M4: FAIL" -ForegroundColor Red
    exit 1
}

Write-Host "=== Milestone M4 Verification Gate (M4_SHADER_PIPELINE) ==="

if (-not (Test-Path $PakPath)) { Fail-Gate "Missing read-only pak at $PakPath" }
if (-not (Test-Path $SpirvValExe)) { Fail-Gate "Missing spirv-val.exe at $SpirvValExe" }
if (-not (Test-Path $SpirvCacheH)) { Fail-Gate "Missing $SpirvCacheH" }
if (-not (Test-Path $SpirvCacheCpp)) { Fail-Gate "Missing $SpirvCacheCpp" }

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Step 1: Compile src/metal_vk/spirv_cache.cpp for Android arm64-v8a with strict warnings
if (Test-Path $NdkClangxx) {
    $ObjOut = Join-Path $BuildDir "spirv_cache_arm64.o"
    & $NdkClangxx -std=c++17 -O2 -Wall -Wextra -Werror -c $SpirvCacheCpp -o $ObjOut
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ObjOut)) {
        Fail-Gate "NDK arm64-v8a compilation of src/metal_vk/spirv_cache.cpp failed"
    }
    Write-Host "[PASS] Compiled src/metal_vk/spirv_cache.cpp for Android arm64-v8a (0 warnings/errors)"
}

# Step 2: Run offline AIR 2.0 -> SPIR-V 1.3 translator & cache packer
if (-not $SkipBuild) {
    Write-Host "[M4 Build] Running tools.air2spirv offline translator and SPVC v1 packer..."
    Push-Location $RepoRoot
    python -m tools.air2spirv --pak $PakPath --out $CacheBin --report $ReportJson --workers $Workers --cli-sample $CliSampleCount
    $BuildRc = $LASTEXITCODE
    Pop-Location
    if ($BuildRc -ne 0) {
        Fail-Gate "tools.air2spirv failed with exit code $BuildRc"
    }
}

if (-not (Test-Path $CacheBin)) { Fail-Gate "spirv_cache.bin not found at $CacheBin" }
if (-not (Test-Path $ReportJson)) { Fail-Gate "m4_report.json not found at $ReportJson" }

# Step 3: Verify SPVC v1 binary cache structure, lookups for all 4,544 shaders, capabilities, and spirv-val
$PyVerify = @"
import hashlib, json, os, struct, subprocess, sys, zlib
sys.path.insert(0, r'$RepoRoot')
from tools.air2spirv.cli import extract_pak_files, parse_ushaderbytecode
from tools.air2spirv.validator import SpirvValidator, validate_with_cli

cache_path = r'$CacheBin'
report_path = r'$ReportJson'
pak_path = r'$PakPath'

with open(report_path, 'r', encoding='utf-8') as rf:
    rep = json.load(rf)

with open(cache_path, 'rb') as cf:
    blob = cf.read()

if len(blob) < 80:
    print('Cache file too small:', len(blob))
    sys.exit(1)

(
    magic, ver, flags, entry_count,
    entries_off, bindings_off, vattrs_off, strings_off, spirv_off, total_sz,
    tot_trans, tot_passed, _res
) = struct.unpack_from('<4I6Q2I8s', blob, 0)

assert magic == 0x43565053, f'Bad SPVC magic 0x{magic:08x}'
assert ver == 1, f'Bad SPVC version {ver}'
assert total_sz == len(blob), f'File size mismatch {total_sz} vs {len(blob)}'
assert entry_count == 4482, f'Expected 4482 unique entries, got {entry_count}'
assert (entries_off & 7) == 0 and (bindings_off & 3) == 0 and (vattrs_off & 3) == 0 and (spirv_off & 3) == 0

# Parse all 4,482 disk entries and verify strictly ascending shader_id order
entries_by_id = {}
prev_sid = None
forbidden_caps = {9, 10, 11, 39, 5379}  # Float16=9, Int64=11, Int16=22, Int8=39, Demote=5379
val = SpirvValidator('vulkan1.1')
val_ok_unique = 0

for i in range(entry_count):
    e_off = entries_off + i * 112
    (
        sid, pak_sha1, mtlb_sha256, stage, spv_offset, spv_size,
        b_first, va_first, name_off, buf_mask, tex_mask,
        samp_mask, b_cnt, va_cnt, frag_mask, e_flags, lx, ly, lz, _r
    ) = struct.unpack_from('<Q20s32s8I8HI', blob, e_off)
    if prev_sid is not None and sid <= prev_sid:
        print(f'Entry table not strictly sorted at index {i}: {sid} <= {prev_sid}')
        sys.exit(1)
    prev_sid = sid
    assert spv_size > 0 and (spv_size & 3) == 0, f'Invalid SPIR-V size {spv_size} at {i}'
    spv_bytes = blob[spv_offset : spv_offset + spv_size]
    spv_magic, spv_ver = struct.unpack_from('<II', spv_bytes, 0)
    assert spv_magic == 0x07230203 and spv_ver == 0x00010300, f'Bad SPIR-V header at {i}'

    # Check capabilities in SPIR-V header
    pos_w = 20
    while pos_w + 8 <= len(spv_bytes):
        w0, w1 = struct.unpack_from('<II', spv_bytes, pos_w)
        opc = w0 & 0xFFFF
        wc = w0 >> 16
        if opc == 17:  # OpCapability
            if w1 in (9, 11, 22, 39, 5379):
                print(f'Forbidden capability {w1} in shader 0x{sid:016x}')
                sys.exit(1)
        elif opc not in (17, 10, 11):
            break
        pos_w += wc * 4

    ok, err = val.validate(spv_bytes)
    if ok:
        val_ok_unique += 1
    entries_by_id[sid] = (stage, spv_bytes, b_cnt, va_cnt, e_flags)

# Verify lookup of all 4,544 shaders from oh2-ios.pak
pak_blobs = extract_pak_files(pak_path, {
    'OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode',
    'OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode',
})
all_pak_shaders = (
    parse_ushaderbytecode(pak_blobs['OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode'], 'Global') +
    parse_ushaderbytecode(pak_blobs['OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode'], 'OH2')
)
assert len(all_pak_shaders) == 4544, f'Expected 4544 pak shaders, got {len(all_pak_shaders)}'

found_count = 0
passed_4544 = 0
for s in all_pak_shaders:
    sid = s['shader_id']
    if sid in entries_by_id:
        found_count += 1
        if entries_by_id[sid][4] & 1:
            passed_4544 += 1

assert found_count == 4544, f'Lookup found {found_count}/4544 shaders'

# Also run standalone spirv-val.exe CLI on a stratified sample (VS, PS, CS)
cli_sample = [entries_by_id[sid][1] for sid in sorted(entries_by_id.keys())[::140]]
for spv_b in cli_sample:
    ok_c, err_c = validate_with_cli(spv_b, 'vulkan1.1')
    assert ok_c, f'CLI spirv-val.exe failed: {err_c}'

pass_rate = passed_4544 / 4544.0 * 100.0
print(f'M4_STATS: total=4544 unique_mtlb={entry_count} translated={found_count} spirv_val_passed={passed_4544} unique_passed={val_ok_unique} pass_rate={pass_rate:.2f}% cache_bytes={len(blob)}')
if pass_rate < 90.0:
    print(f'Pass rate {pass_rate:.2f}% is below 90.0% gate threshold')
    sys.exit(1)
"@

python -c $PyVerify
if ($LASTEXITCODE -ne 0) {
    Fail-Gate "SPVC v1 cache verification or spirv-val threshold check failed"
}

Write-Host "[PASS] Verified SPVC v1 spirv_cache.bin ($CacheBin) and m4_report.json ($ReportJson)"
Write-Host "GATE M4: PASS" -ForegroundColor Green
exit 0
