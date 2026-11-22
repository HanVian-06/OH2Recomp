/*
 * framework_stubs.cpp — Safe offline inert stubs for non-core Apple frameworks
 * (GameKit, StoreKit, CloudKit, MultipeerConnectivity, UserNotifications, DeviceCheck,
 * AVFoundation, CoreMotion, Accessibility, Security) + post-load PLCrashReporter isolation.
 */

#include "oh2_runtime.h"
#include "../loader/macho_loader.h"

#include <cstring>

namespace {

struct BlockHeadStub {
    void *isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke2)(void *, void *);
};

struct BlockHeadStub3 {
    void *isa;
    int32_t flags;
    int32_t reserved;
    void (*invoke3)(void *, uint64_t, void *);
};

static void *g_gk_local_player   = nullptr;
static void *g_sk_payment_queue  = nullptr;
static void *g_ck_container      = nullptr;
static void *g_ck_database       = nullptr;
static void *g_dc_device         = nullptr;
static void *g_un_center         = nullptr;
static void *g_plcrash_reporter  = nullptr;

static uint8_t  stub_ret_no(void)  { return 0; }
static uint8_t  stub_ret_yes(void) { return 1; }
static void    *stub_ret_nil(void) { return nullptr; }
static void    *stub_ret_self(void *self) { return self; }
static int64_t  stub_kern_success(void) { return 0; }

/* GameKit */
static void *gk_localPlayer(void *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_gk_local_player) {
        g_gk_local_player = oh2_objc_alloc_instance(oh2_objc_get_class("GKLocalPlayer"), 64);
    }
    return g_gk_local_player;
}

static void gk_reportAchievements(void *cls, const char *sel, void *achs, void *handler) {
    (void)cls; (void)sel; (void)achs;
    if (handler) {
        BlockHeadStub *b = (BlockHeadStub *)handler;
        if (b->invoke2) b->invoke2(handler, nullptr);
    }
}

static void gk_score_report(void *self, const char *sel, void *handler) {
    (void)self; (void)sel;
    if (handler) {
        BlockHeadStub *b = (BlockHeadStub *)handler;
        if (b->invoke2) b->invoke2(handler, nullptr);
    }
}

static void gk_loadScores(void *self, const char *sel, void *handler) {
    (void)self; (void)sel;
    if (handler) {
        typedef void (*fn3_t)(void *, void *, void *);
        BlockHeadStub *b = (BlockHeadStub *)handler;
        if (b->invoke2) ((fn3_t)b->invoke2)(handler, nullptr, nullptr);
    }
}

/* StoreKit */
static void *sk_defaultQueue(void *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_sk_payment_queue) {
        g_sk_payment_queue = oh2_objc_alloc_instance(oh2_objc_get_class("SKPaymentQueue"), 64);
    }
    return g_sk_payment_queue;
}

static void *sk_paymentWithProduct(void *cls, const char *sel, void *prod) {
    (void)cls; (void)sel; (void)prod;
    return oh2_objc_alloc_instance(oh2_objc_get_class("SKPayment"), 64);
}

/* CloudKit */
static void *ck_defaultContainer(void *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_ck_container) {
        g_ck_container = oh2_objc_alloc_instance(oh2_objc_get_class("CKContainer"), 64);
    }
    return g_ck_container;
}

static void *ck_database(void *self, const char *sel) {
    (void)self; (void)sel;
    if (!g_ck_database) {
        g_ck_database = oh2_objc_alloc_instance(oh2_objc_get_class("CKDatabase"), 64);
    }
    return g_ck_database;
}

static void ck_accountStatus(void *self, const char *sel, void *handler) {
    (void)self; (void)sel;
    if (handler) {
        BlockHeadStub3 *b = (BlockHeadStub3 *)handler;
        if (b->invoke3) b->invoke3(handler, 3 /* CKAccountStatusNoAccount */, nullptr);
    }
}

static void ck_performQuery(void *self, const char *sel, void *q, void *zone, void *handler) {
    (void)self; (void)sel; (void)q; (void)zone;
    if (handler) {
        typedef void (*fn3_t)(void *, void *, void *);
        BlockHeadStub *b = (BlockHeadStub *)handler;
        if (b->invoke2) ((fn3_t)b->invoke2)(handler, nullptr, nullptr);
    }
}

static void *ck_record_initWithType(void *self, const char *sel, void *rtype) {
    (void)sel; (void)rtype;
    return self;
}

static void *ck_record_initWithTypeZone(void *self, const char *sel, void *rtype, void *rid) {
    (void)sel; (void)rtype; (void)rid;
    return self;
}

/* UserNotifications */
static void *un_currentCenter(void *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_un_center) {
        g_un_center = oh2_objc_alloc_instance(oh2_objc_get_class("UNUserNotificationCenter"), 64);
    }
    return g_un_center;
}

