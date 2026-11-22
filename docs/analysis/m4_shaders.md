# M4 Offline AIR 2.0 LLVM Bitcode -> SPIR-V 1.3 Shader Pipeline (`M4_SHADER_PIPELINE`)

## 1. Corpus Census & Archive Structure (`oh2-ios.pak`)
All shaders in Oceanhorn 2 (UE4.23 iOS) reside inside `Payload/OH2.app/cookeddata/oh2/content/paks/oh2-ios.pak`. Zero shaders contain Metal Shading Language (MSL) source text; `100%` (`4,544 / 4,544`) are precompiled Apple `MTLB` containers embedding Apple-wrapped (`0x0b17c0de`) AIR 2.0 LLVM bitcode (`BC\xc0\xde`, target triple `air64-apple-ios11.0.0`).

| Archive / Metadata Entry | Count | Stage Breakdown | Unique `pak_sha1` | Unique `MTLB` SHA-256 (`shader_id`) |
|---|---:|---|---:|---:|
| `OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode` | `317` | `255` PS (`freq=3`), `53` VS (`freq=0`), `9` CS (`freq=5`) | `317` | `313` |
| `OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode` | `4,227` | `2,743` PS (`freq=3`), `1,484` VS (`freq=0`), `0` CS | `4,227` | `4,171` |
| **Combined Shader Corpus** | **`4,544`** | **`2,998` PS, `1,537` VS, `9` CS** | **`4,542`** | **`4,482`** (`0` 64-bit `shader_id` collisions) |
| `OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderpipelines` | `482` | Version `1` graphics pipeline descriptors (`50,136` B) | — | — |

### 1.1 LLVM Bitcode & AIR 2.0 IR Surface Area
Across all `4,482` unique `MTLB` bitcode streams (`1,487,206` main-function SSA instructions):
- **Target Datalayout**: `e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32`
- **Compiler Versions (`!llvm.ident`)**: `Apple LLVM version 902.9 (metalfe-902.9.61)` and `Apple LLVM version 3902.67 (metalfe-3902.67)`.
- **Type Records (`TYPE_BLOCK_ID_NEW = 17`)**: `15` record codes (`VOID`, `FLOAT`, `HALF`, `LABEL`, `OPAQUE`, `INTEGER` widths `{1, 3, 8, 16, 32, 64}`, `POINTER` addrspaces `{0, 1, 2, 3}`, `ARRAY`, `VECTOR` lengths `{2, 3, 4}`, `METADATA`, `STRUCT_ANON`, `STRUCT_NAME`, `STRUCT_NAMED`, `FUNCTION`).
- **Constant Records (`CONSTANTS_BLOCK = 11`)**: `7` record codes (`SETTYPE`, `NULL`, `UNDEF`, `INTEGER`, `FLOAT`, `AGGREGATE`, `DATA`).
- **Instruction Records (`FUNCTION_BLOCK = 12`)**: `18` instruction opcodes (`INST_BINOP`, `INST_CAST`, `INST_EXTRACTELT`, `INST_INSERTELT`, `INST_SHUFFLEVEC`, `INST_RET`, `INST_BR`, `INST_SWITCH`, `INST_UNREACHABLE`, `INST_PHI`, `INST_ALLOCA`, `INST_LOAD`, `INST_INSERTVAL`, `INST_CMP2`, `INST_VSELECT`, `INST_CALL`, `INST_GEP`, `INST_STORE`).
- **Function Definitions per Module**: `4,175` shaders define `1` function (`@Main_...`); `369` shaders define `6` functions (`@Main_...` plus internal `___metal_fract_v2float` or `___metal_fract_v3float` and its 4 sub-helpers, which compute `fract(v)`).
- **Control Flow Complexity**:
  - `2,128` unique shaders are single-basic-block (`1` BB).
  - `2,303` unique shaders are multi-block acyclic DAGs (`2..N` BBs, `0` loop back-edges).
  - `51` unique shaders contain loop back-edges (up to `380` BBs and `40` `phi` nodes in `Global[202]` RadixSort CS).
  - `18` shaders contain `switch` instructions (`76` total sites); `6` PS shaders contain unconditional `air.discard_fragment` + `unreachable` with no `ret`.
