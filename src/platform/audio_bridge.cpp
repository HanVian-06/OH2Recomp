#include "platform_bridges.h"
#include "../loader/macho_loader.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#define AUDIO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OH2_AUDIO", __VA_ARGS__)
#define AUDIO_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OH2_AUDIO", __VA_ARGS__)

namespace {

/* Apple AudioToolbox ABI Structures */
struct DarwinAudioComponentDescription {
    uint32_t componentType;
    uint32_t componentSubType;
    uint32_t componentManufacturer;
    uint32_t componentFlags;
    uint32_t componentFlagsMask;
};

struct DarwinAudioStreamBasicDescription {
    double   mSampleRate;
    uint32_t mFormatID;
    uint32_t mFormatFlags;
    uint32_t mBytesPerPacket;
    uint32_t mFramesPerPacket;
    uint32_t mBytesPerFrame;
    uint32_t mChannelsPerFrame;
    uint32_t mBitsPerChannel;
    uint32_t mReserved;
};

struct DarwinAudioBuffer {
    uint32_t mNumberChannels;
    uint32_t mDataByteSize;
    void    *mData;
};

struct DarwinAudioBufferList {
    uint32_t          mNumberBuffers;
    uint32_t          _pad;
    DarwinAudioBuffer mBuffers[2];
};

struct DarwinAudioTimeStamp {
    double   mSampleTime;
    uint64_t mHostTime;
    double   mRateScalar;
    uint64_t mWordClockTime;
    uint64_t mSMPTETime[3];
    uint32_t mFlags;
    uint32_t mReserved;
};

typedef int32_t (*DarwinAURenderCallback)(
    void *inRefCon,
    uint32_t *ioActionFlags,
    const DarwinAudioTimeStamp *inTimeStamp,
    uint32_t inBusNumber,
    uint32_t inNumberFrames,
    DarwinAudioBufferList *ioData);

struct DarwinAURenderCallbackStruct {
    DarwinAURenderCallback inputProc;
    void *inputProcRefCon;
};

struct FakeAudioUnit {
    uint32_t               node_id = 0;
    uint32_t               type = 0;
    uint32_t               sample_rate = 44100;
    uint32_t               channels = 1;
    bool                   is_float = false;
    bool                   is_non_interleaved = false;
    DarwinAURenderCallback cb = nullptr;
    void                  *cb_ref = nullptr;
};

struct FakeAUGraph {
    std::vector<FakeAudioUnit *> units;
    bool open = false;
    bool initialized = false;
    bool running = false;
};

static constexpr uint32_t kMaxAudioBuses = 128;

struct AudioBus {
    DarwinAURenderCallback cb = nullptr;
    void                  *ref = nullptr;
    uint32_t               sample_rate = 44100;
    uint32_t               channels = 1;
    bool                   is_float = false;
    bool                   is_non_interleaved = false;
    bool                   has_input_asbd = false;
    bool                   has_cb = false;
    bool                   enabled = true;
    float                  volume = 1.0f;
    float                  pan = 0.0f;
};

struct ActiveAudioBus {
    uint32_t               bus_idx = 0;
    DarwinAURenderCallback cb = nullptr;
    void                  *ref = nullptr;
    uint32_t               channels = 1;
    bool                   is_float = false;
    bool                   is_non_interleaved = false;
    float                  volume = 1.0f;
    float                  pan = 0.0f;
};

static std::mutex g_audio_mutex;
static AAudioStream *g_aaudio_stream = nullptr;
static std::atomic<bool> g_audio_active{false};
static std::atomic<bool> g_audio_paused{false};
static AudioBus g_buses[kMaxAudioBuses]{};
static float g_master_volume = 1.0f;
static uint32_t g_ue4_sample_rate = 44100;
static uint32_t g_aaudio_sample_rate = 44100;
static uint32_t g_last_requested_rate = 0;
static uint32_t g_active_channels = 2;
static uint64_t g_sample_cursor = 0;
static std::atomic<uint64_t> g_stream_gen{1};
static void *g_av_audio_session = nullptr;

/* Audio-callback-only static scratch & FIFO buffers (zero heap allocation in RT callback) */
static constexpr uint32_t kAudioChunkFrames = 256;
static constexpr size_t   kAudioFifoCap     = 16384;
static int16_t s_bus_i16[kAudioChunkFrames * 2]{};
static float   s_bus_f32[kAudioChunkFrames * 2]{};
static float   s_bus_f32_r[kAudioChunkFrames]{};
static float   s_mix_L[kAudioChunkFrames]{};
static float   s_mix_R[kAudioChunkFrames]{};
static float   s_fifo_L[kAudioFifoCap]{};
static float   s_fifo_R[kAudioFifoCap]{};
static size_t  s_fifo_count = 0;
static double  s_resample_pos = 0.0;
static uint64_t s_cb_stream_gen = 0;

static inline float clamp_audio(float x) {
    return (x < -1.0f) ? -1.0f : ((x > 1.0f) ? 1.0f : x);
}

static aaudio_data_callback_result_t aaudio_cb(AAudioStream *stream, void *user_data,
                                               void *audio_data, int32_t num_frames) {
    (void)stream; (void)user_data;
    if (!audio_data || num_frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    const uint64_t cur_gen = g_stream_gen.load(std::memory_order_acquire);
    if (s_cb_stream_gen != cur_gen) {
        s_fifo_count = 0;
        s_resample_pos = 0.0;
        s_cb_stream_gen = cur_gen;
    }

    const uint32_t out_ch = (g_active_channels >= 1 && g_active_channels <= 2) ? g_active_channels : 2u;
    float *out = static_cast<float *>(audio_data);
    std::memset(out, 0, static_cast<size_t>(num_frames) * out_ch * sizeof(float));
    if (g_audio_paused.load(std::memory_order_acquire)) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    ActiveAudioBus active[kMaxAudioBuses];
    uint32_t active_count = 0;
    uint32_t src_rate = 44100;
    float master_vol = 1.0f;
    {
        std::lock_guard<std::mutex> lk(g_audio_mutex);
        src_rate = (g_ue4_sample_rate == 44100 || g_ue4_sample_rate == 48000) ? g_ue4_sample_rate : 44100u;
        master_vol = g_master_volume;
        for (uint32_t i = 0; i < kMaxAudioBuses; ++i) {
            const AudioBus &b = g_buses[i];
            if (b.has_cb && b.cb && b.enabled) {
                ActiveAudioBus &ab = active[active_count++];
                ab.bus_idx = i;
                ab.cb = b.cb;
                ab.ref = b.ref;
                ab.channels = (b.channels == 2) ? 2u : 1u;
                ab.is_float = b.is_float;
                ab.is_non_interleaved = b.is_non_interleaved;
                ab.volume = b.volume;
                ab.pan = b.pan;
            }
        }
    }

    if (active_count == 0) {
        s_fifo_count = 0;
        s_resample_pos = 0.0;
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    const uint32_t out_rate = g_aaudio_sample_rate ? g_aaudio_sample_rate : 48000u;
    const double ratio = static_cast<double>(src_rate) / static_cast<double>(out_rate);

    int32_t written = 0;
    while (written < num_frames) {
        const int32_t rem = num_frames - written;
        const size_t max_out_per_pass = (src_rate == out_rate)
                                            ? kAudioFifoCap
                                            : static_cast<size_t>(static_cast<double>(kAudioFifoCap - 4) / std::max(0.5, ratio));
        const int32_t pass_frames = std::min<int32_t>(rem, static_cast<int32_t>(std::max<size_t>(1, max_out_per_pass)));
        const size_t needed = (src_rate == out_rate)
                                  ? static_cast<size_t>(pass_frames)
                                  : static_cast<size_t>(s_resample_pos + static_cast<double>(pass_frames) * ratio) + 2;

        while (s_fifo_count < needed && s_fifo_count + kAudioChunkFrames <= kAudioFifoCap) {
            std::memset(s_mix_L, 0, sizeof(s_mix_L));
            std::memset(s_mix_R, 0, sizeof(s_mix_R));

            DarwinAudioTimeStamp ts{};
            ts.mSampleTime = static_cast<double>(g_sample_cursor);
            ts.mRateScalar = 1.0;
            ts.mFlags = 1; /* kAudioTimeStampSampleTimeValid */
            g_sample_cursor += kAudioChunkFrames;

            for (uint32_t bi = 0; bi < active_count; ++bi) {
                const ActiveAudioBus &b = active[bi];
                uint32_t action_flags = 0;
                DarwinAudioBufferList abl{};
                int32_t cb_rc = -1;

                if (b.is_float && b.is_non_interleaved) {
                    std::memset(s_bus_f32, 0, kAudioChunkFrames * sizeof(float));
                    abl.mNumberBuffers = (b.channels >= 2) ? 2u : 1u;
                    abl.mBuffers[0] = {1u, kAudioChunkFrames * static_cast<uint32_t>(sizeof(float)), s_bus_f32};
                    if (b.channels >= 2) {
                        std::memset(s_bus_f32_r, 0, sizeof(s_bus_f32_r));
                        abl.mBuffers[1] = {1u, kAudioChunkFrames * static_cast<uint32_t>(sizeof(float)), s_bus_f32_r};
                    }
                    cb_rc = b.cb(b.ref, &action_flags, &ts, b.bus_idx, kAudioChunkFrames, &abl);
                } else if (b.is_float) {
                    const size_t nfloats = static_cast<size_t>(kAudioChunkFrames) * b.channels;
                    std::memset(s_bus_f32, 0, nfloats * sizeof(float));
                    abl.mNumberBuffers = 1u;
                    abl.mBuffers[0] = {b.channels, static_cast<uint32_t>(nfloats * sizeof(float)), s_bus_f32};
                    cb_rc = b.cb(b.ref, &action_flags, &ts, b.bus_idx, kAudioChunkFrames, &abl);
                } else {
                    const size_t nsamp = static_cast<size_t>(kAudioChunkFrames) * b.channels;
                    std::memset(s_bus_i16, 0, nsamp * sizeof(int16_t));
                    abl.mNumberBuffers = 1u;
                    abl.mBuffers[0] = {b.channels, static_cast<uint32_t>(nsamp * sizeof(int16_t)), s_bus_i16};
                    cb_rc = b.cb(b.ref, &action_flags, &ts, b.bus_idx, kAudioChunkFrames, &abl);
                }

                if (cb_rc != 0 || (action_flags & 0x10u) != 0) { /* kAudioUnitRenderAction_OutputIsSilence */
                    continue;
                }

                const float vol = b.volume;
                if (vol <= 0.0f) continue;

                if (b.channels == 1) {
                    const float gL = vol * std::min(1.0f, 1.0f - b.pan);
                    const float gR = vol * std::min(1.0f, 1.0f + b.pan);
                    if (b.is_float) {
                        for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                            const float s = s_bus_f32[i];
                            s_mix_L[i] += s * gL;
                            s_mix_R[i] += s * gR;
                        }
                    } else {
                        constexpr float kScale = 1.0f / 32768.0f;
                        for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                            const float s = static_cast<float>(s_bus_i16[i]) * kScale;
                            s_mix_L[i] += s * gL;
                            s_mix_R[i] += s * gR;
                        }
                    }
                } else {
                    if (b.is_float && b.is_non_interleaved) {
                        for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                            s_mix_L[i] += s_bus_f32[i] * vol;
                            s_mix_R[i] += s_bus_f32_r[i] * vol;
                        }
                    } else if (b.is_float) {
                        for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                            s_mix_L[i] += s_bus_f32[i * 2 + 0] * vol;
                            s_mix_R[i] += s_bus_f32[i * 2 + 1] * vol;
                        }
                    } else {
                        constexpr float kScale = 1.0f / 32768.0f;
                        for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                            s_mix_L[i] += static_cast<float>(s_bus_i16[i * 2 + 0]) * kScale * vol;
                            s_mix_R[i] += static_cast<float>(s_bus_i16[i * 2 + 1]) * kScale * vol;
                        }
                    }
                }
            }

