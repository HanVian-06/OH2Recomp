#include "air2spirv_api.h"
#include "air_to_spirv.h"
#include "sha256.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

static constexpr uint32_t SPIRV_CACHE_MAGIC = 0x43565053u; // 'SPVC'
static constexpr uint32_t SPIRV_CACHE_VERSION = 1u;

#pragma pack(push, 1)
struct DiskHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t entry_count;
    uint64_t entries_offset;
    uint64_t bindings_offset;
    uint64_t vertex_attrs_offset;
    uint64_t strings_offset;
    uint64_t spirv_blob_offset;
    uint64_t total_file_size;
    uint32_t total_translated;
    uint32_t total_spirv_val_passed;
    uint8_t reserved[8];
};

struct DiskEntry {
    uint64_t shader_id;
    uint8_t pak_sha1[20];
    uint8_t mtlb_sha256[32];
    uint32_t stage;
    uint32_t spirv_offset;
    uint32_t spirv_size_bytes;
    uint32_t binding_first;
    uint32_t vertex_attr_first;
    uint32_t entry_name_offset;
    uint32_t buffer_mask;
    uint32_t texture_mask;
    uint16_t sampler_mask;
    uint16_t binding_count;
    uint16_t vertex_attr_count;
    uint16_t fragment_output_mask;
    uint16_t flags;
    uint16_t local_size_x;
    uint16_t local_size_y;
    uint16_t local_size_z;
    uint32_t reserved;
};

struct DiskBinding {
    uint8_t set;
    uint8_t binding;
    uint8_t descriptor_type;
    uint8_t metal_slot;
    uint8_t metal_kind;
    uint8_t image_dim;
    uint8_t is_depth;
    uint8_t flags;
    uint32_t buffer_size_bytes;
};

struct DiskVertexAttr {
    uint8_t location;
    uint8_t component_type;
    uint8_t components;
    uint8_t flags;
    uint32_t name_offset;
};
#pragma pack(pop)

static_assert(sizeof(DiskHeader) == 80, "DiskHeader must be 80 bytes");
static_assert(sizeof(DiskEntry) == 112, "DiskEntry must be 112 bytes");
static_assert(sizeof(DiskBinding) == 12, "DiskBinding must be 12 bytes");
static_assert(sizeof(DiskVertexAttr) == 8, "DiskVertexAttr must be 8 bytes");

static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
static inline int32_t rd_i32(const uint8_t *p) {
    int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
static inline uint64_t rd_u64(const uint8_t *p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

static bool read_fstring(const uint8_t *buf, size_t len, size_t &pos, std::string &out) {
    if (pos + 4 > len) return false;
    int32_t slen = rd_i32(buf + pos);
    pos += 4;
    if (slen > 0) {
        size_t n = static_cast<size_t>(slen);
        if (pos + n > len) return false;
        out.assign(reinterpret_cast<const char *>(buf + pos), n > 0 ? n - 1 : 0);
        pos += n;
        return true;
    } else if (slen < 0) {
        size_t chars = static_cast<size_t>(-slen);
        size_t nbytes = chars * 2;
        if (pos + nbytes > len) return false;
        out.clear();
        out.reserve(chars);
        for (size_t i = 0; i + 1 < chars; ++i) {
            uint16_t ch = static_cast<uint16_t>(buf[pos + i * 2]) |
                          (static_cast<uint16_t>(buf[pos + i * 2 + 1]) << 8);
            out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        pos += nbytes;
        return true;
    }
    out.clear();
    return true;
}

static bool zlib_inflate_to(const uint8_t *src, size_t src_len, std::vector<uint8_t> &dst,
                            size_t expected_out_hint) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(src));
    zs.avail_in = static_cast<uInt>(src_len);

    size_t start_sz = dst.size();
    size_t chunk = expected_out_hint > 0 ? expected_out_hint : std::max<size_t>(src_len * 2, 16384);
    int ret = Z_OK;
    while (ret == Z_OK) {
        size_t cur = dst.size();
        dst.resize(cur + chunk);
        zs.next_out = reinterpret_cast<Bytef *>(dst.data() + cur);
        zs.avail_out = static_cast<uInt>(chunk);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            dst.resize(start_sz);
            return false;
        }
        size_t produced = chunk - zs.avail_out;
        dst.resize(cur + produced);
        if (ret == Z_STREAM_END) break;
        chunk = 65536;
    }
    inflateEnd(&zs);
    return ret == Z_STREAM_END;
}

