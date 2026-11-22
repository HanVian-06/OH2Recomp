#include <android/api-level.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "macho_loader.h"
#include "libc_wrapper.h"
#include "oh2_runtime.h"
#include "mtl_vk.h"
#include "platform_bridges.h"

#define APP_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "OH2_APP", __VA_ARGS__)
#define APP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OH2_APP", __VA_ARGS__)

namespace {

struct Oh2AppState {
    struct android_app *app = nullptr;
    ANativeWindow      *window = nullptr;
    bool                vulkan_initialized = false;
    bool                ue4_started = false;
    bool                test_mode = false;
    std::string         test_name;
    std::string         bundle_root;
    std::string         spirv_cache_path;
    MachOImage          macho_img{};
    pthread_t           ue4_thread{};
};

static Oh2AppState g_app_state;

static void (*g_orig_on_window_focus_changed)(ANativeActivity *, int) = nullptr;
static void (*g_orig_on_resume)(ANativeActivity *) = nullptr;
static void (*g_orig_on_native_window_created)(ANativeActivity *, ANativeWindow *) = nullptr;

static void apply_immersive_fullscreen_ui_thread(ANativeActivity *activity) {
    if (!activity || !activity->clazz) return;
    JNIEnv *env = activity->env;
    if (!env && activity->vm) {
        if (activity->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
            return;
        }
    }
    if (!env) return;

    jclass cls_act = env->GetObjectClass(activity->clazz);
    if (!cls_act) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jmethodID mid_get_win = env->GetMethodID(cls_act, "getWindow", "()Landroid/view/Window;");
    jobject win = mid_get_win ? env->CallObjectMethod(activity->clazz, mid_get_win) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        win = nullptr;
    }

    if (win) {
        jclass cls_win = env->GetObjectClass(win);
        if (cls_win) {
            jmethodID mid_add_flags = env->GetMethodID(cls_win, "addFlags", "(I)V");
            if (mid_add_flags) {
                /* FLAG_KEEP_SCREEN_ON (0x80) | FLAG_FULLSCREEN (0x400) */
                env->CallVoidMethod(win, mid_add_flags, static_cast<jint>(0x00000080 | 0x00000400));
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            jmethodID mid_get_attrs = env->GetMethodID(cls_win, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
            jmethodID mid_set_attrs = env->GetMethodID(cls_win, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
            if (mid_get_attrs && mid_set_attrs) {
                jobject lp = env->CallObjectMethod(win, mid_get_attrs);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    lp = nullptr;
                }
                if (lp) {
                    jclass cls_lp = env->GetObjectClass(lp);
                    jfieldID fid_cutout = cls_lp ? env->GetFieldID(cls_lp, "layoutInDisplayCutoutMode", "I") : nullptr;
                    if (fid_cutout) {
                        const jint mode = (android_get_device_api_level() >= 30) ? 3 /* ALWAYS */ : 1 /* SHORT_EDGES */;
                        env->SetIntField(lp, fid_cutout, mode);
                    } else if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    }
                    env->CallVoidMethod(win, mid_set_attrs, lp);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (cls_lp) env->DeleteLocalRef(cls_lp);
                    env->DeleteLocalRef(lp);
                }
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }

            if (android_get_device_api_level() >= 30) {
                jmethodID mid_decor_fits = env->GetMethodID(cls_win, "setDecorFitsSystemWindows", "(Z)V");
                if (mid_decor_fits) {
                    env->CallVoidMethod(win, mid_decor_fits, JNI_FALSE);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                } else if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }

                jmethodID mid_get_ic = env->GetMethodID(cls_win, "getInsetsController", "()Landroid/view/WindowInsetsController;");
                jobject ic = mid_get_ic ? env->CallObjectMethod(win, mid_get_ic) : nullptr;
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    ic = nullptr;
                }
                if (ic) {
                    jclass cls_ic = env->GetObjectClass(ic);
                    if (cls_ic) {
                        jmethodID mid_hide = env->GetMethodID(cls_ic, "hide", "(I)V");
                        if (mid_hide) {
                            /* statusBars(1) | navigationBars(2) | captionBar(4) = 7 */
                            env->CallVoidMethod(ic, mid_hide, static_cast<jint>(7));
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        jmethodID mid_beh = env->GetMethodID(cls_ic, "setSystemBarsBehavior", "(I)V");
                        if (mid_beh) {
                            /* BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE = 2 */
                            env->CallVoidMethod(ic, mid_beh, static_cast<jint>(2));
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->DeleteLocalRef(cls_ic);
                    }
                    env->DeleteLocalRef(ic);
                }
            }

            jmethodID mid_get_decor = env->GetMethodID(cls_win, "getDecorView", "()Landroid/view/View;");
            jobject decor = mid_get_decor ? env->CallObjectMethod(win, mid_get_decor) : nullptr;
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                decor = nullptr;
            }
            if (decor) {
                jclass cls_view = env->GetObjectClass(decor);
                jmethodID mid_sys_vis = cls_view ? env->GetMethodID(cls_view, "setSystemUiVisibility", "(I)V") : nullptr;
                if (mid_sys_vis) {
                    /* IMMERSIVE_STICKY | FULLSCREEN | HIDE_NAVIGATION | LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION | LAYOUT_FULLSCREEN */
                    env->CallVoidMethod(decor, mid_sys_vis, static_cast<jint>(0x1706));
                    if (env->ExceptionCheck()) env->ExceptionClear();
                } else if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }
                if (cls_view) env->DeleteLocalRef(cls_view);
                env->DeleteLocalRef(decor);
            }

            env->DeleteLocalRef(cls_win);
        }
        env->DeleteLocalRef(win);
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls_act);
}