            for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                s_fifo_L[s_fifo_count + i] = clamp_audio(s_mix_L[i] * master_vol);
                s_fifo_R[s_fifo_count + i] = clamp_audio(s_mix_R[i] * master_vol);
            }
            s_fifo_count += kAudioChunkFrames;
        }

        if (src_rate == out_rate) {
            const size_t avail = std::min<size_t>(s_fifo_count, static_cast<size_t>(pass_frames));
            for (size_t i = 0; i < avail; ++i) {
                if (out_ch == 1) {
                    out[written + i] = 0.5f * (s_fifo_L[i] + s_fifo_R[i]);
                } else {
                    out[(written + i) * 2 + 0] = s_fifo_L[i];
                    out[(written + i) * 2 + 1] = s_fifo_R[i];
                }
            }
            if (avail < s_fifo_count) {
                const size_t left = s_fifo_count - avail;
                std::memmove(s_fifo_L, s_fifo_L + avail, left * sizeof(float));
                std::memmove(s_fifo_R, s_fifo_R + avail, left * sizeof(float));
            }
            s_fifo_count -= avail;
        } else {
            for (int32_t i = 0; i < pass_frames; ++i) {
                if (s_fifo_count == 0) break;
                size_t idx = static_cast<size_t>(s_resample_pos);
                if (idx >= s_fifo_count) idx = s_fifo_count - 1;
                const size_t idx1 = (idx + 1 < s_fifo_count) ? (idx + 1) : idx;
                const float frac = static_cast<float>(s_resample_pos - static_cast<double>(idx));
                const float l = s_fifo_L[idx] + (s_fifo_L[idx1] - s_fifo_L[idx]) * frac;
                const float r = s_fifo_R[idx] + (s_fifo_R[idx1] - s_fifo_R[idx]) * frac;
                if (out_ch == 1) {
                    out[written + i] = 0.5f * (l + r);
                } else {
                    out[(written + i) * 2 + 0] = l;
                    out[(written + i) * 2 + 1] = r;
                }
                s_resample_pos += ratio;
            }
            const size_t consumed = std::min(s_fifo_count, static_cast<size_t>(s_resample_pos));
            if (consumed > 0) {
                const size_t left = s_fifo_count - consumed;
                if (left > 0) {
                    std::memmove(s_fifo_L, s_fifo_L + consumed, left * sizeof(float));
                    std::memmove(s_fifo_R, s_fifo_R + consumed, left * sizeof(float));
                }
                s_fifo_count = left;
                s_resample_pos -= static_cast<double>(consumed);
            }
        }
        written += pass_frames;
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static bool start_aaudio_locked(std::unique_lock<std::mutex> &lk, uint32_t req_rate) {
    if (g_audio_paused.load(std::memory_order_acquire)) {
        return true;
    }
    uint32_t rate = (req_rate == 44100 || req_rate == 48000) ? req_rate : 44100u;

    if (g_aaudio_stream && rate == g_last_requested_rate) {
        if (g_audio_active.load()) return true;
        AAudioStream *s = g_aaudio_stream;
        lk.unlock();
        aaudio_result_t r = AAudioStream_requestStart(s);
        lk.lock();
        if (r == AAUDIO_OK && g_aaudio_stream == s) {
            g_audio_active.store(true);
            return true;
        }
    }

    if (g_aaudio_stream) {
        AAudioStream *old_s = g_aaudio_stream;
        g_aaudio_stream = nullptr;
        g_audio_active.store(false);
        lk.unlock();
        AAudioStream_requestStop(old_s);
        AAudioStream_close(old_s);
        lk.lock();
    }

    AAudioStreamBuilder *builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
        AUDIO_LOGE("AAudio_createStreamBuilder failed");
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(rate));
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setDataCallback(builder, aaudio_cb, nullptr);

    AAudioStream *new_stream = nullptr;
    aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &new_stream);
    AAudioStreamBuilder_delete(builder);
    if (res != AAUDIO_OK || !new_stream) {
        AUDIO_LOGE("AAudioStreamBuilder_openStream failed: %d", res);
        return false;
    }

    g_aaudio_stream = new_stream;
    g_last_requested_rate = rate;
    g_aaudio_sample_rate = static_cast<uint32_t>(AAudioStream_getSampleRate(new_stream));
    g_active_channels = static_cast<uint32_t>(AAudioStream_getChannelCount(new_stream));
    g_stream_gen.fetch_add(1, std::memory_order_release);

    res = AAudioStream_requestStart(new_stream);
    if (res != AAUDIO_OK) {
        AUDIO_LOGE("AAudioStream_requestStart failed: %d", res);
        g_aaudio_stream = nullptr;
        lk.unlock();
        AAudioStream_close(new_stream);
        lk.lock();
        return false;
    }
    g_audio_active.store(true);
    AUDIO_LOGI("AAudio output stream started: req=%u Hz -> hw=%u Hz, %u ch (PCM_FLOAT), ue4_src=%u Hz",
               rate, g_aaudio_sample_rate, g_active_channels, g_ue4_sample_rate);
    return true;
}

