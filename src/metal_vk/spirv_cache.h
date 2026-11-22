#ifndef OH2_METAL_VK_SPIRV_CACHE_H_
#define OH2_METAL_VK_SPIRV_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPIRV_CACHE_MAGIC   0x43565053u /* 'SPVC' little-endian */
#define SPIRV_CACHE_VERSION 1u

/* Shader stages stored in SpirvCacheDiskEntry::stage */
enum SpirvShaderStage {
    SPIRV_STAGE_VERTEX   = 0,
    SPIRV_STAGE_FRAGMENT = 1,
    SPIRV_STAGE_COMPUTE  = 2,
};

/* Header flags (SpirvCacheHeader::flags) */
enum SpirvCacheHeaderFlags {
    SPIRV_CACHE_FLAG_ALL_VALIDATED = (1u << 0),
};

/* Entry flags (SpirvCacheDiskEntry::flags) */
enum SpirvShaderEntryFlags {
    SPIRV_ENTRY_FLAG_VALIDATED         = (1u << 0),
    SPIRV_ENTRY_FLAG_USES_WORKGROUP    = (1u << 1),
    SPIRV_ENTRY_FLAG_USES_KILL         = (1u << 2),
    SPIRV_ENTRY_FLAG_RELAXED_PRECISION = (1u << 3),
    SPIRV_ENTRY_FLAG_FRAG_DEPTH        = (1u << 4),
    SPIRV_ENTRY_FLAG_FB_FETCH          = (1u << 5),
};

/* Metal resource kind (SpirvBindingDesc::metal_kind) */
enum SpirvMetalResourceKind {
    SPIRV_METAL_KIND_BUFFER  = 0,
    SPIRV_METAL_KIND_TEXTURE = 1,
    SPIRV_METAL_KIND_SAMPLER = 2,
};

/* Vulkan VkDescriptorType subset stored in SpirvBindingDesc::descriptor_type */
enum SpirvVkDescriptorType {
    SPIRV_VK_DESC_SAMPLER        = 0, /* VK_DESCRIPTOR_TYPE_SAMPLER */
    SPIRV_VK_DESC_SAMPLED_IMAGE  = 2, /* VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE */
    SPIRV_VK_DESC_STORAGE_IMAGE  = 3, /* VK_DESCRIPTOR_TYPE_STORAGE_IMAGE */
    SPIRV_VK_DESC_UNIFORM_BUFFER = 6, /* VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER */
    SPIRV_VK_DESC_STORAGE_BUFFER = 7, /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */
};

/* Image dimensionality stored in SpirvBindingDesc::image_dim */
enum SpirvImageDim {
    SPIRV_IMAGE_DIM_NONE     = 0,
    SPIRV_IMAGE_DIM_1D       = 1,
    SPIRV_IMAGE_DIM_2D       = 2,
    SPIRV_IMAGE_DIM_3D       = 3,
    SPIRV_IMAGE_DIM_CUBE     = 4,
    SPIRV_IMAGE_DIM_2D_ARRAY = 5,
    SPIRV_IMAGE_DIM_2D_MS    = 6,
};

/* Vertex attribute component type (SpirvVertexAttrDesc::component_type) */
enum SpirvComponentType {
    SPIRV_COMP_FLOAT = 0,
    SPIRV_COMP_HALF  = 1,
    SPIRV_COMP_INT   = 2,
    SPIRV_COMP_UINT  = 3,
};

/* 80-byte binary file header at offset 0 of spirv_cache.bin */
typedef struct SpirvCacheHeader {
    uint32_t magic;                  /* 0x43565053 ('SPVC') */
    uint32_t version;                /* 1 */
    uint32_t flags;                  /* SpirvCacheHeaderFlags */
    uint32_t entry_count;            /* Number of SpirvCacheDiskEntry records (sorted by shader_id) */
    uint64_t entries_offset;         /* Byte offset to SpirvCacheDiskEntry[entry_count] (8-byte aligned) */
    uint64_t bindings_offset;        /* Byte offset to SpirvBindingDesc[] (4-byte aligned) */
    uint64_t vertex_attrs_offset;    /* Byte offset to SpirvVertexAttrDesc[] (4-byte aligned) */
    uint64_t strings_offset;         /* Byte offset to NUL-terminated UTF-8 string table */
    uint64_t spirv_blob_offset;      /* Byte offset to 4-byte aligned SPIR-V words */
    uint64_t total_file_size;        /* Exact file size in bytes */
    uint32_t total_translated;       /* Count of translated shaders */
    uint32_t total_spirv_val_passed; /* Count of shaders passing spirv-val --target-env vulkan1.1 */
    uint8_t  reserved[8];            /* Reserved (zero) */
} SpirvCacheHeader;

