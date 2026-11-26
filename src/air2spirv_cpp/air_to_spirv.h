#ifndef OH2_AIR2SPIRV_AIR_TO_SPIRV_H_
#define OH2_AIR2SPIRV_AIR_TO_SPIRV_H_

#include "bitcode_reader.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace oh2::air2spirv {

struct BindingRefl {
    uint8_t set = 0;
    uint8_t binding = 0;
    uint8_t descriptor_type = 0;
    uint8_t metal_slot = 0;
    uint8_t metal_kind = 0;
    uint8_t image_dim = 0;
    uint8_t is_depth = 0;
    uint8_t flags = 0;
    uint32_t buffer_size_bytes = 0;
};

struct VertexAttrRefl {
    uint8_t location = 0;
    uint8_t component_type = 0;
    uint8_t components = 0;
    uint8_t flags = 0;
    std::string name;
};

struct ShaderReflection {
    uint64_t shader_id = 0;
    std::array<uint8_t, 32> mtlb_sha256{};
    std::string entry_name = "main";
    uint32_t stage = 0;
    std::vector<BindingRefl> bindings;
    std::vector<VertexAttrRefl> vertex_attrs;
    uint32_t buffer_mask = 0;
    uint32_t texture_mask = 0;
    uint16_t sampler_mask = 0;
    uint16_t fragment_output_mask = 0;
    uint16_t flags = 0;
    uint16_t local_size_x = 0;
    uint16_t local_size_y = 0;
    uint16_t local_size_z = 0;
};

bool translate_air_shader(const uint8_t *raw_entry, size_t len, uint32_t default_freq,
                          std::vector<uint32_t> &out_spirv, ShaderReflection &out_refl,
                          std::string &err);

} // namespace oh2::air2spirv

#endif /* OH2_AIR2SPIRV_AIR_TO_SPIRV_H_ */