static bool extract_pak_files(const char *pak_path,
                              const std::unordered_set<std::string> &targets,
                              std::unordered_map<std::string, std::vector<uint8_t>> &out,
                              std::string &err) {
    std::ifstream f(pak_path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        err = std::string("Cannot open PAK file: ") + pak_path;
        return false;
    }
    std::streamoff file_size = f.tellg();
    if (file_size < 221) {
        err = "PAK file too small (< 221 bytes)";
        return false;
    }

    uint8_t footer[221];
    f.seekg(file_size - 221, std::ios::beg);
    f.read(reinterpret_cast<char *>(footer), 221);
    if (!f) {
        err = "Failed to read PAK footer";
        return false;
    }

    uint8_t b_enc_idx = footer[16];
    uint32_t magic = rd_u32(footer + 17);
    uint32_t version = rd_u32(footer + 21);
    uint64_t index_off = rd_u64(footer + 25);
    uint64_t index_size = rd_u64(footer + 33);

    if (magic != 0x5A6F12E1u || version != 8u || b_enc_idx != 0u) {
        err = "Unsupported PAK header (expected UE4 PAK v8 unencrypted)";
        return false;
    }
    if (index_off + index_size > static_cast<uint64_t>(file_size)) {
        err = "PAK index out of bounds";
        return false;
    }

    std::vector<uint8_t> idx(static_cast<size_t>(index_size));
    f.seekg(static_cast<std::streamoff>(index_off), std::ios::beg);
    f.read(reinterpret_cast<char *>(idx.data()), static_cast<std::streamsize>(index_size));
    if (!f) {
        err = "Failed to read PAK index";
        return false;
    }

    size_t pos = 0;
    std::string mount_point;
    if (!read_fstring(idx.data(), idx.size(), pos, mount_point)) {
        err = "Failed to parse PAK mount point";
        return false;
    }
    if (pos + 4 > idx.size()) {
        err = "Truncated PAK entry count";
        return false;
    }
    uint32_t num_entries = rd_u32(idx.data() + pos);
    pos += 4;

    std::vector<std::pair<uint64_t, uint64_t>> blocks;
    std::vector<uint8_t> comp_buf;

    for (uint32_t i = 0; i < num_entries; ++i) {
        std::string fname;
        if (!read_fstring(idx.data(), idx.size(), pos, fname)) {
            err = "Truncated PAK entry filename";
            return false;
        }
        if (pos + 48 > idx.size()) {
            err = "Truncated PAK entry metadata";
            return false;
        }
        uint64_t offset = rd_u64(idx.data() + pos);
        uint64_t uncomp_size = rd_u64(idx.data() + pos + 16);
        uint32_t comp_method = rd_u32(idx.data() + pos + 24);
        pos += 48; // offset(8) + size(8) + uncomp(8) + method(4) + sha1(20)

        blocks.clear();
        if (comp_method != 0) {
            if (pos + 4 > idx.size()) {
                err = "Truncated PAK block count";
                return false;
            }
            uint32_t bcount = rd_u32(idx.data() + pos);
            pos += 4;
            if (pos + static_cast<size_t>(bcount) * 16 > idx.size()) {
                err = "Truncated PAK block table";
                return false;
            }
            blocks.reserve(bcount);
            for (uint32_t b = 0; b < bcount; ++b) {
                uint64_t bstart = rd_u64(idx.data() + pos);
                uint64_t bend = rd_u64(idx.data() + pos + 8);
                pos += 16;
                blocks.emplace_back(bstart, bend);
            }
        }
        if (pos + 5 > idx.size()) {
            err = "Truncated PAK entry trailer";
            return false;
        }
        pos += 5; // flags(1) + comp_block_size(4)

        if (targets.find(fname) != targets.end()) {
            std::vector<uint8_t> data;
            if (comp_method == 0) {
                data.resize(static_cast<size_t>(uncomp_size));
                f.seekg(static_cast<std::streamoff>(offset + 53), std::ios::beg);
                f.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(uncomp_size));
                if (!f) {
                    err = "Failed reading uncompressed PAK entry: " + fname;
                    return false;
                }
            } else if (comp_method == 1) {
                data.reserve(static_cast<size_t>(uncomp_size));
                for (const auto &blk : blocks) {
                    size_t blen = static_cast<size_t>(blk.second - blk.first);
                    comp_buf.resize(blen);
                    f.seekg(static_cast<std::streamoff>(offset + blk.first), std::ios::beg);
                    f.read(reinterpret_cast<char *>(comp_buf.data()), static_cast<std::streamsize>(blen));
                    if (!f) {
                        err = "Failed reading compressed PAK block: " + fname;
                        return false;
                    }
                    if (!zlib_inflate_to(comp_buf.data(), blen, data, 65536)) {
                        err = "Zlib inflate failed in PAK entry: " + fname;
                        return false;
                    }
                }
                if (data.size() > static_cast<size_t>(uncomp_size)) {
                    data.resize(static_cast<size_t>(uncomp_size));
                }
            } else {
                err = "Unsupported PAK compression method in: " + fname;
                return false;
            }
            out[fname] = std::move(data);
            if (out.size() == targets.size()) break;
        }
    }

    for (const auto &t : targets) {
        if (out.find(t) == out.end()) {
            err = "Missing required shader archive in PAK: " + t;
            return false;
        }
    }
    return true;
}

