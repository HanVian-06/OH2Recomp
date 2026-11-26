#ifndef OH2_AIR2SPIRV_API_H_
#define OH2_AIR2SPIRV_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Air2SpirvProgressFn)(int done, int total, const char *stage, void *user_data);

/*
 * Extracts ShaderArchive-Global-SF_METAL.ushaderbytecode & ShaderArchive-OH2-SF_METAL.ushaderbytecode
 * from oh2-ios.pak, translates all unique AIR 2.0 MTLB modules to Vulkan 1.1 SPIR-V 1.3 using
 * num_threads worker threads, and writes SPVC v1 cache to out_cache_path.
 * Returns 0 on success, non-zero on error (with message written to err_buf if non-null).
 */
int oh2_compile_shaders_from_pak(const char *pak_path,
                                 const char *out_cache_path,
                                 int num_threads,
                                 Air2SpirvProgressFn progress_cb,
                                 void *user_data,
                                 char *err_buf,
                                 size_t err_buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* OH2_AIR2SPIRV_API_H_ */