/* ============================================================================
 * AudioToolbox C APIs
 * ============================================================================ */
static int32_t at_NewAUGraph(FakeAUGraph **outGraph) {
    if (!outGraph) return -50;
    *outGraph = new FakeAUGraph();
    return 0;
}

static int32_t at_DisposeAUGraph(FakeAUGraph *g) {
    if (!g) return 0;
    for (FakeAudioUnit *u : g->units) delete u;
    delete g;
    return 0;
}

static int32_t at_AUGraphOpen(FakeAUGraph *g) {
    if (g) g->open = true;
    return 0;
}

static int32_t at_AUGraphInitialize(FakeAUGraph *g) {
    if (g) g->initialized = true;
    std::unique_lock<std::mutex> lk(g_audio_mutex);
    start_aaudio_locked(lk, g_ue4_sample_rate);
    return 0;
}

static int32_t at_AUGraphAddNode(FakeAUGraph *g, const DarwinAudioComponentDescription *desc, int32_t *outNode) {
    if (!g || !outNode) return -50;
    auto *u = new FakeAudioUnit();
    u->node_id = static_cast<uint32_t>(g->units.size()) + 1;
    u->type = desc ? desc->componentType : 0;
    g->units.push_back(u);
    *outNode = static_cast<int32_t>(u->node_id);
    return 0;
}

