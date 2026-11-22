# Pak, Texture & Audio Analysis (`oh2-ios.pak`)

**Reproducible Verification Script**: `python scripts/analysis/inspect_shaders_pak.py` (supports `--json`)  
**Target Pak (Read-Only)**: `Payload/OH2.app/cookeddata/oh2/content/paks/oh2-ios.pak` (`2,430,963,623` bytes / `2,318.35 MB`)

---

## 1. UE4 `.pak` v8 Structure (`FPakInfo`)
- **Magic / Version**: `0x5a6f12e1` / **Version `8`** (221-byte footer)
- **Encryption**: `bEncryptedIndex = 0` (`False`), `0 / 19,920` encrypted entries (**unencrypted**)
- **Index**: `IndexOffset = 2,427,412,912`, `IndexSize = 3,550,490` B, `MountPoint = "../../../"`, **`NumEntries = 19,920`**
- **Compression Breakdown**:
  - `Zlib` (`comp_method = 1`): **`17,271`** entries
  - Uncompressed (`comp_method = 0`): **`2,649`** entries
  - Total Uncompressed Payload: **`4,247.69 MB`** (`4,454,021,592` bytes)

---

## 2. Breakdown by File Extension

| Extension | Count | Compressed (B) | Uncompressed (B) |
|---|---:|---:|---:|
| `.ubulk` | 3,597 | 1,558,076,049 | 2,540,957,422 |
| `.uexp` | 7,713 | 747,817,264 | 1,722,852,668 |
| `.umap` | 147 | 7,633,681 | 49,742,847 |
| `.ufont` | 12 | 31,403,341 | 47,676,252 |
| `.ushaderbytecode` | 2 | 45,792,252 | 45,887,368 |
| `.uasset` | 7,566 | 9,227,643 | 24,402,664 |
| `.txt` | 25 | 1,903,462 | 7,605,545 |
| `.ttf` | 13 | 2,793,443 | 5,803,364 |
| `.bin` | 2 | 489,620 | 4,076,482 |
| `.png` / `.res` / `.psd` / `.locres` / `.ini` / other | 843 | 2,247,305 | 5,016,980 |
| **Total** | **19,920** | **2,407,384,060** (+ headers/index) | **4,454,021,592 (`4,247.69 MB`)** |

---

## 3. Texture Pixel Formats (`7,566` `.uasset` Scan)
`OH2/Config/DefaultEngine.ini` confirms `bCookASTCTextures=True`, `bCookPVRTCTextures=False`, `r.Streaming.PoolSize=384`. Scanning all `7,566` `.uasset` files yields:

| Pixel Format | `.uasset` Hits | Vulkan Mapping (M3/M5/M6) |
|---|---:|---|
| `PF_ASTC_8x8` | **827** | `VK_FORMAT_ASTC_8x8_UNORM_BLOCK` / `_SRGB_BLOCK` |
| `PF_ASTC_4x4` | **617** | `VK_FORMAT_ASTC_4x4_UNORM_BLOCK` / `_SRGB_BLOCK` |
| `PF_B8G8R8A8` | **195** | `VK_FORMAT_B8G8R8A8_UNORM` / `_SRGB` |
| `PF_G8` | **194** | `VK_FORMAT_R8_UNORM` |
| `PF_ASTC_6x6` | **38** | `VK_FORMAT_ASTC_6x6_UNORM_BLOCK` / `_SRGB_BLOCK` |
| `PF_FloatRGBA` | **13** | `VK_FORMAT_R16G16B16A16_SFLOAT` |
| BCn / ETC2 / PVRTC | **0** | Not used |

Per-texture mip dimensions inside `.ubulk` are `"BELUM DIKETAHUI"` until runtime texture streaming or a dedicated `.uasset` bulk header parser in M5/M6.

---

## 4. Audio Format & Memory Implications (M5 / M6)
- `Engine/Config/IOS/IOSEngine.ini` & `OH2/Config/IOS/IOSEngine.ini`: `AudioDeviceModuleName=IOSAudio`, `[MemoryMappedFiles] MasterEnable=true`.
- `OH2/Config/DefaultEngine.ini`: `AudioSampleRate=44100` (and `48000` in one section).
- Sampling `10` `OH2/Content/Audio/*.m.ubulk` payloads inside `oh2-ios.pak` confirmed **`10 / 10` are `RIFF/WAVE` with `fmt ` `wFormatTag = 0x0002` (Microsoft ADPCM)** at `44100 Hz`. M5 (`AAudio`) must include an MS-ADPCM -> PCM16 decoder before feeding `AAudio`.
- **M6 Memory Strategy (`<= 2.5 GB`)**: `mmap` `oh2-ios.pak` for zero-copy read access to the `2,649` uncompressed entries and compressed stream reads, decompress the `17,271` zlib entries strictly on-demand within UE4's `r.Streaming.PoolSize` (`256–384 MB`), evict caches on `onTrimMemory`, and upload `PF_ASTC_*` directly to Vulkan (`textureCompressionASTC_LDR = 1.0` on Galaxy A56) with **zero RGBA8 decompression**.