static void hooked_on_window_focus_changed(ANativeActivity *activity, int has_focus) {
    if (g_orig_on_window_focus_changed) g_orig_on_window_focus_changed(activity, has_focus);
    if (has_focus) apply_immersive_fullscreen_ui_thread(activity);
}

static void hooked_on_resume(ANativeActivity *activity) {
    if (g_orig_on_resume) g_orig_on_resume(activity);
    apply_immersive_fullscreen_ui_thread(activity);
}

static void hooked_on_native_window_created(ANativeActivity *activity, ANativeWindow *window) {
    if (g_orig_on_native_window_created) g_orig_on_native_window_created(activity, window);
    apply_immersive_fullscreen_ui_thread(activity);
}

static std::string find_self_apk_path() {
    FILE *fp = std::fopen("/proc/self/maps", "r");
    if (!fp) return "";
    char line[1024];
    std::string found;
    while (std::fgets(line, sizeof(line), fp)) {
        const char *p = std::strstr(line, "/data/app/");
        if (!p) p = std::strstr(line, "/mnt/expand/");
        if (!p) continue;
        const char *apk = std::strstr(p, "base.apk");
        if (apk) {
            size_t len = (size_t)(apk + 8 - p);
            found.assign(p, len);
            break;
        }
    }
    std::fclose(fp);
    return found;
}

static bool check_trigger_file(const std::string &dir, const char *filename, std::string *out_content) {
    if (dir.empty()) return false;
    std::string path = dir + "/" + filename;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    char buf[256]{};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    ::unlink(path.c_str());
    if (out_content) {
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
            buf[--n] = '\0';
        }
        *out_content = buf;
    }
    return true;
}

static void *ue4_main_thread_fn(void *raw) {
    auto *st = static_cast<Oh2AppState *>(raw);
    oh2_darwin_pthread_setname_np("OH2_iOSMain");
    oh2_set_ios_main_thread();

    std::string apk_path = find_self_apk_path();
    APP_LOGI("Loading Mach-O from APK='%s' entry='assets/oh2_macho.bin'", apk_path.c_str());
    int rc = -1;
    if (!apk_path.empty()) {
        rc = macho_load_from_apk(apk_path.c_str(), "assets/oh2_macho.bin", &st->macho_img);
    }
    if (rc != 0) {
        std::string fallback = st->bundle_root + "/OH2";
        APP_LOGI("Fallback macho_load('%s')", fallback.c_str());
        rc = macho_load(fallback.c_str(), &st->macho_img);
    }
    if (rc != 0) {
        APP_LOGE("FATAL: Failed to load OH2 Mach-O binary");
        return nullptr;
    }

    oh2_runtime_post_image_overrides();

    {
        std::string pak_dir = st->bundle_root + "/cookeddata/oh2/content/paks";
        std::string pak_file = pak_dir + "/oh2-ios.pak";
        struct stat st_pak{};
        int stat_rc = ::stat(pak_file.c_str(), &st_pak);
        int stat_err = stat_rc == 0 ? 0 : errno;
        DIR *d = ::opendir(pak_dir.c_str());
        int dir_err = d ? 0 : errno;
        int pak_seen = 0;
        if (d) {
            while (struct dirent *ent = ::readdir(d)) {
                if (std::strcmp(ent->d_name, "oh2-ios.pak") == 0) pak_seen = 1;
            }
            ::closedir(d);
        }
        APP_LOGI("Pak preflight: dir='%s' stat_rc=%d(size=%lld,err=%d) opendir_ok=%d(err=%d,pak_seen=%d)",
                 pak_dir.c_str(), stat_rc, (long long)st_pak.st_size, stat_err, d ? 1 : 0, dir_err, pak_seen);
    }

    const char *argv_fake[] = { "OH2", nullptr };
    const char *envp_fake[] = { nullptr };
    const char *apple_fake[] = { "executable_path=/sdcard/Android/data/com.oceanhorn2.port/files/OH2", nullptr };

    macho_run_initializers(&st->macho_img, 1, (char **)argv_fake, (char **)envp_fake, (char **)apple_fake);
    int main_rc = macho_call_main(&st->macho_img, 1, (char **)argv_fake, (char **)envp_fake, (char **)apple_fake);
    APP_LOGI("UE4 LC_MAIN exited with code %d", main_rc);
    return nullptr;
}