static int32_t at_AUGraphNodeInfo(FakeAUGraph *g, int32_t node, DarwinAudioComponentDescription *outDesc, void **outAudioUnit) {
    (void)outDesc;
    if (!g || node <= 0 || static_cast<size_t>(node) > g->units.size()) return -50;
    if (outAudioUnit) *outAudioUnit = g->units[static_cast<size_t>(node - 1)];
    return 0;
}

static int32_t at_AUGraphConnectNodeInput(FakeAUGraph *g, int32_t srcNode, uint32_t srcBus, int32_t dstNode, uint32_t dstBus) {
    (void)g; (void)srcNode; (void)srcBus; (void)dstNode; (void)dstBus;
    return 0;
}

static int32_t at_AUGraphSetNodeInputCallback(FakeAUGraph *g, int32_t dstNode, uint32_t dstBus,
                                              const DarwinAURenderCallbackStruct *cb) {
    std::lock_guard<std::mutex> lk(g_audio_mutex);
    DarwinAURenderCallback proc = cb ? cb->inputProc : nullptr;
    void *ref = cb ? cb->inputProcRefCon : nullptr;
    if (dstBus < kMaxAudioBuses) {
        g_buses[dstBus].cb = proc;
        g_buses[dstBus].ref = ref;
        g_buses[dstBus].has_cb = (proc != nullptr);
    }
    if (g && dstNode > 0 && static_cast<size_t>(dstNode) <= g->units.size()) {
        g->units[static_cast<size_t>(dstNode - 1)]->cb = proc;
        g->units[static_cast<size_t>(dstNode - 1)]->cb_ref = ref;
    }
    static uint32_t s_cb_logs = 0;
    if (s_cb_logs < 8) {
        s_cb_logs++;
        AUDIO_LOGI("AUGraphSetNodeInputCallback node=%d bus=%u cb=%p", dstNode, dstBus, reinterpret_cast<void *>(proc));
    }
    return 0;
}

