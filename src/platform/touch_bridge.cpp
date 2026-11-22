#include "platform_bridges.h"
#include "../loader/macho_loader.h"
#include "../runtime/oh2_runtime.h"

#include <android/log.h>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

#define TOUCH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OH2_TOUCH", __VA_ARGS__)

extern "C" {
void oh2_objc_msgSend(void);
void oh2_perform_selector_on_main_thread(void *target, const char *selector, void *arg, int wait);
}

namespace {

struct CGPoint64 {
    double x;
    double y;
};

struct UITouchPayload {
    void     *isa;
    double    x_pt;
    double    y_pt;
    double    prev_x_pt;
    double    prev_y_pt;
    double    timestamp;
    double    force;
    uint64_t  phase;      /* 0=Began, 1=Moved, 2=Stationary, 3=Ended, 4=Cancelled */
    uint64_t  tap_count;
    void     *view;
    void     *window;
    int32_t   pointer_id;
};

struct UIEventPayload {
    void   *isa;
    void   *all_touches_set;
    double  timestamp;
};

struct PendingTouchBatch {
    void        *target_view;
    const char  *selector;
    void        *touches_set;
    void        *ui_event;
};

static std::mutex g_touch_mutex;
static std::unordered_map<int32_t, void *> g_active_touches;
static void *g_touch_proxy = nullptr;

static double now_mono_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static CGPoint64 uitouch_locationInView(void *self, const char *sel, void *view) {
    (void)sel; (void)view;
    if (!self) return {0.0, 0.0};
    auto *t = static_cast<UITouchPayload *>(self);
    return {t->x_pt, t->y_pt};
}

static CGPoint64 uitouch_previousLocationInView(void *self, const char *sel, void *view) {
    (void)sel; (void)view;
    if (!self) return {0.0, 0.0};
    auto *t = static_cast<UITouchPayload *>(self);
    return {t->prev_x_pt, t->prev_y_pt};
}

static uint64_t uitouch_phase(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UITouchPayload *>(self)->phase : 3ULL;
}

static uint64_t uitouch_tapCount(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UITouchPayload *>(self)->tap_count : 1ULL;
}

static double uitouch_timestamp(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UITouchPayload *>(self)->timestamp : 0.0;
}

static double uitouch_force(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UITouchPayload *>(self)->force : 1.0;
}

static double uitouch_maxForce(void *self, const char *sel) {
    (void)self; (void)sel;
    return 1.0;
}

static void *uitouch_view(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UITouchPayload *>(self)->view : nullptr;
}

static void *uitouch_window(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UITouchPayload *>(self)->window : nullptr;
}

static int64_t uitouch_type(void *self, const char *sel) {
    (void)self; (void)sel;
    return 0; /* UITouchTypeDirect */
}

static void *uievent_allTouches(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UIEventPayload *>(self)->all_touches_set : nullptr;
}

static void *uievent_touchesForView(void *self, const char *sel, void *view) {
    (void)view;
    return uievent_allTouches(self, sel);
}

static double uievent_timestamp(void *self, const char *sel) {
    (void)sel;
    return self ? static_cast<UIEventPayload *>(self)->timestamp : 0.0;
}

static void touch_proxy_deliver(void *self, const char *sel, void *raw_batch) {
    (void)self; (void)sel;
    auto *batch = static_cast<PendingTouchBatch *>(raw_batch);
    if (!batch) return;
    void *target = batch->target_view ? batch->target_view : oh2_ui_get_active_ios_view();
    if (target && batch->selector) {
        typedef void (*touch_cb_t)(void *, const char *, void *, void *);
        ((touch_cb_t)oh2_objc_msgSend)(target, batch->selector, batch->touches_set, batch->ui_event);
    }
    delete batch;
}

} // namespace