- **Opaque Texture & Sampler Types**:
  - `texture2d<float, sample>` (`3,376` shaders), `texturecube<float, sample>` (`1,985` shaders), `texture2d_ms<float, read>` (`6` shaders), `texture3d<float, sample>` (`4` shaders), `texture2d_array<float, sample>` (`1` shader), `texture2d<uint, sample>` (CS/VS buffers), and `sampler` (`3,111` shaders).
  - Zero tessellation, zero function constants, zero argument buffers, zero raytracing, zero tile/imageblock shaders, and zero `STORAGE_IMAGE` (`Sampled=2`) textures (`all 9` `"air.read_write"` shaders operate on `"air.buffer"` SSBOs in `Global` CS).

---

## 2. AIR 2.0 -> Vulkan 1.1 SPIR-V 1.3 Translation Rules (`tools/air2spirv/`)

| Component | Path | Responsibility |
|---|---|---|
| **Bitcode & Container Decoder** | `tools/air2spirv/bitcode_reader.py` | `BitReader`, `parse_bitstream`, `parse_fmetal_code_header`, `parse_mtlb_container`, `IRModule` (types, constants, metadata, VST, SSA instructions, datalayout byte offsets) |
| **SPIR-V 1.3 Emitter** | `tools/air2spirv/air_to_spirv.py` | `SpirvBuilder`, `translate_air_shader`: lowers AIR 2.0 IR and metadata into Vulkan 1.1 SPIR-V 1.3 (`0x00010300`) binary modules + reflection tables |
| **Vulkan 1.1 Validator** | `tools/air2spirv/validator.py` | In-process `SPIRV-Tools-shared.dll` (`spvValidateBinary`, `vulkan1.1`) + standalone `spirv-val.exe --target-env vulkan1.1` CLI verifier |
| **Batch Driver & Cache Packer** | `tools/air2spirv/cli.py` | Parallel `.pak` extraction, deduplicated translation, validation, `SPVC` v1 `spirv_cache.bin` packing, and `m4_report.json` generation |

### 2.1 Core Vulkan 1.1 Compatibility & Capability Discipline
1. **Binary Header**: SPIR-V `1.3` (`0x00010300`), `OpMemoryModel Logical GLSL450`, `OpCapability Shader` (+ `OpCapability ImageQuery` (`50`) only when `air.get_width_texture_2d` or `air.get_height_texture_2d` is called).
2. **`half` (`f16`) Lowering via `RelaxedPrecision`**:
   - Avoids optional `OpCapability Float16` and `SPV_KHR_16bit_storage`.
   - Maps `f16` and `f32` to `OpTypeFloat 32` (with `RelaxedPrecision` decoration on `half` outputs and `SPIRV_ENTRY_FLAG_RELAXED_PRECISION` recorded in reflection).
   - IEEE-754 16-bit half constants in LLVM bitcode are converted at compile time to 32-bit IEEE-754 float literals, and `air.convert.f.*f16.f.*f32` / `air.convert.f.*f32.f.*f16` / `fpext` / `fptrunc` become zero-cost SSA pass-throughs.
   - Vector-to-scalar `bitcast <2 x half> to i32` (`Global[209]` CS) lowers to `GLSL.std.450 PackHalf2x16`.