static int32_t at_AUGraphStart(FakeAUGraph *g) {
    if (g) g->running = true;
    std::unique_lock<std::mutex> lk(g_audio_mutex);
    start_aaudio_locked(lk, g_ue4_sample_rate);
    return 0;
}

static int32_t at_AUGraphStop(FakeAUGraph *g) {
    if (g) g->running = false;
    return 0;
}

static int32_t at_AudioUnitSetProperty(void *inUnit, uint32_t propID, uint32_t scope,
                                       uint32_t element, const void *inData, uint32_t inDataSize) {
    auto *u = static_cast<FakeAudioUnit *>(inUnit);
    if (!inData) return 0;

    std::unique_lock<std::mutex> lk(g_audio_mutex);
    if (propID == 8 && inDataSize >= sizeof(DarwinAudioStreamBasicDescription)) {
        /* kAudioUnitProperty_StreamFormat */
        const auto *asbd = static_cast<const DarwinAudioStreamBasicDescription *>(inData);
        uint32_t rate = (asbd->mSampleRate > 0.0) ? static_cast<uint32_t>(asbd->mSampleRate + 0.5) : 0u;
        uint32_t ch = (asbd->mChannelsPerFrame >= 1 && asbd->mChannelsPerFrame <= 2) ? asbd->mChannelsPerFrame : 1u;
        bool is_float = (asbd->mFormatFlags & 1U) != 0; /* kAudioFormatFlagIsFloat */
        bool is_non_int = (asbd->mFormatFlags & (1U << 5)) != 0; /* kAudioFormatFlagIsNonInterleaved */

        bool rate_changed = false;
        if (rate == 44100 || rate == 48000) {
            if (g_ue4_sample_rate != rate) rate_changed = true;
            g_ue4_sample_rate = rate;
        }
        if (scope == 1 && element < kMaxAudioBuses) { /* kAudioUnitScope_Input */
            AudioBus &b = g_buses[element];
            b.sample_rate = (rate == 44100 || rate == 48000) ? rate : g_ue4_sample_rate;
            b.channels = ch;
            b.is_float = is_float;
            b.is_non_interleaved = is_non_int;
            b.has_input_asbd = true;
        } else if (scope == 2 && element == 0 && !g_buses[0].has_input_asbd) { /* kAudioUnitScope_Output fallback */
            g_buses[0].sample_rate = (rate == 44100 || rate == 48000) ? rate : g_ue4_sample_rate;
            g_buses[0].channels = ch;
            g_buses[0].is_float = is_float;
            g_buses[0].is_non_interleaved = is_non_int;
        }
        if (u) {
            u->sample_rate = g_ue4_sample_rate;
            u->channels = ch;
            u->is_float = is_float;
            u->is_non_interleaved = is_non_int;
        }

        static uint32_t s_log_count = 0;
        static uint32_t s_last_scope = 0xFFFFFFFFu, s_last_rate = 0, s_last_ch = 0, s_last_bits = 0, s_last_flags = 0;
        if (s_log_count < 8 || scope != s_last_scope || rate != s_last_rate || ch != s_last_ch ||
            asbd->mBitsPerChannel != s_last_bits || asbd->mFormatFlags != s_last_flags) {
            s_log_count++;
            s_last_scope = scope;
            s_last_rate = rate;
            s_last_ch = ch;
            s_last_bits = asbd->mBitsPerChannel;
            s_last_flags = asbd->mFormatFlags;
            AUDIO_LOGI("AudioUnit ASBD: scope=%u elem=%u rate=%.0f fmt=0x%x flags=0x%x ch=%u bits=%u",
                       scope, element, asbd->mSampleRate, asbd->mFormatID, asbd->mFormatFlags,
                       asbd->mChannelsPerFrame, asbd->mBitsPerChannel);
        }

        if (g_aaudio_stream && rate_changed && g_ue4_sample_rate != g_last_requested_rate) {
            start_aaudio_locked(lk, g_ue4_sample_rate);
        }
    } else if (propID == 23 && inDataSize >= sizeof(DarwinAURenderCallbackStruct)) {
        /* kAudioUnitProperty_SetRenderCallback */
        const auto *cbs = static_cast<const DarwinAURenderCallbackStruct *>(inData);
        uint32_t bus = (element < kMaxAudioBuses) ? element : 0u;
        g_buses[bus].cb = cbs->inputProc;
        g_buses[bus].ref = cbs->inputProcRefCon;
        g_buses[bus].has_cb = (cbs->inputProc != nullptr);
        if (u) {
            u->cb = cbs->inputProc;
            u->cb_ref = cbs->inputProcRefCon;
        }
        AUDIO_LOGI("AudioUnitSetProperty SetRenderCallback scope=%u bus=%u cb=%p",
                   scope, bus, reinterpret_cast<void *>(cbs->inputProc));
    }
    return 0;
}

