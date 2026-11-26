#include "platform_bridges.h"
#include "../runtime/oh2_runtime.h"

#include <android/choreographer.h>
#include <android/log.h>
#include <atomic>

#define VSYNC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OH2_VSYNC", __VA_ARGS__)

namespace {

static std::atomic<int> g_choreographer_active{0};
static std::atomic<uint64_t> g_vsync_frame_count{0};

static void choreographer_frame_cb(int64_t frame_time_nanos, void *data) {
    (void)data;
    if (!g_choreographer_active.load()) return;

    AChoreographer *ch = AChoreographer_getInstance();
    if (ch) {
        AChoreographer_postFrameCallback64(ch, choreographer_frame_cb, nullptr);
    }

    double ts = (double)frame_time_nanos * 1e-9;
    double target = ts + (1.0 / 60.0);
    oh2_cadisplaylink_tick_all(ts, target);

    uint64_t fc = g_vsync_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fc == 1 || (fc % 300) == 0) {
        VSYNC_LOGI("AChoreographer VSYNC tick #%llu (ts=%.3f)", (unsigned long long)fc, ts);
    }
}

} // namespace

extern "C" {

void oh2_platform_on_choreographer_attach(void) {
    if (g_choreographer_active.exchange(1) == 1) return;
    oh2_cadisplaylink_set_vsync_driven(1);
    AChoreographer *ch = AChoreographer_getInstance();
    if (ch) {
        AChoreographer_postFrameCallback64(ch, choreographer_frame_cb, nullptr);
        VSYNC_LOGI("Attached AChoreographer_postFrameCallback64 -> CADisplayLink");
    } else {
        oh2_cadisplaylink_set_vsync_driven(0);
        g_choreographer_active.store(0);
    }
}

void oh2_platform_choreographer_shutdown(void) {
    g_choreographer_active.store(0);
}

} // extern "C"