extern "C" {

void oh2_platform_touch_register(void) {
    oh2_objc_ensure_class("UITouch", "NSObject");
    oh2_objc_ensure_class("UIEvent", "NSObject");
    oh2_objc_ensure_class("OH2TouchDispatchProxy", "NSObject");

    oh2_objc_register_instance_method("UITouch", "locationInView:",          (void *)&uitouch_locationInView);
    oh2_objc_register_instance_method("UITouch", "previousLocationInView:",  (void *)&uitouch_previousLocationInView);
    oh2_objc_register_instance_method("UITouch", "phase",                    (void *)&uitouch_phase);
    oh2_objc_register_instance_method("UITouch", "tapCount",                 (void *)&uitouch_tapCount);
    oh2_objc_register_instance_method("UITouch", "timestamp",                (void *)&uitouch_timestamp);
    oh2_objc_register_instance_method("UITouch", "force",                    (void *)&uitouch_force);
    oh2_objc_register_instance_method("UITouch", "maximumPossibleForce",     (void *)&uitouch_maxForce);
    oh2_objc_register_instance_method("UITouch", "view",                     (void *)&uitouch_view);
    oh2_objc_register_instance_method("UITouch", "window",                   (void *)&uitouch_window);
    oh2_objc_register_instance_method("UITouch", "type",                     (void *)&uitouch_type);

    oh2_objc_register_instance_method("UIEvent", "allTouches",               (void *)&uievent_allTouches);
    oh2_objc_register_instance_method("UIEvent", "touchesForView:",          (void *)&uievent_touchesForView);
    oh2_objc_register_instance_method("UIEvent", "touchesForWindow:",        (void *)&uievent_touchesForView);
    oh2_objc_register_instance_method("UIEvent", "timestamp",                (void *)&uievent_timestamp);

    oh2_objc_register_instance_method("OH2TouchDispatchProxy", "deliverTouch:", (void *)&touch_proxy_deliver);
    if (!g_touch_proxy) {
        g_touch_proxy = oh2_objc_alloc_instance(oh2_objc_get_class("OH2TouchDispatchProxy"), 64);
    }
}

int32_t oh2_platform_handle_input_event(AInputEvent *event) {
    if (!event) return 0;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        return 0;
    }

    int32_t raw_action = AMotionEvent_getAction(event);
    int32_t action = raw_action & AMOTION_EVENT_ACTION_MASK;
    int32_t action_idx = (raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t pointer_count = AMotionEvent_getPointerCount(event);
    if (pointer_count == 0) return 0;

    uint32_t w_px = 0, h_px = 0;
    double scale = 2.0;
    oh2_ui_get_screen_dimensions(&w_px, &h_px, &scale);
    if (scale <= 0.0) scale = 2.0;

    void *ios_view = oh2_ui_get_active_ios_view();
    void *win = oh2_ui_get_active_window();
    double ts = now_mono_sec();

    std::vector<void *> changed_touches;
    std::vector<void *> all_touches;
    const char *sel_name = nullptr;

    {
        std::lock_guard<std::mutex> lk(g_touch_mutex);

        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            int32_t idx = (action == AMOTION_EVENT_ACTION_DOWN) ? 0 : action_idx;
            int32_t pid = AMotionEvent_getPointerId(event, idx);
            double x_pt = (double)AMotionEvent_getX(event, idx) / scale;
            double y_pt = (double)AMotionEvent_getY(event, idx) / scale;

            auto *tp = static_cast<UITouchPayload *>(
                oh2_objc_alloc_instance(oh2_objc_get_class("UITouch"), sizeof(UITouchPayload)));
            if (tp) {
                tp->x_pt = x_pt;
                tp->y_pt = y_pt;
                tp->prev_x_pt = x_pt;
                tp->prev_y_pt = y_pt;
                tp->timestamp = ts;
                tp->force = 1.0;
                tp->phase = 0; /* Began */
                tp->tap_count = 1;
                tp->view = ios_view;
                tp->window = win;
                tp->pointer_id = pid;
                g_active_touches[pid] = tp;
                changed_touches.push_back(tp);
                TOUCH_LOGI("Touch BEGAN pid=%d pt=(%.1f, %.1f)", pid, x_pt, y_pt);
            }
            sel_name = "touchesBegan:withEvent:";
        } else if (action == AMOTION_EVENT_ACTION_MOVE) {
            for (size_t i = 0; i < pointer_count; ++i) {
                int32_t pid = AMotionEvent_getPointerId(event, i);
                double x_pt = (double)AMotionEvent_getX(event, i) / scale;
                double y_pt = (double)AMotionEvent_getY(event, i) / scale;
                auto it = g_active_touches.find(pid);
                if (it != g_active_touches.end() && it->second) {
                    auto *tp = static_cast<UITouchPayload *>(it->second);
                    tp->prev_x_pt = tp->x_pt;
                    tp->prev_y_pt = tp->y_pt;
                    tp->x_pt = x_pt;
                    tp->y_pt = y_pt;
                    tp->timestamp = ts;
                    tp->phase = 1; /* Moved */
                    changed_touches.push_back(tp);
                }
            }
            sel_name = "touchesMoved:withEvent:";
        } else if (action == AMOTION_EVENT_ACTION_UP ||
                   action == AMOTION_EVENT_ACTION_POINTER_UP ||
                   action == AMOTION_EVENT_ACTION_CANCEL) {
            int32_t idx = (action == AMOTION_EVENT_ACTION_POINTER_UP) ? action_idx : 0;
            bool is_cancel = (action == AMOTION_EVENT_ACTION_CANCEL);
            if (is_cancel) {
                for (auto &kv : g_active_touches) {
                    auto *tp = static_cast<UITouchPayload *>(kv.second);
                    if (tp) {
                        tp->phase = 4; /* Cancelled */
                        tp->timestamp = ts;
                        changed_touches.push_back(tp);
                    }
                }
                g_active_touches.clear();
                sel_name = "touchesCancelled:withEvent:";
            } else {
                int32_t pid = AMotionEvent_getPointerId(event, idx);
                double x_pt = (double)AMotionEvent_getX(event, idx) / scale;
                double y_pt = (double)AMotionEvent_getY(event, idx) / scale;
                auto it = g_active_touches.find(pid);
                if (it != g_active_touches.end() && it->second) {
                    auto *tp = static_cast<UITouchPayload *>(it->second);
                    tp->prev_x_pt = tp->x_pt;
                    tp->prev_y_pt = tp->y_pt;
                    tp->x_pt = x_pt;
                    tp->y_pt = y_pt;
                    tp->timestamp = ts;
                    tp->phase = 3; /* Ended */
                    changed_touches.push_back(tp);
                    g_active_touches.erase(it);
                    TOUCH_LOGI("Touch ENDED pid=%d pt=(%.1f, %.1f)", pid, x_pt, y_pt);
                }
                sel_name = "touchesEnded:withEvent:";
            }
        }

        for (auto &kv : g_active_touches) {
            if (kv.second) all_touches.push_back(kv.second);
        }
        for (void *ct : changed_touches) {
            bool present = false;
            for (void *at : all_touches) {
                if (at == ct) { present = true; break; }
            }
            if (!present) all_touches.push_back(ct);
        }
    }

    if (!sel_name || changed_touches.empty()) {
        return 1;
    }

    void *changed_set = oh2_objc_set_create(changed_touches.data(), changed_touches.size());
    void *all_set = oh2_objc_set_create(all_touches.data(), all_touches.size());
    auto *ev = static_cast<UIEventPayload *>(
        oh2_objc_alloc_instance(oh2_objc_get_class("UIEvent"), sizeof(UIEventPayload)));
    if (ev) {
        ev->all_touches_set = all_set;
        ev->timestamp = ts;
    }

    if (ios_view && g_touch_proxy) {
        auto *batch = new PendingTouchBatch{ios_view, oh2_objc_intern_selector(sel_name), changed_set, ev};
        oh2_perform_selector_on_main_thread(g_touch_proxy, "deliverTouch:", batch, 0);
    }
    return 1;
}

} // extern "C"