static int32_t at_AudioUnitSetParameter(void *inUnit, uint32_t paramID, uint32_t scope,
                                        uint32_t element, float value, uint32_t bufferOffset) {
    (void)inUnit; (void)bufferOffset;
    std::lock_guard<std::mutex> lk(g_audio_mutex);
    if (paramID == 0) { /* kMultiChannelMixerParam_Volume */
        float clamped = std::max(0.0f, std::min(2.0f, value));
        if (scope == 2) { /* Output */
            g_master_volume = clamped;
        } else if (scope == 1 && element < kMaxAudioBuses) {
            g_buses[element].volume = clamped;
        }
    } else if (paramID == 1) { /* kMultiChannelMixerParam_Enable */
        if (scope == 1 && element < kMaxAudioBuses) {
            g_buses[element].enabled = (value > 0.5f);
        }
    } else if (paramID == 2) { /* kMultiChannelMixerParam_Pan */
        if (scope == 1 && element < kMaxAudioBuses) {
            g_buses[element].pan = std::max(-1.0f, std::min(1.0f, value));
        }
    }
    return 0;
}

static int32_t at_AudioOutputUnitStart(void *ci) {
    (void)ci;
    std::unique_lock<std::mutex> lk(g_audio_mutex);
    start_aaudio_locked(lk, g_ue4_sample_rate);
    return 0;
}

static int32_t at_AudioOutputUnitStop(void *ci) {
    (void)ci;
    return 0;
}

static void at_AudioServicesPlaySystemSound(uint32_t inSystemSoundID) {
    (void)inSystemSoundID;
}

static const DarwinAudioStreamBasicDescription *cm_get_asbd(void *desc) {
    (void)desc;
    static DarwinAudioStreamBasicDescription s_asbd = {48000.0, 0x6c70636dU, 0x29U, 4, 1, 4, 2, 32, 0};
    return &s_asbd;
}