/* 112-byte on-disk shader record sorted ascending by shader_id */
typedef struct SpirvCacheDiskEntry {
    uint64_t shader_id;              /* le64(SHA-256(mtlb_bytes)[0:8]) */
    uint8_t  pak_sha1[20];           /* Original 20-byte UE4 shader archive SHA-1 */
    uint8_t  mtlb_sha256[32];        /* Full 32-byte SHA-256 of MTLB container */
    uint32_t stage;                  /* SpirvShaderStage (0=VS, 1=PS, 2=CS) */
    uint32_t spirv_offset;           /* Byte offset from start of file to SPIR-V words */
    uint32_t spirv_size_bytes;       /* Size of SPIR-V module in bytes (multiple of 4) */
    uint32_t binding_first;          /* First index in SpirvBindingDesc table */
    uint32_t vertex_attr_first;      /* First index in SpirvVertexAttrDesc table */
    uint32_t entry_name_offset;      /* Byte offset in string table to Metal function name */
    uint32_t buffer_mask;            /* Bitmask of active Metal buffer(0..30) slots */
    uint32_t texture_mask;           /* Bitmask of active Metal texture(0..31) slots */
    uint16_t sampler_mask;           /* Bitmask of active Metal sampler(0..15) slots */
    uint16_t binding_count;          /* Number of SpirvBindingDesc records */
    uint16_t vertex_attr_count;      /* Number of SpirvVertexAttrDesc records */
    uint16_t fragment_output_mask;   /* Bitmask of active fragment color attachment locations */
    uint16_t flags;                  /* SpirvShaderEntryFlags */
    uint16_t local_size_x;           /* Compute workgroup size X (0 for VS/PS) */
    uint16_t local_size_y;           /* Compute workgroup size Y (0 for VS/PS) */
    uint16_t local_size_z;           /* Compute workgroup size Z (0 for VS/PS) */
    uint32_t reserved;               /* Reserved (zero) */
} SpirvCacheDiskEntry;

/* 12-byte descriptor binding reflection record */
typedef struct SpirvBindingDesc {
    uint8_t  set;                    /* Vulkan descriptor set: 0 = VS/CS, 1 = PS */
    uint8_t  binding;                /* Vulkan binding: slot (buf), 32+slot (tex), 64+slot (samp) */
    uint8_t  descriptor_type;        /* SpirvVkDescriptorType (VkDescriptorType) */
    uint8_t  metal_slot;             /* Original Metal [[buffer(N)]] / [[texture(N)]] / [[sampler(N)]] */
    uint8_t  metal_kind;             /* SpirvMetalResourceKind */
    uint8_t  image_dim;              /* SpirvImageDim */
    uint8_t  is_depth;               /* 1 if depth texture, 0 otherwise */
    uint8_t  flags;                  /* bit0 = read, bit1 = write */
    uint32_t buffer_size_bytes;      /* Minimum buffer size in bytes from AIR metadata (0 if unsized) */
} SpirvBindingDesc;

/* 8-byte vertex input attribute reflection record */
typedef struct SpirvVertexAttrDesc {
    uint8_t  location;               /* Vulkan vertex attribute location (0..31) */
    uint8_t  component_type;         /* SpirvComponentType */
    uint8_t  components;             /* 1..4 */
    uint8_t  flags;                  /* Reserved flags */
    uint32_t name_offset;            /* Byte offset in string table to attribute name */
} SpirvVertexAttrDesc;

/* Runtime view returned by spirv_cache_find() for M5 Metal-to-Vulkan pipeline creation */
typedef struct SpirvShaderEntry {
    uint64_t                   shader_id;
    uint32_t                   stage;
    uint32_t                   spirv_word_count;
    uint32_t                   spirv_size_bytes;
    const uint32_t*            spirv_words;
    const char*                entry_name;        /* Metal function name, e.g. "Main_000022b0_2e2dc863" */
    const char*                spirv_entry_point; /* SPIR-V OpEntryPoint name ("main") */
    const SpirvBindingDesc*    bindings;
    uint16_t                   binding_count;
    const SpirvVertexAttrDesc* vertex_attrs;
    uint16_t                   vertex_attr_count;
    uint32_t                   buffer_mask;
    uint32_t                   texture_mask;
    uint16_t                   sampler_mask;
    uint16_t                   fragment_output_mask;
    uint16_t                   flags;
    uint16_t                   local_size_x;
    uint16_t                   local_size_y;
    uint16_t                   local_size_z;
    const SpirvCacheDiskEntry* disk_entry;
} SpirvShaderEntry;

#ifdef __cplusplus
static_assert(sizeof(SpirvCacheHeader) == 80, "SpirvCacheHeader must be 80 bytes");
static_assert(sizeof(SpirvCacheDiskEntry) == 112, "SpirvCacheDiskEntry must be 112 bytes");
static_assert(sizeof(SpirvBindingDesc) == 12, "SpirvBindingDesc must be 12 bytes");
static_assert(sizeof(SpirvVertexAttrDesc) == 8, "SpirvVertexAttrDesc must be 8 bytes");
#endif

/* Primary M4 <-> M5 interface contract */
bool spirv_cache_load(const char* cache_path);
const SpirvShaderEntry* spirv_cache_find(uint64_t shader_hash_or_id);
uint64_t spirv_cache_hash_mtlb(const void* mtlb_data, size_t size);
void spirv_cache_unload(void);

/* Additional lookup & inspection helpers for M5 */
const SpirvShaderEntry* spirv_cache_find_by_mtlb(const void* mtlb_data, size_t size);
const SpirvShaderEntry* spirv_cache_find_by_pak_sha1(const uint8_t pak_sha1[20]);
const SpirvShaderEntry* spirv_cache_find_by_entry_name(const char* entry_name);
const SpirvShaderEntry* spirv_cache_entry_at(uint32_t index);
uint32_t spirv_cache_count(void);
const SpirvCacheHeader* spirv_cache_header(void);
const char* spirv_cache_string(uint32_t string_offset);

#ifdef __cplusplus
}
#endif

#endif /* OH2_METAL_VK_SPIRV_CACHE_H_ */