static void un_requestAuthorization(void *self, const char *sel, uint64_t opts, void *handler) {
    (void)self; (void)sel; (void)opts;
    if (handler) {
        BlockHeadStub3 *b = (BlockHeadStub3 *)handler;
        if (b->invoke3) b->invoke3(handler, 0, nullptr);
    }
}

/* DeviceCheck */
static void *dc_currentDevice(void *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_dc_device) {
        g_dc_device = oh2_objc_alloc_instance(oh2_objc_get_class("DCDevice"), 64);
    }
    return g_dc_device;
}

static void dc_generateToken(void *self, const char *sel, void *handler) {
    (void)self; (void)sel;
    if (handler) {
        typedef void (*fn3_t)(void *, void *, void *);
        BlockHeadStub *b = (BlockHeadStub *)handler;
        if (b->invoke2) ((fn3_t)b->invoke2)(handler, nullptr, nullptr);
    }
}

/* MultipeerConnectivity */
static void *mc_initWithDisplayName(void *self, const char *sel, void *name) {
    (void)sel; (void)name;
    return self;
}
static void *mc_session_initWithPeer(void *self, const char *sel, void *peer) {
    (void)sel; (void)peer;
    return self;
}

/* PLCrashReporter (safe non-Mach-exception-port overrides applied after macho_load) */
static void *plcrash_sharedReporter(void *cls, const char *sel) {
    (void)sel;
    if (!g_plcrash_reporter) {
        g_plcrash_reporter = oh2_objc_alloc_instance(cls, 128);
    }
    return g_plcrash_reporter;
}

static uint8_t plcrash_enableAndReturnError(void *self, const char *sel, void **out_error) {
    (void)self; (void)sel;
    if (out_error) *out_error = nullptr;
    return 1; /* YES, safely initialized without Mach ports */
}

} // namespace

