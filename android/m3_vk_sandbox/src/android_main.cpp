#include "vk_foundation/m3_renderer.h"
#include "vk_foundation/vk_log.h"

#include <android/asset_manager.h>
#include <android_native_app_glue.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct AppState {
    android_app* app = nullptr;
    oh2::vk::M3Renderer renderer;
    std::vector<uint32_t> vert_spirv;
    std::vector<uint32_t> frag_spirv;
    bool can_render = false;
    bool selftest_failed = false;
};

bool load_spirv_asset(AAssetManager* mgr, const char* asset_path, std::vector<uint32_t>& out_words) {
    if (!mgr || !asset_path) return false;
    AAsset* asset = AAssetManager_open(mgr, asset_path, AASSET_MODE_BUFFER);
    if (!asset) {
        M3_LOGE("Failed to open SPIR-V asset: %s", asset_path);
        return false;
    }
    const off_t len = AAsset_getLength(asset);
    if (len <= 0 || (len % 4) != 0) {
        M3_LOGE("Invalid SPIR-V asset length (%ld) for %s", static_cast<long>(len), asset_path);
        AAsset_close(asset);
        return false;
    }
    out_words.resize(static_cast<size_t>(len) / sizeof(uint32_t));
    const int read_bytes = AAsset_read(asset, out_words.data(), static_cast<size_t>(len));
    AAsset_close(asset);
    if (read_bytes != len) {
        M3_LOGE("Short read on SPIR-V asset: %s", asset_path);
        out_words.clear();
        return false;
    }
    return true;
}

void init_window_renderer(AppState* state) {
    if (!state || !state->app || !state->app->window) return;

    if (state->vert_spirv.empty() || state->frag_spirv.empty()) {
        AAssetManager* mgr = state->app->activity ? state->app->activity->assetManager : nullptr;
        if (!load_spirv_asset(mgr, "shaders/triangle.vert.spv", state->vert_spirv) ||
            !load_spirv_asset(mgr, "shaders/triangle.frag.spv", state->frag_spirv)) {
            state->selftest_failed = true;
            return;
        }
    }

    oh2::vk::M3RendererConfig cfg{};
    cfg.window = state->app->window;
    cfg.vert_spirv = state->vert_spirv.data();
    cfg.vert_spirv_bytes = state->vert_spirv.size() * sizeof(uint32_t);
    cfg.frag_spirv = state->frag_spirv.data();
    cfg.frag_spirv_bytes = state->frag_spirv.size() * sizeof(uint32_t);
    cfg.staging_ring_bytes = oh2::vk::StagingRingBuffer::kMinCapacityBytes;
    cfg.enable_validation = true;

    if (!state->renderer.init(cfg)) {
        M3_LOGE("M3Renderer::init failed");
        state->selftest_failed = true;
        state->can_render = false;
        return;
    }

    if (!state->renderer.run_self_tests()) {
        M3_LOGE("M3Renderer::run_self_tests reported failure");
        state->selftest_failed = true;
    }

    state->can_render = true;
}

void on_app_cmd(android_app* app, int32_t cmd) {
    auto* state = static_cast<AppState*>(app->userData);
    if (!state) return;

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != nullptr) {
                M3_LOGI("APP_CMD_INIT_WINDOW (%dx%d)",
                        ANativeWindow_getWidth(app->window),
                        ANativeWindow_getHeight(app->window));
                init_window_renderer(state);
            }
            break;

        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (state->can_render && app->window != nullptr) {
                state->renderer.handle_window_resized(app->window);
            }
            break;

        case APP_CMD_TERM_WINDOW:
            M3_LOGI("APP_CMD_TERM_WINDOW");
            state->can_render = false;
            state->renderer.destroy();
            break;

        default:
            break;
    }
}

}  // namespace

extern "C" void android_main(struct android_app* app) {
    M3_LOGI("OH2 M3 Vulkan Sandbox starting");

    AppState state{};
    state.app = app;
    app->userData = &state;
    app->onAppCmd = on_app_cmd;

    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int timeout_ms = state.can_render ? 0 : -1;

        while (ALooper_pollOnce(timeout_ms, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
        }

        if (state.can_render && !app->destroyRequested) {
            state.renderer.render_steady_frame();
        }
    }

    state.renderer.destroy();
    M3_LOGI("OH2 M3 Vulkan Sandbox exiting cleanly");
}