struct ArchiveEntry {
    uint64_t shader_id = 0;
    std::array<uint8_t, 20> pak_sha1{};
    uint32_t freq = 0;
    std::vector<uint8_t> raw;
};

static bool parse_ushaderbytecode(const std::vector<uint8_t> &blob,
                                  const char *label,
                                  std::vector<ArchiveEntry> &out,
                                  std::string &err) {
    if (blob.size() < 8) {
        err = std::string("Truncated ushaderbytecode: ") + label;
        return false;
    }
    uint32_t ver = rd_u32(blob.data());
    uint32_t num_shaders = rd_u32(blob.data() + 4);
    if (ver != 1) {
        err = std::string("Unexpected ushaderbytecode version in ") + label;
        return false;
    }
    size_t data_base = 8 + static_cast<size_t>(num_shaders) * 37;
    if (data_base > blob.size()) {
        err = std::string("Truncated ushaderbytecode index in ") + label;
        return false;
    }

    out.reserve(out.size() + num_shaders);
    size_t pos = 8;
    for (uint32_t i = 0; i < num_shaders; ++i) {
        ArchiveEntry e;
        std::memcpy(e.pak_sha1.data(), blob.data() + pos, 20);
        uint64_t off = rd_u64(blob.data() + pos + 20);
        uint32_t comp_sz = rd_u32(blob.data() + pos + 28);
        uint32_t uncomp_sz = rd_u32(blob.data() + pos + 32);
        e.freq = blob[pos + 36];
        pos += 37;

        size_t abs_off = data_base + static_cast<size_t>(off);
        if (abs_off + comp_sz > blob.size()) {
            err = std::string("Shader entry out of bounds in ") + label;
            return false;
        }

        if (comp_sz != uncomp_sz) {
            e.raw.reserve(uncomp_sz);
            if (!zlib_inflate_to(blob.data() + abs_off, comp_sz, e.raw, uncomp_sz)) {
                err = std::string("Zlib inflate failed for shader in ") + label;
                return false;
            }
        } else {
            e.raw.assign(blob.data() + abs_off, blob.data() + abs_off + comp_sz);
        }

        // Find "MTLB" signature
        size_t mtlb_idx = std::string::npos;
        for (size_t k = 0; k + 4 <= e.raw.size(); ++k) {
            if (e.raw[k] == 'M' && e.raw[k + 1] == 'T' && e.raw[k + 2] == 'L' && e.raw[k + 3] == 'B') {
                mtlb_idx = k;
                break;
            }
        }
        if (mtlb_idx == std::string::npos) {
            err = std::string("Missing MTLB magic in ") + label;
            return false;
        }

        auto sha = oh2::air2spirv::sha256_bytes(e.raw.data() + mtlb_idx, e.raw.size() - mtlb_idx);
        std::memcpy(&e.shader_id, sha.data(), 8);
        out.push_back(std::move(e));
    }
    return true;
}

struct CompiledWorkItem {
    uint64_t shader_id = 0;
    std::array<uint8_t, 20> pak_sha1{};
    uint32_t freq = 0;
    std::vector<uint8_t> raw;

    bool ok = false;
    std::vector<uint32_t> spirv_words;
    oh2::air2spirv::ShaderReflection refl;
    std::string err;
};

