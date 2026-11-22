#include <android/log.h>
#include <android_native_app_glue.h>

#include "oh2_runtime.h"

static void handle_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    if (cmd == APP_CMD_INIT_WINDOW) {
        __android_log_print(ANDROID_LOG_INFO, "OH2_M2", "m2runtime NativeActivity window ready");
    }
}

void android_main(struct android_app *app) {
    app->onAppCmd = handle_cmd;
    oh2_runtime_init("/sdcard/Android/data/com.oceanhorn2.port/files");
    oh2_runtime_run_selftest("all_unit", "/data/local/tmp/OH2");
    __android_log_print(ANDROID_LOG_INFO, "OH2_M2", "[M2_APK] M2 runtime self-test ready");

    int events = 0;
    struct android_poll_source *source = nullptr;
    while (!app->destroyRequested) {
        while (ALooper_pollOnce(100, nullptr, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
        }
    }
}