extern "C" void oh2_m2_register_framework_stubs(void) {
    /* GameKit */
    oh2_objc_register_class_method("GKLocalPlayer",    "localPlayer",                            (void *)&gk_localPlayer);
    oh2_objc_register_instance_method("GKLocalPlayer", "isAuthenticated",                        (void *)&stub_ret_no);
    oh2_objc_register_instance_method("GKLocalPlayer", "setAuthenticateHandler:",                (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("GKLocalPlayer", "playerID",                               (void *)&stub_ret_nil);
    oh2_objc_register_class_method("GKAchievement",    "reportAchievements:withCompletionHandler:", (void *)&gk_reportAchievements);
    oh2_objc_register_class_method("GKAchievement",    "resetAchievementsWithCompletionHandler:", (void *)&gk_reportAchievements);
    oh2_objc_register_instance_method("GKLeaderboard", "loadScoresWithCompletionHandler:",       (void *)&gk_loadScores);
    oh2_objc_register_instance_method("GKScore",       "reportScoreWithCompletionHandler:",      (void *)&gk_score_report);
    oh2_objc_register_instance_method("GKTurnBasedMatch", "matchID",                             (void *)&stub_ret_nil);

    /* StoreKit */
    oh2_objc_register_class_method("SKPaymentQueue",    "defaultQueue",              (void *)&sk_defaultQueue);
    oh2_objc_register_class_method("SKPaymentQueue",    "canMakePayments",           (void *)&stub_ret_no);
    oh2_objc_register_instance_method("SKPaymentQueue", "addTransactionObserver:",   (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("SKPaymentQueue", "removeTransactionObserver:",(void *)&stub_ret_nil);
    oh2_objc_register_instance_method("SKPaymentQueue", "addPayment:",               (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("SKPaymentQueue", "finishTransaction:",        (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("SKPaymentQueue", "restoreCompletedTransactions", (void *)&stub_ret_nil);
    oh2_objc_register_class_method("SKPayment",         "paymentWithProduct:",       (void *)&sk_paymentWithProduct);
    oh2_objc_register_instance_method("SKProductsRequest", "initWithProductIdentifiers:", (void *)&stub_ret_self);
    oh2_objc_register_instance_method("SKProductsRequest", "start",                  (void *)&stub_ret_nil);
    oh2_objc_register_class_method("SKStoreReviewController", "requestReview",       (void *)&stub_ret_nil);

    /* CloudKit */
    oh2_objc_register_class_method("CKContainer",    "defaultContainer",                     (void *)&ck_defaultContainer);
    oh2_objc_register_instance_method("CKContainer", "publicCloudDatabase",                  (void *)&ck_database);
    oh2_objc_register_instance_method("CKContainer", "privateCloudDatabase",                 (void *)&ck_database);
    oh2_objc_register_instance_method("CKContainer", "accountStatusWithCompletionHandler:",  (void *)&ck_accountStatus);
    oh2_objc_register_instance_method("CKDatabase",  "performQuery:inZoneWithID:completionHandler:", (void *)&ck_performQuery);
    oh2_objc_register_instance_method("CKDatabase",  "addOperation:",                        (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("CKRecord",    "initWithRecordType:",                  (void *)&ck_record_initWithType);
    oh2_objc_register_instance_method("CKRecord",    "initWithRecordType:recordID:",         (void *)&ck_record_initWithTypeZone);
    oh2_objc_register_instance_method("CKRecord",    "objectForKey:",                        (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("CKRecord",    "setObject:forKey:",                    (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("CKRecordID",  "initWithRecordName:",                  (void *)&ck_record_initWithType);
    oh2_objc_register_instance_method("CKQuery",     "initWithRecordType:predicate:",        (void *)&ck_record_initWithTypeZone);

    /* MultipeerConnectivity */
    oh2_objc_register_instance_method("MCPeerID",  "initWithDisplayName:", (void *)&mc_initWithDisplayName);
    oh2_objc_register_instance_method("MCSession", "initWithPeer:",        (void *)&mc_session_initWithPeer);
    oh2_objc_register_instance_method("MCSession", "disconnect",           (void *)&stub_ret_nil);

    /* UserNotifications */
    oh2_objc_register_class_method("UNUserNotificationCenter",    "currentNotificationCenter", (void *)&un_currentCenter);
    oh2_objc_register_instance_method("UNUserNotificationCenter", "setDelegate:",              (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("UNUserNotificationCenter", "requestAuthorizationWithOptions:completionHandler:", (void *)&un_requestAuthorization);
    oh2_objc_register_instance_method("UNUserNotificationCenter", "addNotificationRequest:withCompletionHandler:", (void *)&gk_score_report);

    /* DeviceCheck */
    oh2_objc_register_class_method("DCDevice",    "currentDevice",                  (void *)&dc_currentDevice);
    oh2_objc_register_instance_method("DCDevice", "isSupported",                    (void *)&stub_ret_no);
    oh2_objc_register_instance_method("DCDevice", "generateTokenWithCompletionHandler:", (void *)&dc_generateToken);

    /* AVFoundation & NSNotificationCenter no-crash stubs */
    oh2_objc_register_instance_method("AVAudioSession",       "isOtherAudioPlaying", (void *)&stub_ret_no);
    oh2_objc_register_instance_method("AVAudioSession",       "setCategory:withOptions:error:", (void *)&stub_ret_yes);
    oh2_objc_register_instance_method("NSNotificationCenter", "addObserver:selector:name:object:", (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("NSNotificationCenter", "addObserverForName:object:queue:usingBlock:", (void *)&stub_ret_nil);
    oh2_objc_register_instance_method("NSNotificationCenter", "removeObserver:", (void *)&stub_ret_nil);

    /* Mach exception-port functions neutralized so PLCrashReporter can never trigger Mach IPC */
    oh2_symbol_register_override("_exception_raise",                 (void *)&stub_kern_success);
    oh2_symbol_register_override("_exception_raise_state",           (void *)&stub_kern_success);
    oh2_symbol_register_override("_exception_raise_state_identity",  (void *)&stub_kern_success);
    oh2_symbol_register_override("_mach_port_allocate",              (void *)&stub_kern_success);
    oh2_symbol_register_override("_mach_port_deallocate",            (void *)&stub_kern_success);
    oh2_symbol_register_override("_mach_port_insert_right",          (void *)&stub_kern_success);
    oh2_symbol_register_override("_mach_port_mod_refs",              (void *)&stub_kern_success);
    oh2_symbol_register_override("_mach_port_move_member",           (void *)&stub_kern_success);
    oh2_symbol_register_override("_mach_port_request_notification",  (void *)&stub_kern_success);
    oh2_symbol_register_override("_mach_msg",                        (void *)&stub_kern_success);
}

extern "C" void oh2_runtime_post_image_overrides(void) {
    /* PLCrashReporter lives inside OH2's 60 local ObjC classes (available after macho_load) */
    oh2_objc_register_class_method("PLCrashReporter",    "sharedReporter",                     (void *)&plcrash_sharedReporter);
    oh2_objc_register_instance_method("PLCrashReporter", "enableCrashReporterAndReturnError:", (void *)&plcrash_enableAndReturnError);
    oh2_objc_register_instance_method("PLCrashReporter", "enableCrashReporter",                (void *)&stub_ret_yes);
    oh2_objc_register_instance_method("PLCrashReporter", "hasPendingCrashReport",              (void *)&stub_ret_no);
    oh2_objc_register_instance_method("PLCrashReporter", "purgePendingCrashReportAndReturnError:", (void *)&plcrash_enableAndReturnError);
}