static bool write_spvc_cache(const std::vector<CompiledWorkItem *> &sorted_items,
                             uint32_t total_archive_shaders,
                             const char *out_path,
                             std::string &err) {
    std::vector<uint8_t> string_table;
    string_table.push_back(0);
    std::unordered_map<std::string, uint32_t> string_offsets;
    string_offsets[""] = 0;

    auto intern_str = [&](const std::string &s) -> uint32_t {
        auto it = string_offsets.find(s);
        if (it != string_offsets.end()) return it->second;
        uint32_t off = static_cast<uint32_t>(string_table.size());
        string_table.insert(string_table.end(), s.begin(), s.end());
        string_table.push_back(0);
        string_offsets[s] = off;
        return off;
    };

    std::vector<DiskBinding> bindings_table;
    std::vector<DiskVertexAttr> vattrs_table;
    std::vector<uint8_t> spirv_blob;
    bindings_table.reserve(sorted_items.size() * 6);
    vattrs_table.reserve(sorted_items.size() * 2);
    spirv_blob.reserve(sorted_items.size() * 4096);

    struct PrelimEntry {
        const CompiledWorkItem *item;
        uint32_t spv_rel_off;
        uint32_t spv_sz;
        uint32_t b_first;
        uint16_t b_cnt;
        uint32_t va_first;
        uint16_t va_cnt;
        uint32_t name_off;
    };
    std::vector<PrelimEntry> prelim;
    prelim.reserve(sorted_items.size());

    uint32_t unique_ok = 0;
    for (const CompiledWorkItem *item : sorted_items) {
        if (item->ok) ++unique_ok;

        uint32_t b_first = static_cast<uint32_t>(bindings_table.size());
        for (const auto &b : item->refl.bindings) {
            DiskBinding db{};
            db.set = b.set;
            db.binding = b.binding;
            db.descriptor_type = b.descriptor_type;
            db.metal_slot = b.metal_slot;
            db.metal_kind = b.metal_kind;
            db.image_dim = b.image_dim;
            db.is_depth = b.is_depth;
            db.flags = b.flags;
            db.buffer_size_bytes = b.buffer_size_bytes;
            bindings_table.push_back(db);
        }

        uint32_t va_first = static_cast<uint32_t>(vattrs_table.size());
        for (const auto &va : item->refl.vertex_attrs) {
            DiskVertexAttr dva{};
            dva.location = va.location;
            dva.component_type = va.component_type;
            dva.components = va.components;
            dva.flags = va.flags;
            dva.name_offset = intern_str(va.name);
            vattrs_table.push_back(dva);
        }

        uint32_t name_off = intern_str(item->refl.entry_name);
        uint32_t spv_rel_off = static_cast<uint32_t>(spirv_blob.size());
        uint32_t spv_sz = static_cast<uint32_t>(item->spirv_words.size() * sizeof(uint32_t));
        if (spv_sz > 0) {
            const uint8_t *p = reinterpret_cast<const uint8_t *>(item->spirv_words.data());
            spirv_blob.insert(spirv_blob.end(), p, p + spv_sz);
        }

        prelim.push_back({
            item,
            spv_rel_off,
            spv_sz,
            b_first,
            static_cast<uint16_t>(item->refl.bindings.size()),
            va_first,
            static_cast<uint16_t>(item->refl.vertex_attrs.size()),
            name_off,
        });
    }

    // Pad string_table to 4-byte alignment
    while ((string_table.size() & 3u) != 0) {
        string_table.push_back(0);
    }

    uint64_t entries_offset = sizeof(DiskHeader); // 80
    uint64_t entries_size = static_cast<uint64_t>(prelim.size()) * sizeof(DiskEntry);
    uint64_t bindings_offset = entries_offset + entries_size;
    uint64_t bindings_size = static_cast<uint64_t>(bindings_table.size()) * sizeof(DiskBinding);
    uint64_t vertex_attrs_offset = bindings_offset + bindings_size;
    uint64_t vertex_attrs_size = static_cast<uint64_t>(vattrs_table.size()) * sizeof(DiskVertexAttr);
    uint64_t strings_offset = vertex_attrs_offset + vertex_attrs_size;
    uint64_t spirv_blob_offset = strings_offset + string_table.size();
    uint64_t total_file_size = spirv_blob_offset + spirv_blob.size();

    std::vector<DiskEntry> entries_table;
    entries_table.reserve(prelim.size());
    for (const auto &pe : prelim) {
        DiskEntry de{};
        de.shader_id = pe.item->shader_id;
        std::memcpy(de.pak_sha1, pe.item->pak_sha1.data(), 20);
        std::memcpy(de.mtlb_sha256, pe.item->refl.mtlb_sha256.data(), 32);
        de.stage = pe.item->refl.stage;
        de.spirv_offset = static_cast<uint32_t>(spirv_blob_offset + pe.spv_rel_off);
        de.spirv_size_bytes = pe.spv_sz;
        de.binding_first = pe.b_first;
        de.vertex_attr_first = pe.va_first;
        de.entry_name_offset = pe.name_off;
        de.buffer_mask = pe.item->refl.buffer_mask;
        de.texture_mask = pe.item->refl.texture_mask;
        de.sampler_mask = pe.item->refl.sampler_mask;
        de.binding_count = pe.b_cnt;
        de.vertex_attr_count = pe.va_cnt;
        de.fragment_output_mask = pe.item->refl.fragment_output_mask;
        de.flags = pe.item->refl.flags | (pe.item->ok ? 1u : 0u); // SPIRV_ENTRY_FLAG_VALIDATED
        de.local_size_x = pe.item->refl.local_size_x;
        de.local_size_y = pe.item->refl.local_size_y;
        de.local_size_z = pe.item->refl.local_size_z;
        de.reserved = 0;
        entries_table.push_back(de);
    }

    DiskHeader hdr{};
    hdr.magic = SPIRV_CACHE_MAGIC;
    hdr.version = SPIRV_CACHE_VERSION;
    hdr.flags = (unique_ok == prelim.size()) ? 1u : 0u; // SPIRV_CACHE_FLAG_ALL_VALIDATED
    hdr.entry_count = static_cast<uint32_t>(prelim.size());
    hdr.entries_offset = entries_offset;
    hdr.bindings_offset = bindings_offset;
    hdr.vertex_attrs_offset = vertex_attrs_offset;
    hdr.strings_offset = strings_offset;
    hdr.spirv_blob_offset = spirv_blob_offset;
    hdr.total_file_size = total_file_size;
    hdr.total_translated = (unique_ok == prelim.size()) ? total_archive_shaders : unique_ok;
    hdr.total_spirv_val_passed = (unique_ok == prelim.size()) ? total_archive_shaders : unique_ok;

    std::string tmp_path = std::string(out_path) + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            err = "Cannot create output shader cache file: " + tmp_path;
            return false;
        }
        out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
        if (!entries_table.empty()) {
            out.write(reinterpret_cast<const char *>(entries_table.data()),
                      static_cast<std::streamsize>(entries_size));
        }
        if (!bindings_table.empty()) {
            out.write(reinterpret_cast<const char *>(bindings_table.data()),
                      static_cast<std::streamsize>(bindings_size));
        }
        if (!vattrs_table.empty()) {
            out.write(reinterpret_cast<const char *>(vattrs_table.data()),
                      static_cast<std::streamsize>(vertex_attrs_size));
        }
        if (!string_table.empty()) {
            out.write(reinterpret_cast<const char *>(string_table.data()),
                      static_cast<std::streamsize>(string_table.size()));
        }
        if (!spirv_blob.empty()) {
            out.write(reinterpret_cast<const char *>(spirv_blob.data()),
                      static_cast<std::streamsize>(spirv_blob.size()));
        }
        out.flush();
        if (!out) {
            err = "Failed writing shader cache to disk";
            return false;
        }
    }

    std::remove(out_path);
    if (std::rename(tmp_path.c_str(), out_path) != 0) {
        err = "Failed renaming temporary shader cache to final path";
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

static void set_err(char *err_buf, size_t err_buf_sz, const std::string &msg) {
    if (err_buf && err_buf_sz > 0) {
        std::snprintf(err_buf, err_buf_sz, "%s", msg.c_str());
    }
}

} // namespace