static void on_window_ready(Oh2AppState *st) {
    if (!st->window) return;

    std::string tname;
    if (check_trigger_file(st->bundle_root, ".oh2_test", &tname)) {
        st->test_mode = true;
        st->test_name = tname.empty() ? "metal_objects" : tname;
        APP_LOGI("Running F10 self-test mode: '%s'", st->test_name.c_str());
        int test_rc = mtl_vk_run_selftest(st->test_name.c_str(), st->window, st->spirv_cache_path.c_str());
        mtl_vk_shutdown();
        APP_LOGI("Self-test '%s' finished rc=%d", st->test_name.c_str(), test_rc);
        ANativeActivity_finish(st->app->activity);
        return;
    }

    if (!st->vulkan_initialized) {
        std::string dummy;
        int enable_val = check_trigger_file(st->bundle_root, ".oh2_validate", &dummy) ? 1 : 0;

        oh2_runtime_init(st->bundle_root.c_str());
        oh2_runtime_set_enable_main_thread(1);
        oh2_runtime_set_live_loop_mode(1);

        if (mtl_vk_init(st->window, st->spirv_cache_path.c_str(), enable_val) != 0) {
            APP_LOGE("mtl_vk_init failed");
            return;
        }
        mtl_vk_register_objc_bridges();
        oh2_platform_init();
        oh2_platform_on_choreographer_attach();
        st->vulkan_initialized = true;
    } else {
        mtl_vk_set_surface(st->window);
        oh2_platform_audio_ensure_active();
    }

    if (!st->ue4_started) {
        st->ue4_started = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8U * 1024U * 1024U);
        pthread_create(&st->ue4_thread, &attr, ue4_main_thread_fn, st);
        pthread_attr_destroy(&attr);
        pthread_detach(st->ue4_thread);
    }
}

static void handle_cmd(struct android_app *app, int32_t cmd) {
    auto *st = static_cast<Oh2AppState *>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                st->window = app->window;
                on_window_ready(st);
            }
            break;
        case APP_CMD_WINDOW_RESIZED:
            if (app->window && st->vulkan_initialized && !st->test_mode) {
                st->window = app->window;
                mtl_vk_set_surface(st->window);
            }
            break;
        case APP_CMD_GAINED_FOCUS:
        case APP_CMD_RESUME:
            if (st->vulkan_initialized && !st->test_mode) {
                oh2_platform_on_choreographer_attach();
                oh2_platform_audio_resume();
            }
            break;
        case APP_CMD_PAUSE:
        case APP_CMD_LOST_FOCUS:
        case APP_CMD_STOP:
        case APP_CMD_TERM_WINDOW:
            if (st->vulkan_initialized && !st->test_mode) {
                oh2_platform_audio_pause();
            }
            break;
        case APP_CMD_LOW_MEMORY:
            APP_LOGI("[M5/M6] onLowMemory / trim signal received");
            break;
        case APP_CMD_DESTROY:
            oh2_runtime_stop_live_loop();
            oh2_platform_shutdown();
            mtl_vk_shutdown();
            break;
        default:
            break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    return oh2_platform_handle_input_event(event);
}

} // namespace

void android_main(struct android_app *app) {
    if (app && app->activity) {
        ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
        if (app->activity->callbacks) {
            g_orig_on_window_focus_changed = app->activity->callbacks->onWindowFocusChanged;
            app->activity->callbacks->onWindowFocusChanged = hooked_on_window_focus_changed;
            g_orig_on_resume = app->activity->callbacks->onResume;
            app->activity->callbacks->onResume = hooked_on_resume;
            g_orig_on_native_window_created = app->activity->callbacks->onNativeWindowCreated;
            app->activity->callbacks->onNativeWindowCreated = hooked_on_native_window_created;
        }
    }
    g_app_state.app = app;
    g_app_state.bundle_root = "/sdcard/Android/data/com.oceanhorn2.port/files";
    if (app->activity && app->activity->externalDataPath && app->activity->externalDataPath[0]) {
        struct stat st_dir{};
        std::string ext = app->activity->externalDataPath;
        if (stat((ext + "/cookeddata/oh2/content/paks/oh2-ios.pak").c_str(), &st_dir) == 0 ||
            stat((ext + "/.oh2_test").c_str(), &st_dir) == 0) {
            g_app_state.bundle_root = ext;
        }
    }
    g_app_state.spirv_cache_path = g_app_state.bundle_root + "/spirv_cache.bin";

    app->userData = &g_app_state;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    int events = 0;
    struct android_poll_source *source = nullptr;
    while (!app->destroyRequested) {
        while (ALooper_pollOnce(16, nullptr, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
        }
    }
}
