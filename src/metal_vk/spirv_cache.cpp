#include "spirv_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

/* Standalone SHA-256 implementation (FIPS 180-4) for spirv_cache_hash_mtlb() */
struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
};

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(Sha256Ctx* ctx, const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(data[i * 4 + 0]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(data[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_compute(const void* data, size_t size, uint8_t digest[32]) {
    Sha256Ctx ctx;
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
    ctx.bitcount = 0;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t idx = 0;
    while (size >= 64) {
        sha256_transform(&ctx, bytes + idx);
        ctx.bitcount += 512;
        idx += 64;
        size -= 64;
    }
    if (size > 0) {
        std::memcpy(ctx.buffer, bytes + idx, size);
        ctx.bitcount += static_cast<uint64_t>(size) * 8u;
    }

    ctx.buffer[size++] = 0x80u;
    if (size > 56) {
        std::memset(ctx.buffer + size, 0, 64 - size);
        sha256_transform(&ctx, ctx.buffer);
        size = 0;
    }
    std::memset(ctx.buffer + size, 0, 56 - size);
    for (int i = 0; i < 8; ++i) {
        ctx.buffer[63 - i] = static_cast<uint8_t>((ctx.bitcount >> (i * 8)) & 0xffu);
    }
    sha256_transform(&ctx, ctx.buffer);

    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>((ctx.state[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<uint8_t>((ctx.state[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<uint8_t>((ctx.state[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i] & 0xffu);
    }
}

struct LoadedSpirvCache {
    const uint8_t*                base = nullptr;
    size_t                        size = 0;
    bool                          is_mmapped = false;
    std::vector<uint8_t>          heap_buf;
    const SpirvCacheHeader*       header = nullptr;
    const SpirvCacheDiskEntry*    disk_entries = nullptr;
    const SpirvBindingDesc*       bindings = nullptr;
    const SpirvVertexAttrDesc*    vertex_attrs = nullptr;
    const char*                   strings = nullptr;
    size_t                        strings_size = 0;
    std::vector<SpirvShaderEntry> runtime_entries;
};

static LoadedSpirvCache g_cache;

} // namespace

extern "C" {

uint64_t spirv_cache_hash_mtlb(const void* mtlb_data, size_t size) {
    if (!mtlb_data || size == 0) {
        return 0;
    }
    /* If caller passed full UE4 entry (FMetalCodeHeader + MTLB), advance to 'MTLB' magic */
    const uint8_t* ptr = static_cast<const uint8_t*>(mtlb_data);
    size_t len = size;
    if (len >= 4 && !(ptr[0] == 'M' && ptr[1] == 'T' && ptr[2] == 'L' && ptr[3] == 'B')) {
        for (size_t i = 0; i + 4 <= len; ++i) {
            if (ptr[i] == 'M' && ptr[i + 1] == 'T' && ptr[i + 2] == 'L' && ptr[i + 3] == 'B') {
                ptr += i;
                len -= i;
                break;
            }
        }
    }
    uint8_t digest[32];
    sha256_compute(ptr, len, digest);
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) {
        id |= static_cast<uint64_t>(digest[i]) << (i * 8);
    }
    return id;
}

void spirv_cache_unload(void) {
#if !defined(_WIN32)
    if (g_cache.is_mmapped && g_cache.base && g_cache.size > 0) {
        munmap(const_cast<uint8_t*>(g_cache.base), g_cache.size);
    }
#endif
    g_cache.base = nullptr;
    g_cache.size = 0;
    g_cache.is_mmapped = false;
    g_cache.heap_buf.clear();
    g_cache.heap_buf.shrink_to_fit();
    g_cache.header = nullptr;
    g_cache.disk_entries = nullptr;
    g_cache.bindings = nullptr;
    g_cache.vertex_attrs = nullptr;
    g_cache.strings = nullptr;
    g_cache.strings_size = 0;
    g_cache.runtime_entries.clear();
    g_cache.runtime_entries.shrink_to_fit();
}

bool spirv_cache_load(const char* cache_path) {
    spirv_cache_unload();
    if (!cache_path || !cache_path[0]) {
        return false;
    }

#if !defined(_WIN32)
    int fd = open(cache_path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size >= static_cast<off_t>(sizeof(SpirvCacheHeader))) {
            void* mapped = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapped != MAP_FAILED) {
                g_cache.base = static_cast<const uint8_t*>(mapped);
                g_cache.size = static_cast<size_t>(st.st_size);
                g_cache.is_mmapped = true;
            }
        }
        close(fd);
    }
#endif

    if (!g_cache.base) {
        std::FILE* fp = std::fopen(cache_path, "rb");
        if (!fp) {
            return false;
        }
        if (std::fseek(fp, 0, SEEK_END) != 0) {
            std::fclose(fp);
            return false;
        }
        long fsz = std::ftell(fp);
        if (fsz < static_cast<long>(sizeof(SpirvCacheHeader)) || std::fseek(fp, 0, SEEK_SET) != 0) {
            std::fclose(fp);
            return false;
        }
        g_cache.heap_buf.resize(static_cast<size_t>(fsz));
        size_t nread = std::fread(g_cache.heap_buf.data(), 1, g_cache.heap_buf.size(), fp);
        std::fclose(fp);
        if (nread != g_cache.heap_buf.size()) {
            spirv_cache_unload();
            return false;
        }
        g_cache.base = g_cache.heap_buf.data();
        g_cache.size = g_cache.heap_buf.size();
        g_cache.is_mmapped = false;
    }

    const auto* hdr = reinterpret_cast<const SpirvCacheHeader*>(g_cache.base);
    if (hdr->magic != SPIRV_CACHE_MAGIC ||
        hdr->version != SPIRV_CACHE_VERSION ||
        hdr->total_file_size != g_cache.size ||
        hdr->entry_count == 0) {
        spirv_cache_unload();
        return false;
    }

    uint64_t entries_bytes = static_cast<uint64_t>(hdr->entry_count) * sizeof(SpirvCacheDiskEntry);
    if ((hdr->entries_offset & 7u) != 0 ||
        hdr->entries_offset + entries_bytes > g_cache.size ||
        (hdr->bindings_offset & 3u) != 0 ||
        hdr->bindings_offset > g_cache.size ||
        (hdr->vertex_attrs_offset & 3u) != 0 ||
        hdr->vertex_attrs_offset > g_cache.size ||
        hdr->strings_offset > g_cache.size ||
        (hdr->spirv_blob_offset & 3u) != 0 ||
        hdr->spirv_blob_offset > g_cache.size ||
        hdr->strings_offset > hdr->spirv_blob_offset) {
        spirv_cache_unload();
        return false;
    }

    g_cache.header = hdr;
    g_cache.disk_entries = reinterpret_cast<const SpirvCacheDiskEntry*>(g_cache.base + hdr->entries_offset);
    g_cache.bindings = reinterpret_cast<const SpirvBindingDesc*>(g_cache.base + hdr->bindings_offset);
    g_cache.vertex_attrs = reinterpret_cast<const SpirvVertexAttrDesc*>(g_cache.base + hdr->vertex_attrs_offset);
    g_cache.strings = reinterpret_cast<const char*>(g_cache.base + hdr->strings_offset);
    g_cache.strings_size = static_cast<size_t>(hdr->spirv_blob_offset - hdr->strings_offset);

    g_cache.runtime_entries.resize(hdr->entry_count);
    for (uint32_t i = 0; i < hdr->entry_count; ++i) {
        const SpirvCacheDiskEntry& de = g_cache.disk_entries[i];
        if (i > 0 && de.shader_id < g_cache.disk_entries[i - 1].shader_id) {
            /* Table must be sorted ascending by shader_id */
            spirv_cache_unload();
            return false;
        }
        if ((de.spirv_offset & 3u) != 0 ||
            (de.spirv_size_bytes & 3u) != 0 ||
            static_cast<uint64_t>(de.spirv_offset) + de.spirv_size_bytes > g_cache.size ||
            de.entry_name_offset >= g_cache.strings_size) {
            spirv_cache_unload();
            return false;
        }

        SpirvShaderEntry& re = g_cache.runtime_entries[i];
        re.shader_id = de.shader_id;
        re.stage = de.stage;
        re.spirv_size_bytes = de.spirv_size_bytes;
        re.spirv_word_count = de.spirv_size_bytes / 4u;
        re.spirv_words = (de.spirv_size_bytes > 0)
                             ? reinterpret_cast<const uint32_t*>(g_cache.base + de.spirv_offset)
                             : nullptr;
        re.entry_name = g_cache.strings + de.entry_name_offset;
        re.spirv_entry_point = "main";
        re.bindings = (de.binding_count > 0) ? (g_cache.bindings + de.binding_first) : nullptr;
        re.binding_count = de.binding_count;
        re.vertex_attrs = (de.vertex_attr_count > 0) ? (g_cache.vertex_attrs + de.vertex_attr_first) : nullptr;
        re.vertex_attr_count = de.vertex_attr_count;
        re.buffer_mask = de.buffer_mask;
        re.texture_mask = de.texture_mask;
        re.sampler_mask = de.sampler_mask;
        re.fragment_output_mask = de.fragment_output_mask;
        re.flags = de.flags;
        re.local_size_x = de.local_size_x;
        re.local_size_y = de.local_size_y;
        re.local_size_z = de.local_size_z;
        re.disk_entry = &de;
    }

    return true;
}

const SpirvShaderEntry* spirv_cache_find(uint64_t shader_hash_or_id) {
    if (g_cache.runtime_entries.empty()) {
        return nullptr;
    }
    auto it = std::lower_bound(
        g_cache.runtime_entries.begin(),
        g_cache.runtime_entries.end(),
        shader_hash_or_id,
        [](const SpirvShaderEntry& entry, uint64_t id) {
            return entry.shader_id < id;
        });
    if (it != g_cache.runtime_entries.end() && it->shader_id == shader_hash_or_id) {
        return &(*it);
    }
    return nullptr;
}

const SpirvShaderEntry* spirv_cache_find_by_mtlb(const void* mtlb_data, size_t size) {
    uint64_t id = spirv_cache_hash_mtlb(mtlb_data, size);
    return spirv_cache_find(id);
}

const SpirvShaderEntry* spirv_cache_find_by_pak_sha1(const uint8_t pak_sha1[20]) {
    if (!pak_sha1 || g_cache.runtime_entries.empty()) {
        return nullptr;
    }
    for (const auto& entry : g_cache.runtime_entries) {
        if (entry.disk_entry && std::memcmp(entry.disk_entry->pak_sha1, pak_sha1, 20) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

const SpirvShaderEntry* spirv_cache_find_by_entry_name(const char* entry_name) {
    if (!entry_name || !entry_name[0] || g_cache.runtime_entries.empty()) {
        return nullptr;
    }
    for (const auto& entry : g_cache.runtime_entries) {
        if (entry.entry_name && std::strcmp(entry.entry_name, entry_name) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

uint32_t spirv_cache_count(void) {
    return static_cast<uint32_t>(g_cache.runtime_entries.size());
}

const SpirvCacheHeader* spirv_cache_header(void) {
    return g_cache.header;
}

const char* spirv_cache_string(uint32_t string_offset) {
    if (!g_cache.strings || string_offset >= g_cache.strings_size) {
        return "";
    }
    return g_cache.strings + string_offset;
}

} // extern "C"