/* AVAudioSession ObjC bridge */
static void *avas_sharedInstance(void *cls, const char *sel) {
    (void)cls; (void)sel;
    if (!g_av_audio_session) {
        g_av_audio_session = oh2_objc_alloc_instance(oh2_objc_get_class("AVAudioSession"), 64);
        oh2_objc_mark_immortal(g_av_audio_session);
    }
    return g_av_audio_session;
}
static uint8_t avas_setCategory3(void *s, const char *sel, void *c, uint64_t o, void **e) {
    (void)s; (void)sel; (void)c; (void)o;
    if (e) *e = nullptr;
    return 1;
}
static uint8_t avas_setCategory2(void *s, const char *sel, void *c, void **e) {
    return avas_setCategory3(s, sel, c, 0, e);
}
static uint8_t avas_setCategory4(void *s, const char *sel, void *c, void *m, uint64_t o, void **e) {
    (void)m;
    return avas_setCategory3(s, sel, c, o, e);
}
static uint8_t avas_setActive(void *s, const char *sel, uint8_t active, void **e) {
    (void)s; (void)sel;
    if (e) *e = nullptr;
    if (active) oh2_platform_audio_ensure_active();
    return 1;
}
static uint8_t avas_setActiveOpts(void *s, const char *sel, uint8_t active, uint64_t opts, void **e) {
    (void)opts;
    return avas_setActive(s, sel, active, e);
}
static double avas_sampleRate(void *s, const char *sel) {
    (void)s; (void)sel;
    return static_cast<double>(g_ue4_sample_rate);
}
static uint8_t avas_setPrefRate(void *s, const char *sel, double rate, void **e) {
    (void)s; (void)sel;
    if (e) *e = nullptr;
    if (rate == 44100.0 || rate == 48000.0) {
        std::unique_lock<std::mutex> lk(g_audio_mutex);
        g_ue4_sample_rate = static_cast<uint32_t>(rate);
        if (g_aaudio_stream && g_ue4_sample_rate != g_last_requested_rate) {
            start_aaudio_locked(lk, g_ue4_sample_rate);
        }
    }
    return 1;
}
static uint8_t avas_setPrefDur(void *s, const char *sel, double dur, void **e) {
    (void)s; (void)sel; (void)dur;
    if (e) *e = nullptr;
    return 1;
}
static float avas_outputVolume(void *s, const char *sel) { (void)s; (void)sel; return 1.0f; }

} // namespace

extern "C" {

void oh2_platform_audio_register(void) {
    oh2_symbol_register_override("_NewAUGraph",                      (void *)&at_NewAUGraph);
    oh2_symbol_register_override("_DisposeAUGraph",                  (void *)&at_DisposeAUGraph);
    oh2_symbol_register_override("_AUGraphOpen",                     (void *)&at_AUGraphOpen);
    oh2_symbol_register_override("_AUGraphInitialize",               (void *)&at_AUGraphInitialize);
    oh2_symbol_register_override("_AUGraphAddNode",                  (void *)&at_AUGraphAddNode);
    oh2_symbol_register_override("_AUGraphNodeInfo",                 (void *)&at_AUGraphNodeInfo);
    oh2_symbol_register_override("_AUGraphConnectNodeInput",         (void *)&at_AUGraphConnectNodeInput);
    oh2_symbol_register_override("_AUGraphSetNodeInputCallback",     (void *)&at_AUGraphSetNodeInputCallback);
    oh2_symbol_register_override("_AUGraphStart",                    (void *)&at_AUGraphStart);
    oh2_symbol_register_override("_AUGraphStop",                     (void *)&at_AUGraphStop);
    oh2_symbol_register_override("_AudioUnitSetProperty",            (void *)&at_AudioUnitSetProperty);
    oh2_symbol_register_override("_AudioUnitSetParameter",           (void *)&at_AudioUnitSetParameter);
    oh2_symbol_register_override("_AudioOutputUnitStart",            (void *)&at_AudioOutputUnitStart);
    oh2_symbol_register_override("_AudioOutputUnitStop",             (void *)&at_AudioOutputUnitStop);
    oh2_symbol_register_override("_AudioServicesPlaySystemSound",    (void *)&at_AudioServicesPlaySystemSound);
    oh2_symbol_register_override("_CMAudioFormatDescriptionGetStreamBasicDescription", (void *)&cm_get_asbd);

    oh2_objc_register_class_method("AVAudioSession",    "sharedInstance",                      (void *)&avas_sharedInstance);
    oh2_objc_register_instance_method("AVAudioSession", "setCategory:error:",                  (void *)&avas_setCategory2);
    oh2_objc_register_instance_method("AVAudioSession", "setCategory:withOptions:error:",      (void *)&avas_setCategory3);
    oh2_objc_register_instance_method("AVAudioSession", "setCategory:mode:options:error:",     (void *)&avas_setCategory4);
    oh2_objc_register_instance_method("AVAudioSession", "setActive:error:",                    (void *)&avas_setActive);
    oh2_objc_register_instance_method("AVAudioSession", "setActive:withOptions:error:",        (void *)&avas_setActiveOpts);
    oh2_objc_register_instance_method("AVAudioSession", "sampleRate",                          (void *)&avas_sampleRate);
    oh2_objc_register_instance_method("AVAudioSession", "preferredSampleRate",                 (void *)&avas_sampleRate);
    oh2_objc_register_instance_method("AVAudioSession", "setPreferredSampleRate:error:",       (void *)&avas_setPrefRate);
    oh2_objc_register_instance_method("AVAudioSession", "setPreferredIOBufferDuration:error:", (void *)&avas_setPrefDur);
    oh2_objc_register_instance_method("AVAudioSession", "outputVolume",                        (void *)&avas_outputVolume);
}

void oh2_platform_audio_ensure_active(void) {
    std::unique_lock<std::mutex> lk(g_audio_mutex);
    start_aaudio_locked(lk, g_ue4_sample_rate);
}

void oh2_platform_audio_pause(void) {
    g_audio_paused.store(true, std::memory_order_release);
    AAudioStream *s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_audio_mutex);
        if (g_aaudio_stream && g_audio_active.load()) {
            g_audio_active.store(false);
            s = g_aaudio_stream;
        }
    }
    if (s) {
        AAudioStream_requestPause(s);
        AAudioStream_requestFlush(s);
        AUDIO_LOGI("AAudio stream paused (app backgrounded/unfocused)");
    }
}