extern "C" int oh2_compile_shaders_from_pak(const char *pak_path,
                                            const char *out_cache_path,
                                            int num_threads,
                                            Air2SpirvProgressFn progress_cb,
                                            void *user_data,
                                            char *err_buf,
                                            size_t err_buf_sz) {
    if (!pak_path || !out_cache_path) {
        set_err(err_buf, err_buf_sz, "Null path argument");
        return -1;
    }

    if (progress_cb) progress_cb(0, 100, "Reading PAK shader archives...", user_data);

    std::unordered_set<std::string> targets = {
        "OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode",
        "OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode",
    };
    std::unordered_map<std::string, std::vector<uint8_t>> pak_blobs;
    std::string err;
    if (!extract_pak_files(pak_path, targets, pak_blobs, err)) {
        set_err(err_buf, err_buf_sz, err);
        return -2;
    }

    std::vector<ArchiveEntry> all_entries;
    if (!parse_ushaderbytecode(pak_blobs["OH2/Content/ShaderArchive-Global-SF_METAL.ushaderbytecode"],
                               "Global", all_entries, err)) {
        set_err(err_buf, err_buf_sz, err);
        return -3;
    }
    if (!parse_ushaderbytecode(pak_blobs["OH2/Content/ShaderArchive-OH2-SF_METAL.ushaderbytecode"],
                               "OH2", all_entries, err)) {
        set_err(err_buf, err_buf_sz, err);
        return -3;
    }
    pak_blobs.clear();

    uint32_t total_archive_shaders = static_cast<uint32_t>(all_entries.size());

    // Deduplicate by shader_id
    std::unordered_map<uint64_t, size_t> seen;
    std::vector<CompiledWorkItem> work;
    work.reserve(all_entries.size());
    for (auto &ae : all_entries) {
        if (seen.find(ae.shader_id) == seen.end()) {
            seen[ae.shader_id] = work.size();
            CompiledWorkItem item;
            item.shader_id = ae.shader_id;
            item.pak_sha1 = ae.pak_sha1;
            item.freq = ae.freq;
            item.raw = std::move(ae.raw);
            work.push_back(std::move(item));
        }
    }
    all_entries.clear();
    all_entries.shrink_to_fit();

    const size_t total_unique = work.size();
    if (progress_cb) {
        progress_cb(0, static_cast<int>(total_unique), "Compiling AIR 2.0 shaders to SPIR-V...", user_data);
    }

    int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (num_threads <= 0) {
        num_threads = hw_threads > 0 ? std::min(hw_threads, 8) : 4;
    }
    if (num_threads > static_cast<int>(total_unique)) {
        num_threads = std::max(1, static_cast<int>(total_unique));
    }

    std::atomic<size_t> next_idx{0};
    std::atomic<size_t> completed{0};
    std::atomic<size_t> failed{0};
    std::mutex cb_mtx;
    std::string first_err;

    auto worker_fn = [&]() {
        while (true) {
            size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= total_unique) break;
            auto &item = work[idx];
            std::string local_err;
            item.ok = oh2::air2spirv::translate_air_shader(
                item.raw.data(), item.raw.size(), item.freq, item.spirv_words, item.refl, local_err);
            // Free raw AIR bytes immediately to keep peak RAM low on mobile devices
            item.raw.clear();
            item.raw.shrink_to_fit();
            if (!item.ok) {
                item.err = local_err;
                failed.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(cb_mtx);
                if (first_err.empty()) {
                    first_err = "Shader translation failed (" + item.refl.entry_name + "): " + local_err;
                }
            }
            size_t done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress_cb && ((done & 31u) == 0u || done == total_unique)) {
                std::lock_guard<std::mutex> lk(cb_mtx);
                progress_cb(static_cast<int>(done), static_cast<int>(total_unique),
                            "Compiling AIR 2.0 shaders to SPIR-V...", user_data);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_threads));
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(worker_fn);
    }
    for (auto &th : threads) {
        th.join();
    }

    if (failed.load() > 0) {
        set_err(err_buf, err_buf_sz, first_err);
        return -4;
    }

    if (progress_cb) {
        progress_cb(static_cast<int>(total_unique), static_cast<int>(total_unique),
                    "Packing SPIR-V shader cache...", user_data);
    }

    std::vector<CompiledWorkItem *> sorted_items;
    sorted_items.reserve(total_unique);
    for (auto &w : work) sorted_items.push_back(&w);
    std::sort(sorted_items.begin(), sorted_items.end(),
              [](const CompiledWorkItem *a, const CompiledWorkItem *b) {
                  return a->shader_id < b->shader_id;
              });

    if (!write_spvc_cache(sorted_items, total_archive_shaders, out_cache_path, err)) {
        set_err(err_buf, err_buf_sz, err);
        return -5;
    }

    if (progress_cb) {
        progress_cb(static_cast<int>(total_unique), static_cast<int>(total_unique),
                    "Shader compilation complete", user_data);
    }
    return 0;
}
