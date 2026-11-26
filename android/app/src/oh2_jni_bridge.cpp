#include <jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>

#include "air2spirv_api.h"
#include "spirv_cache.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OH2_JNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OH2_JNI", __VA_ARGS__)

namespace {

struct JniProgressCtx {
    JavaVM *vm = nullptr;
    jobject listener_global = nullptr;
    jmethodID on_progress_mid = nullptr;
};

static void jni_progress_cb(int done, int total, const char *stage, void *user_data) {
    auto *ctx = static_cast<JniProgressCtx *>(user_data);
    if (!ctx || !ctx->vm || !ctx->listener_global || !ctx->on_progress_mid) return;

    JNIEnv *env = nullptr;
    bool attached = false;
    jint res = ctx->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (ctx->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    } else if (res != JNI_OK) {
        return;
    }

    jstring jstage = env->NewStringUTF(stage ? stage : "");
    env->CallVoidMethod(ctx->listener_global, ctx->on_progress_mid,
                        static_cast<jint>(done), static_cast<jint>(total), jstage);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (jstage) env->DeleteLocalRef(jstage);

    if (attached) {
        ctx->vm->DetachCurrentThread();
    }
}

static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_oceanhorn2_port_NativeBridge_nativeVerifyMachO(JNIEnv *env, jclass, jstring jpath) {
    if (!jpath) return -1;
    const char *path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) return -1;

    int result = -1;
    std::ifstream f(path, std::ios::binary);
    if (f.is_open()) {
        uint8_t hdr[32];
        f.read(reinterpret_cast<char *>(hdr), sizeof(hdr));
        if (f) {
            uint32_t magic = rd_u32(hdr + 0);
            uint32_t cputype = rd_u32(hdr + 4);
            uint32_t ncmds = rd_u32(hdr + 16);
            uint32_t sizeofcmds = rd_u32(hdr + 20);
            if (magic == 0xFEEDFACFu && cputype == 0x0100000Cu && sizeofcmds <= 2 * 1024 * 1024) {
                std::vector<uint8_t> cmds(sizeofcmds);
                f.read(reinterpret_cast<char *>(cmds.data()), sizeofcmds);
                if (f) {
                    result = 0; // Valid ARM64 Mach-O (decrypted unless cryptid != 0)
                    size_t off = 0;
                    for (uint32_t i = 0; i < ncmds && off + 8 <= cmds.size(); ++i) {
                        uint32_t cmd = rd_u32(cmds.data() + off);
                        uint32_t cmdsize = rd_u32(cmds.data() + off + 4);
                        if (cmdsize < 8 || off + cmdsize > cmds.size()) break;
                        // LC_ENCRYPTION_INFO_64 = 0x2C, cryptid at offset 16
                        if (cmd == 0x2Cu && cmdsize >= 20) {
                            uint32_t cryptid = rd_u32(cmds.data() + off + 16);
                            if (cryptid != 0) {
                                result = 1; // FairPlay encrypted
                                break;
                            }
                        }
                        off += cmdsize;
                    }
                }
            }
        }
    }

    env->ReleaseStringUTFChars(jpath, path);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_oceanhorn2_port_NativeBridge_nativeVerifySpirvCache(JNIEnv *env, jclass, jstring jpath) {
    if (!jpath) return 0;
    const char *path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) return 0;

    jint count = 0;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (f.is_open()) {
        std::streamoff sz = f.tellg();
        if (sz >= static_cast<std::streamoff>(sizeof(SpirvCacheHeader))) {
            f.seekg(0, std::ios::beg);
            SpirvCacheHeader hdr{};
            f.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
            if (f &&
                hdr.magic == SPIRV_CACHE_MAGIC &&
                hdr.version == SPIRV_CACHE_VERSION &&
                hdr.entry_count > 0 &&
                hdr.total_file_size == static_cast<uint64_t>(sz)) {
                count = static_cast<jint>(hdr.total_translated > 0 ? hdr.total_translated : hdr.entry_count);
            }
        }
    }

    env->ReleaseStringUTFChars(jpath, path);
    return count;
}

JNIEXPORT jstring JNICALL
Java_com_oceanhorn2_port_NativeBridge_nativeCompileShadersFromPak(JNIEnv *env, jclass,
                                                                  jstring jpakPath,
                                                                  jstring joutCachePath,
                                                                  jobject jlistener) {
    if (!jpakPath || !joutCachePath) {
        return env->NewStringUTF("Invalid null path argument");
    }
    const char *pak_path = env->GetStringUTFChars(jpakPath, nullptr);
    const char *out_path = env->GetStringUTFChars(joutCachePath, nullptr);

    JniProgressCtx ctx{};
    if (jlistener) {
        env->GetJavaVM(&ctx.vm);
        ctx.listener_global = env->NewGlobalRef(jlistener);
        jclass cls = env->GetObjectClass(jlistener);
        if (cls) {
            ctx.on_progress_mid = env->GetMethodID(cls, "onProgress", "(IILjava/lang/String;)V");
            env->DeleteLocalRef(cls);
        }
    }

    char err_buf[512] = {0};
    LOGI("Compiling shaders from PAK: %s -> %s", pak_path, out_path);
    int rc = oh2_compile_shaders_from_pak(
        pak_path,
        out_path,
        0, // auto hardware threads (up to 8)
        ctx.on_progress_mid ? jni_progress_cb : nullptr,
        &ctx,
        err_buf,
        sizeof(err_buf));

    if (ctx.listener_global) {
        env->DeleteGlobalRef(ctx.listener_global);
    }
    env->ReleaseStringUTFChars(jpakPath, pak_path);
    env->ReleaseStringUTFChars(joutCachePath, out_path);

    if (rc != 0) {
        LOGE("Shader compilation failed (rc=%d): %s", rc, err_buf);
        return env->NewStringUTF(err_buf[0] ? err_buf : "Shader compilation failed");
    }
    LOGI("Shader compilation succeeded");
    return nullptr;
}

JNIEXPORT jstring JNICALL
Java_com_oceanhorn2_port_NativeBridge_nativeGetVulkanDeviceInfo(JNIEnv *env, jclass) {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Oceanhorn 2 Launcher";
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;

    std::string info = "Vulkan 1.1+ Device";
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) == VK_SUCCESS && inst != VK_NULL_HANDLE) {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(inst, &count, nullptr) == VK_SUCCESS && count > 0) {
            std::vector<VkPhysicalDevice> devs(count);
            if (vkEnumeratePhysicalDevices(inst, &count, devs.data()) == VK_SUCCESS && count > 0) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(devs[0], &props);
                uint32_t maj = VK_VERSION_MAJOR(props.apiVersion);
                uint32_t min = VK_VERSION_MINOR(props.apiVersion);
                char buf[320];
                std::snprintf(buf, sizeof(buf), "%s  •  Vulkan %u.%u", props.deviceName, maj, min);
                info = buf;
            }
        }
        vkDestroyInstance(inst, nullptr);
    }
    return env->NewStringUTF(info.c_str());
}

} // extern "C"