void oh2_platform_audio_resume(void) {
    g_audio_paused.store(false, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(g_audio_mutex);
        start_aaudio_locked(lk, g_ue4_sample_rate);
    }
    AUDIO_LOGI("AAudio stream resumed");
}

void oh2_platform_audio_shutdown(void) {
    AAudioStream *s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_audio_mutex);
        g_audio_active.store(false);
        s = g_aaudio_stream;
        g_aaudio_stream = nullptr;
    }
    if (s) {
        AAudioStream_requestStop(s);
        AAudioStream_close(s);
    }
}

int oh2_platform_decode_ms_adpcm(const uint8_t *src, size_t src_bytes,
                                 int channels, int block_align,
                                 int16_t *dst_pcm, size_t max_samples) {
    static const int kAdaptTable[16] = {
        230, 230, 230, 230, 307, 409, 512, 614,
        768, 614, 512, 409, 307, 230, 230, 230
    };
    static const int kCoeff1[7] = {256, 512, 0, 192, 240, 460, 392};
    static const int kCoeff2[7] = {0, -256, 0, 64, 0, -208, -232};

    if (!src || !dst_pcm || (channels != 1 && channels != 2) || block_align < 7 * channels) {
        return -1;
    }

    size_t pos = 0;
    size_t out_i = 0;
    auto clamp16 = [](int v) -> int16_t {
        return (int16_t)std::max(-32768, std::min(32767, v));
    };
    auto read_i16 = [](const uint8_t *p) -> int16_t {
        return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    };

    while (pos + (size_t)block_align <= src_bytes && out_i + 2 * (size_t)channels <= max_samples) {
        const uint8_t *blk = src + pos;
        pos += (size_t)block_align;

        int pred[2]{}, delta[2]{}, s1[2]{}, s2[2]{};
        size_t hdr = 0;
        for (int c = 0; c < channels; ++c) pred[c] = blk[hdr++] % 7;
        for (int c = 0; c < channels; ++c) { delta[c] = read_i16(blk + hdr); hdr += 2; }
        for (int c = 0; c < channels; ++c) { s1[c]    = read_i16(blk + hdr); hdr += 2; }
        for (int c = 0; c < channels; ++c) { s2[c]    = read_i16(blk + hdr); hdr += 2; }

        for (int c = 0; c < channels && out_i < max_samples; ++c) dst_pcm[out_i++] = clamp16(s2[c]);
        for (int c = 0; c < channels && out_i < max_samples; ++c) dst_pcm[out_i++] = clamp16(s1[c]);

        size_t nibble_bytes = (size_t)block_align - hdr;
        int ch_turn = 0;
        for (size_t i = 0; i < nibble_bytes && out_i < max_samples; ++i) {
            uint8_t byte = blk[hdr + i];
            for (int nib_idx = 0; nib_idx < 2 && out_i < max_samples; ++nib_idx) {
                int c = (channels == 1) ? 0 : ch_turn;
                ch_turn ^= 1;
                int n = (nib_idx == 0) ? ((byte >> 4) & 0xF) : (byte & 0xF);
                int signed_n = (n & 8) ? (n - 16) : n;
                int predicted = (s1[c] * kCoeff1[pred[c]] + s2[c] * kCoeff2[pred[c]]) / 256;
                predicted += signed_n * delta[c];
                int16_t cur = clamp16(predicted);
                dst_pcm[out_i++] = cur;
                s2[c] = s1[c];
                s1[c] = cur;
                delta[c] = std::max(16, (delta[c] * kAdaptTable[n]) / 256);
            }
        }
    }
    return (int)out_i;
}

void oh2_platform_touch_register(void);
void oh2_platform_choreographer_shutdown(void);

int oh2_platform_init(void) {
    oh2_platform_touch_register();
    oh2_platform_audio_register();
    oh2_platform_audio_ensure_active();
    return 0;
}

void oh2_platform_shutdown(void) {
    oh2_platform_choreographer_shutdown();
    oh2_platform_audio_shutdown();
}

} // extern "C"