3. **Integer Width Normalization**:
   - Avoids optional `Int8`, `Int16`, `Int64` capabilities (and handles LLVM's `i3` narrowed switch type).
   - Maps `i1` to `OpTypeBool` (using `OpLogicalAnd`, `OpLogicalOr`, `OpLogicalEqual`, `OpLogicalNotEqual`, and splatted `OpCompositeConstruct %vNbool` for vector `OpSelect` in SPIR-V 1.3) and maps all non-boolean integers (`i3`, `i8`, `i16`, `i32`, `i64`) to 32-bit `OpTypeInt 32 0` with bitmask truncation where required.
4. **Fragment Discard**:
   - Because `VK_EXT_demote_to_helper_invocation` is `False` on Samsung Galaxy A56 (`Samsung Xclipse 540`), `@air.discard_fragment()` (`458` shaders) emits core `OpKill` and terminates the enclosing basic block cleanly.
5. **Pointer Offset Representation (`addrspace 0, 1, 2, 3`)**:
   - Represents pointers into `addrspace(2)` (`constant` UBO/SSBO), `addrspace(1)` (`device` SSBO), `addrspace(3)` (`Workgroup` shared memory), and `addrspace(0)` (`Function` local `alloca`) as `(base_resource, byte_offset_uint32)` pairs.
   - `getelementptr` computes exact byte offsets from `IRModule.type_layouts` (folding constant offsets at compile time); pointer `bitcast`s (`831` shaders) and pointer `phi`s (`235` sites) operate directly on the 32-bit `byte_offset_uint32` SSA value without requiring `VariablePointers`.
   - Sized read-only `addrspace(2)` buffers emit canonical `Uniform` `Block { vec4 data[N]; }` (`ArrayStride 16`, `Offset 0`); `addrspace(1)` and unsized `addrspace(2)` buffers emit canonical `StorageBuffer` `Block { uint data[]; }` (`ArrayStride 4`, `Offset 0`, plus `NonWritable` when read-only).
6. **`reg2mem` & Single-Entry Single-Exit (SESE) Structured Control Flow**:
   - Cross-block SSA values and `phi` nodes (`2,112` shaders) are promoted to entry-block `OpVariable ... Function` variables initialized in `%entry`, guaranteeing strict SSA dominance across all basic blocks.
   - Single-block shaders (`2,128` unique) emit a single flat `%entry` block ending in `OpReturn` (or `OpKill`).
   - Multi-block acyclic DAG shaders (`2,303` unique) emit a loop-free topologically sorted SESE chain where each non-entry block `b_i` is guarded by its own `OpSelectionMerge %after_i None` diamond.
   - Multi-block cyclic shaders (`51` unique) emit a single outer `OpLoopMerge %loop_exit %loop_cont None` driving an RPO SESE chain where forward edges execute in the same pass and back-edges (`target_pos <= curr_pos`) naturally advance to the single `%loop_cont -> %loop_hdr` latch.

### 2.2 Complete `air.*` Intrinsic Mapping Table (`126` Intrinsics + `7` Internal Helpers)
| AIR 2.0 Intrinsic Family | Shader Count | Call Sites | SPIR-V 1.3 / `GLSL.std.450` Lowering |
|---|---:|---:|---|
| `air.convert.f.*f16.f.*f32` / `air.convert.f.*f32.f.*f16` | `4,242` | `95,844` | Identity SSA pass-through (`OpTypeFloat 32` + `RelaxedPrecision`) |
| `air.convert.f.*.u.i32` / `.u.v2i32` / `.u.v4i32` | `107` | `264` | `OpConvertUToF` |
| `air.convert.f.*.u.i1` / `.u.v4i1` | `328` | `1,116` | `OpSelect %res_ty %cond %const_1_0 %const_0_0` |
| `air.convert.f.*.s.i32` / `.s.v2i32` | `512` | `529` | `OpConvertSToF` |
| `air.convert.u.*.f.*` | `94` | `181` | `OpConvertFToU` |
| `air.convert.s.*.f.*` | `14` | `23` | `OpConvertFToS %s32` + `OpBitcast %u32` |
| `air.sample_texture_2d.v4f32` | `3,066` | `15,901` | `OpSampledImage` + `OpImageSampleImplicitLod` (PS) / `OpImageSampleExplicitLod Lod` (VS/CS) |
| `air.sample_texture_cube.v4f32` | `1,979` | `3,973` | `OpSampledImage` + `OpImageSampleImplicitLod` (PS) / `OpImageSampleExplicitLod Lod` (VS/CS) |
| `air.sample_texture_2d_grad.v4f32` | `82` | `156` | `OpSampledImage` + `OpImageSampleExplicitLod Grad %ddx %ddy` |
| `air.sample_texture_3d.v4f32` | `4` | `7` | `OpSampledImage` + `OpImageSampleImplicitLod` (PS) / `OpImageSampleExplicitLod Lod` (VS/CS) |
| `air.sample_texture_2d_array.v4f32` | `1` | `1` | `OpConvertSToF` layer + `OpCompositeConstruct %v3f32` + `OpImageSampleImplicitLod` |
| `air.gather_texture_2d.v4f32` | `652` | `2,610` | `OpSampledImage` + `OpImageGather %v4f32 %si %coord %component` |
| `air.read_texture_2d.*` (`.v4f32`, `.i16.v4f32`, `.u.v4i32`, `.i16.u.v4i32`) | `388` | `4,351` | `OpImageFetch %vec4 %tex %coord Lod %lod` |
| `air.read_texture_2d_ms.v4f32` | `6` | `28` | `OpImageFetch %v4f32 %tex %coord Sample %sample_idx` |
| `air.get_width_texture_2d` / `air.get_height_texture_2d` | `379` | `828` | `OpCapability ImageQuery` + `OpImageQuerySizeLod` + `OpCompositeExtract` (`0` / `1`) |
| `air.dot.v2f32` / `v3f32` / `v4f32` / `v2f16` / `v3f16` / `v4f16` | `3,400+` | `45,629` | `OpDot %f32 %v1 %v2` |
| `air.fma.*` (`f32`, `v2f32`, `v3f32`, `v4f32`, `f16`, `v3f16`) | `1,500+` | `59,932` | `GLSL.std.450 Fma` (`50`) |
| `air.mix.*` (`f32`, `v2f32`, `v3f32`, `f16`, `v2f16`, `v3f16`, `v4f16`) | `2,100+` | `15,909` | `GLSL.std.450 FMix` (`46`) |
| `air.saturate.*` / `air.fast_saturate.*` | `3,100+` | `32,438` | `GLSL.std.450 FClamp(%x, 0.0, 1.0)` (`43`) |
| `air.clamp.*` / `air.fast_clamp.*` | `1,980+` | `2,292` | `GLSL.std.450 FClamp` (`43`) |
| `air.fmin.*` / `air.fast_fmin.*` / `air.fmax.*` / `air.fast_fmax.*` | `3,100+` | `19,353` | `GLSL.std.450 FMin` (`37`) / `FMax` (`40`) |
| `air.min.u.i32` / `air.max.s.v2i32` / `air.abs.s.v2i32` | `398` | `2,582` | `GLSL.std.450 UMin` (`38`) / `OpSGreaterThan`+`OpSelect` / `OpSLessThan`+`OpSNegate`+`OpSelect` |
| `air.sqrt.*` / `air.fast_sqrt.*` / `air.rsqrt.*` / `air.fast_rsqrt.*` | `2,950+` | `14,997` | `GLSL.std.450 Sqrt` (`31`) / `InverseSqrt` (`32`) |
| `air.exp2.*` / `air.fast_exp2.*` / `air.fast_exp.*` / `air.log2.*` / `air.fast_log.*` / `air.pow.*` / `air.fast_pow.*` | `2,100+` | `9,310` | `GLSL.std.450 Exp2` (`29`) / `Exp` (`27`) / `Log2` (`30`) / `Log` (`28`) / `Pow` (`26`) |
| `air.sin.*` / `air.fast_sin.*` / `air.cos.*` / `air.fast_cos.*` / `air.atan2.*` / `air.fast_atan2.*` | `700+` | `3,459` | `GLSL.std.450 Sin` (`13`) / `Cos` (`14`) / `Atan2` (`25`) |
| `air.fast_sincos.f32` / `air.fast_sincos.v3f32` / `air.sincos.f16` | `177` | `177` | `GLSL.std.450 Sin` (return) + `GLSL.std.450 Cos` (`OpStore` to out pointer) |
| `air.floor.*` / `air.fast_floor.*` / `air.fast_trunc.*` / `air.fract.*` / `air.fast_fract.*` / `___metal_fract_*` | `1,200+` | `4,462` | `GLSL.std.450 Floor` (`8`) / `Trunc` (`3`) / `Fract` (`10`) |
| `air.fabs.*` / `air.fast_fabs.*` / `air.sign.*` / `air.fast_fmod.f32` | `900+` | `1,955` | `GLSL.std.450 FAbs` (`4`) / `FSign` (`6`) / `OpFRem` (`140`) |
| `air.dfdx.*` / `air.dfdy.*` | `134` | `267` | `OpDPdx` (`207`) / `OpDPdy` (`208`) |
| `air.any.v3i1` / `air.reverse_bits.i32` / `air.pack.unorm2x16.v2f32` | `3` | `5` | `OpAny` (`154`) / `OpBitReverse` (`204`) / `GLSL.std.450 PackUnorm2x16` (`57`) |
| `air.discard_fragment` | `458` | `498` | `OpKill` (`252`) |
| `air.wg.barrier` | `3` | `24` | `OpControlBarrier Workgroup Workgroup AcquireRelease\|WorkgroupMemory (0x108)` |

---

## 3. Descriptor Set, Binding & Stage I/O ABI (`M4` <-> `M5`)

| Vulkan Descriptor Set | Metal Stage | Metal Resource Slot | Vulkan Binding Formula | Observed Max Slot | Vulkan Descriptor Type |
|---:|---|---|---|---:|---|
| **Set 0** | Vertex (`VS`) / Compute (`CS`) | `[[buffer(0..30)]]` (non-`stage_in`) | `binding = slot` (`0..30`) | `8` | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` (`6`) or `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` (`7`) |
| **Set 0** | Vertex / Compute | `[[texture(0..31)]]` | `binding = 32 + slot` (`32..63`) | `15` | `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` (`2`) |
| **Set 0** | Vertex / Compute | `[[sampler(0..15)]]` | `binding = 64 + slot` (`64..79`) | `15` | `VK_DESCRIPTOR_TYPE_SAMPLER` (`0`) |
| **Set 1** | Fragment (`PS`) | `[[buffer(0..30)]]` | `binding = slot` (`0..30`) | `8` | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` (`6`) or `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` (`7`) |
| **Set 1** | Fragment (`PS`) | `[[texture(0..31)]]` | `binding = 32 + slot` (`32..63`) | `15` | `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` (`2`) |
| **Set 1** | Fragment (`PS`) | `[[sampler(0..15)]]` | `binding = 64 + slot` (`64..79`) | `15` | `VK_DESCRIPTOR_TYPE_SAMPLER` (`0`) |

- **Per-Stage Limits Compliance**: Across all `4,544` shaders, the maximum active UBOs in a single stage is `8` (`<= 12` Vulkan 1.1 guaranteed minimum `maxPerStageDescriptorUniformBuffers`), maximum textures is `16` (`<= 16` `maxPerStageDescriptorSampledImages`), and maximum samplers is `16` (`<= 16` `maxPerStageDescriptorSamplers`).
- **Stage I/O Semantic Location Mapping (`0..14`)**:
  - `user(TEXCOORD0..8)` -> `Location 0..8`
  - `user(TEXCOORD10)` / `user(PARTICLE_POSITION)` -> `Location 9`
  - `user(TEXCOORD11)` / `user(PARTICLE_SUBUVS)` -> `Location 10`
  - `user(COLOR0)` / `user(FRINGE_COORDS)` -> `Location 11`
  - `user(COLOR1)` / `user(ORIGINAL_POSITION)` -> `Location 12`
  - `user(TANGENTX)` -> `Location 13`, `user(TANGENTZ)` -> `Location 14`

---

## 4. `SPVC` v1 `spirv_cache.bin` Binary Layout & C/C++ Loader (`src/metal_vk/spirv_cache.{h,cpp}`)
Generated at `build/shaders/spirv_cache.bin` (`69,057,232` bytes total):

| Section | Offset | Size (Bytes) | Alignment | Record Layout |
|---|---:|---:|---:|---|
| `SpirvCacheHeader` | `0` | `80` | `8` B | `magic = 0x43565053 ('SPVC')`, `version = 1`, `flags = 1`, `entry_count = 4,482`, section offsets, `total_file_size = 69,057,232`, `total_translated = 4,544`, `total_spirv_val_passed = 4,544` |
| `SpirvCacheDiskEntry[4482]` | `80` | `501,984` | `8` B | `4,482` × `112` B sorted ascending by `uint64_t shader_id = le64(SHA-256(mtlb_bytes)[0:8])` for `O(log N)` binary search (`pak_sha1[20]`, `mtlb_sha256[32]`, `stage`, `spirv_offset`, `spirv_size_bytes`, masks, reflection slices, workgroup local size) |
| `SpirvBindingDesc[52989]` | `502,064` | `635,868` | `4` B | `52,989` × `12` B (`set`, `binding`, `descriptor_type`, `metal_slot`, `metal_kind`, `image_dim`, `is_depth`, `flags`, `buffer_size_bytes`) |
| `SpirvVertexAttrDesc[7588]` | `1,137,932` | `60,704` | `4` B | `7,588` × `8` B (`location`, `component_type`, `components`, `flags`, `name_offset`) |
| `StringTable` | `1,198,636` | `103,824` | `4` B | NUL-terminated UTF-8 Metal function names (`Main_0000XXXX_YYYYYYYY`) and vertex attribute names |
| `SpirvWordBlob` | `1,302,460` | `67,754,772` | `4` B | Concatenated 4-byte aligned SPIR-V 1.3 binary modules (`16,938,693` words) |

Runtime C/C++ interface (`src/metal_vk/spirv_cache.h`, `src/metal_vk/spirv_cache.cpp`, verified with Android NDK r28c `aarch64-linux-android29-clang++ -Wall -Wextra -Werror`):
- `bool spirv_cache_load(const char* cache_path);` (zero-copy `mmap` on Android/POSIX with `fread` fallback)
- `const SpirvShaderEntry* spirv_cache_find(uint64_t shader_hash_or_id);` (`O(log N)` binary search)
- `uint64_t spirv_cache_hash_mtlb(const void* mtlb_data, size_t size);` (standalone FIPS 180-4 SHA-256 prefix hasher)
- `void spirv_cache_unload(void);`

---

## 5. `spirv-val` Verification Results (`scripts/gate_m4.ps1`)
Validated against `C:\VulkanSDK\1.4.350.0\Bin\spirv-val.exe --target-env vulkan1.1` / `SPIRV-Tools v2026.2`:

| Metric | Global Archive | OH2 Archive | Combined Total | Unique `MTLB` Modules |
|---|---:|---:|---:|---:|
| **Total Extracted** | `317` | `4,227` | **`4,544`** | **`4,482`** |
| **Translated to SPIR-V 1.3** | `317` | `4,227` | **`4,544`** | **`4,482`** |
| **`spirv-val` (`vulkan1.1`) Passed** | `317` | `4,227` | **`4,544`** | **`4,482`** |
| **Pass Rate** | **`100.00%`** | **`100.00%`** | **`100.00%`** (`>= 90.0%` gate) | **`100.00%`** |
| **Stage Breakdown (Passed / Total)** | `255/255` PS, `53/53` VS, `9/9` CS | `2,743/2,743` PS, `1,484/1,484` VS | `2,998/2,998` PS, `1,537/1,537` VS, `9/9` CS | `2,958/2,958` PS, `1,515/1,515` VS, `9/9` CS |

Repeatable gate execution:
```powershell
powershell -ExecutionPolicy Bypass -File scripts/gate_m4.ps1
```
Output: `M4_STATS: total=4544 unique_mtlb=4482 translated=4544 spirv_val_passed=4544 unique_passed=4482 pass_rate=100.00% cache_bytes=69057232` -> `GATE M4: PASS` (exit `0`).
