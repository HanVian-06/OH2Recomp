# Shader & AIR Bitcode Analysis (`oh2-ios.pak`)

**Reproducible Verification Script**: `python scripts/analysis/inspect_shaders_pak.py` (supports `--json`)  
**Target Pak (Read-Only)**: `Payload/OH2.app/cookeddata/oh2/content/paks/oh2-ios.pak`

---

## 1. Shader Inventory (`4,544` Shaders Total)
Zero loose `.metallib` or `.metal` files exist in `Payload/OH2.app/`. All shaders reside inside `oh2-ios.pak`:

| Pak Entry | Uncompressed Size | Compressed Size | Shader Count & Stage Breakdown |
|---|---:|---:|---|
| `OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode` | `1,236,176` B | `1,236,176` B (uncomp in pak) | **`317`** (`255` PS `freq=3`, `53` VS `freq=0`, `9` CS `freq=5`) |
| `OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode` | `44,651,192` B | `44,556,076` B | **`4,227`** (`2,743` PS `freq=3`, `1,484` VS `freq=0`, `0` CS) |
| **Total** | **`45,887,368` B** | **`45,792,252` B** | **`4,544`** (`2,998` PS, `1,537` VS, `9` CS) |

*(Additional metadata files: `OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderpipelines` = `50,136` B; `Engine/GlobalShaderCache-SF_METAL.bin` = `184,034` B.)*

---

## 2. Format & Feature Census Across All `4,544` Shaders
Decompressing every entry (`zlib`) inside both `.ushaderbytecode` archives yields a UE4 `FMetalCodeHeader` followed by an Apple `MTLB` container:

| Property | Global (`317`) | OH2 (`4,227`) | Total (`4,544`) |
|---|---:|---:|---:|
| **`MTLB` Container Present** | `317 / 317` | `4,227 / 4,227` | **`4,544 / 4,544` (100%)** |
| **MSL Source Present** | `0 / 317` | `0 / 4,227` | **`0 / 4,544` (0%)** |
| **AIR Version (`VERS = 2.0.x`)** | `317 / 317` | `4,227 / 4,227` | **`4,544 / 4,544` (100%)** |
| **Apple Bitcode Wrapper (`0x0b17c0de`)** | `317 / 317` | `4,227 / 4,227` | **`4,544 / 4,544` (100%)** |
| **LLVM Bitcode (`BC\xc0\xde`)** | `317 / 317` | `4,227 / 4,227` | **`4,544 / 4,544` (100%)** |
| **Tessellation (`TESS` tag)** | `0` | `0` | **`0`** |
| **Function Constants (`CNST` tag)** | `0` | `0` | **`0`** |
| **Argument Buffers (`air.argument`)** | `0` | `0` | **`0`** |
| **Raytracing (`intersection_function`)** | `0` | `0` | **`0`** |
| **Tile / Imageblock Shaders** | `0` | `0` | **`0`** |
| **`air.sample`** | `228` | `3,167` | `3,395` |
| **`half` (FP16)** | `253` | `2,758` | `3,011` |
| **`texturecube`** | `12` | `1,973` | `1,985` |
| **`read_write` / `threadgroup`** | `9` / `5` | `0` / `0` | `9` / `5` |
| **`texture3d` / `texture2d_array`** | `1` / `1` | `3` / `0` | `4` / `1` |

`MTLB` header versions: `4,533` shaders use `(1, 2, 4, 0)` and `11` Global shaders use `(1, 2, 3, 0)`.

---

## 3. M4 Pipeline Strategy Implications
1. **AIR -> SPIR-V Mandatory (ADR-002)**: Because `0 / 4,544` shaders contain MSL source and `4,544 / 4,544` contain Apple LLVM Bitcode (`0x0b17c0de` wrapping `BC\xc0\xde`, AIR 2.0), `SPIRV-Cross` cannot be used as the front-end parser. M4 (`tools/air2spirv/`) must decode the LLVM Bitcode / AIR 2.0 IR (`air.*` intrinsics, Metal 2.0 metadata) and emit SPIR-V modules into `spirv_cache.bin`, validated by `spirv-val` (`>= 90%` pass threshold).
2. **Bounded Feature Surface**: Zero tessellation, zero function constants, zero argument buffers, zero raytracing, and zero tile/imageblock shaders keep M4 focused on VS/PS/CS (`air.sample`, `half`, `texturecube`, `texture3d`, `texture2d_array`, `read_write`, `threadgroup`).
3. **Full `air.*` Intrinsic Inventory**: Exact complete list of `air.*` intrinsics inside abbreviated LLVM bitcode records is `"BELUM DIKETAHUI"` until M4 disassembles all `4,544` bitcode streams with an LLVM bitcode reader / `llvm-dis`.
