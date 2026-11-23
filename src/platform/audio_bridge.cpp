#include "platform_bridges.h"
#include "../loader/macho_loader.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
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

static constexpr uint32_t kSubType3DMixerEmbedded  = 0x3364656dU; /* '3dem' */
static constexpr uint32_t kSubTypeMultiChannelMix  = 0x6d636d78U; /* 'mcmx' */

struct FakeAudioUnit {
    uint32_t               node_id = 0;
    uint32_t               type = 0;
    uint32_t               sub_type = 0;
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
    bool                   enabled = false;
    bool                   is_3d_mixer = true;
    float                  volume = 0.0f;       /* linear gain (from dB for AU3DMixerEmbedded) */
    float                  pan_L = 0.70710678f; /* constant-power left gain */
    float                  pan_R = 0.70710678f; /* constant-power right gain */
    float                  playback_rate = 1.0f;
    uint64_t               reset_gen = 1;
};

struct ActiveAudioBus {
    uint32_t               bus_idx = 0;
    DarwinAURenderCallback cb = nullptr;
    void                  *ref = nullptr;
    uint32_t               sample_rate = 44100;
    uint32_t               channels = 1;
    bool                   is_float = false;
    bool                   is_non_interleaved = false;
    float                  volume = 0.0f;
    float                  pan_L = 0.70710678f;
    float                  pan_R = 0.70710678f;
    float                  playback_rate = 1.0f;
    uint64_t               reset_gen = 0;
};

/* Per-bus resampling state owned exclusively by the audio pump thread */
static constexpr uint32_t kAudioChunkFrames     = 512;
static constexpr size_t   kBusFifoCap           = 4096;
static constexpr size_t   kRingCap              = 16384; /* power of 2 */
static constexpr size_t   kRingMask             = kRingCap - 1;
static constexpr size_t   kTargetBufferedFrames = 4096;  /* ~85 ms cushion */

struct BusResamplerState {
    float    fifo_L[kBusFifoCap]{};
    float    fifo_R[kBusFifoCap]{};
    size_t   fifo_count = 0;
    double   pos = 0.0;
    uint64_t last_reset_gen = 0;
};

static std::mutex g_audio_mutex;
static AAudioStream *g_aaudio_stream = nullptr;
static std::atomic<bool> g_audio_active{false};
static std::atomic<bool> g_audio_paused{false};
static AudioBus g_buses[kMaxAudioBuses]{};
static bool g_graph_uses_3d_mixer = true;
static float g_master_volume = 1.0f;
static uint32_t g_ue4_sample_rate = 44100;
static uint32_t g_aaudio_sample_rate = 44100;
static uint32_t g_last_requested_rate = 0;
static uint32_t g_active_channels = 2;
static uint64_t g_sample_cursor = 0;
static std::atomic<uint64_t> g_stream_gen{1};
static void *g_av_audio_session = nullptr;

static int16_t           s_bus_i16[kAudioChunkFrames * 2]{};
static float             s_bus_f32[kAudioChunkFrames * 2]{};
static float             s_bus_f32_r[kAudioChunkFrames]{};
static float             s_mix_L[kAudioChunkFrames]{};
static float             s_mix_R[kAudioChunkFrames]{};
static BusResamplerState s_bus_rs[kMaxAudioBuses]{};
static uint64_t          s_pump_stream_gen = 0;

static float                 s_ring_L[kRingCap]{};
static float                 s_ring_R[kRingCap]{};
static std::atomic<uint64_t> s_ring_write{0};
static std::atomic<uint64_t> s_ring_read{0};
static std::atomic<bool>     g_pump_running{false};
static std::atomic<bool>     g_pump_stop{false};
static std::thread           g_pump_thread;

static inline float soft_limit_audio(float x) {
    if (x > 0.92f) {
        return 0.92f + 0.08f * std::tanh((x - 0.92f) * 12.5f);
    }
    if (x < -0.92f) {
        return -0.92f + 0.08f * std::tanh((x + 0.92f) * 12.5f);
    }
    return x;
}

static inline void set_bus_pan_from_norm(AudioBus &b, float pan_norm) {
    /* pan_norm in [-1.0 (hard Left), +1.0 (hard Right)], constant-power pan law */
    float p = std::max(-1.0f, std::min(1.0f, pan_norm));
    constexpr float kQuarterPi = 0.7853981633974483f;
    float angle = (p + 1.0f) * kQuarterPi;
    b.pan_L = std::cos(angle);
    b.pan_R = std::sin(angle);
}

static inline float db_to_linear(float db) {
    if (db <= -100.0f) return 0.0f;
    float clamped_db = std::min(20.0f, db);
    return std::pow(10.0f, clamped_db * 0.05f);
}

static bool pull_bus_chunk(const ActiveAudioBus &b, const DarwinAudioTimeStamp &ts,
                           float *out_L, float *out_R) {
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

    if (cb_rc != 0 || (action_flags & 0x10u) != 0) {
        return false;
    }

    if (b.channels == 1) {
        if (b.is_float) {
            std::memcpy(out_L, s_bus_f32, kAudioChunkFrames * sizeof(float));
        } else {
            constexpr float kScale = 1.0f / 32768.0f;
            for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                out_L[i] = static_cast<float>(s_bus_i16[i]) * kScale;
            }
        }
    } else {
        if (b.is_float && b.is_non_interleaved) {
            std::memcpy(out_L, s_bus_f32, kAudioChunkFrames * sizeof(float));
            std::memcpy(out_R, s_bus_f32_r, kAudioChunkFrames * sizeof(float));
        } else if (b.is_float) {
            for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                out_L[i] = s_bus_f32[i * 2 + 0];
                out_R[i] = s_bus_f32[i * 2 + 1];
            }
        } else {
            constexpr float kScale = 1.0f / 32768.0f;
            for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                out_L[i] = static_cast<float>(s_bus_i16[i * 2 + 0]) * kScale;
                out_R[i] = static_cast<float>(s_bus_i16[i * 2 + 1]) * kScale;
            }
        }
    }
    return true;
}

static void audio_pump_thread_fn() {
    while (!g_pump_stop.load(std::memory_order_acquire)) {
        if (g_audio_paused.load(std::memory_order_acquire) || !g_audio_active.load(std::memory_order_acquire)) {
            usleep(4000);
            continue;
        }

        const uint64_t cur_gen = g_stream_gen.load(std::memory_order_acquire);
        if (s_pump_stream_gen != cur_gen) {
            for (uint32_t i = 0; i < kMaxAudioBuses; ++i) {
                s_bus_rs[i].fifo_count = 0;
                s_bus_rs[i].pos = 0.0;
            }
            s_ring_read.store(0, std::memory_order_relaxed);
            s_ring_write.store(0, std::memory_order_release);
            s_pump_stream_gen = cur_gen;
        }

        const uint64_t w_pos = s_ring_write.load(std::memory_order_relaxed);
        const uint64_t r_pos = s_ring_read.load(std::memory_order_acquire);
        const size_t buffered = (w_pos >= r_pos) ? static_cast<size_t>(w_pos - r_pos) : 0;
        if (buffered >= kTargetBufferedFrames) {
            usleep(2000);
            continue;
        }

        ActiveAudioBus active[kMaxAudioBuses];
        uint32_t active_count = 0;
        float master_vol = 1.0f;
        {
            std::lock_guard<std::mutex> lk(g_audio_mutex);
            master_vol = g_master_volume;
            for (uint32_t i = 0; i < kMaxAudioBuses; ++i) {
                const AudioBus &b = g_buses[i];
                if (b.has_cb && b.cb && b.enabled) {
                    ActiveAudioBus &ab = active[active_count++];
                    ab.bus_idx = i;
                    ab.cb = b.cb;
                    ab.ref = b.ref;
                    ab.sample_rate = (b.sample_rate >= 8000 && b.sample_rate <= 96000) ? b.sample_rate : 44100u;
                    ab.channels = (b.channels == 2) ? 2u : 1u;
                    ab.is_float = b.is_float;
                    ab.is_non_interleaved = b.is_non_interleaved;
                    ab.volume = b.volume;
                    ab.pan_L = b.pan_L;
                    ab.pan_R = b.pan_R;
                    ab.playback_rate = b.playback_rate;
                    ab.reset_gen = b.reset_gen;
                }
            }
        }

        const uint32_t out_rate = g_aaudio_sample_rate ? g_aaudio_sample_rate : 44100u;
        const size_t free_space = (buffered < kRingCap) ? (kRingCap - buffered - 1) : 0;
        if (free_space < kAudioChunkFrames) {
            usleep(1000);
            continue;
        }

        if (active_count == 0) {
            /* Write silence into ring so AAudio output remains smooth without stale artifacts */
            uint64_t cur_w = w_pos;
            for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                const size_t slot = static_cast<size_t>(cur_w & kRingMask);
                s_ring_L[slot] = 0.0f;
                s_ring_R[slot] = 0.0f;
                cur_w++;
            }
            s_ring_write.store(cur_w, std::memory_order_release);
            usleep(2000);
            continue;
        }

        std::memset(s_mix_L, 0, sizeof(s_mix_L));
        std::memset(s_mix_R, 0, sizeof(s_mix_R));

        DarwinAudioTimeStamp ts{};
        ts.mSampleTime = static_cast<double>(g_sample_cursor);
        ts.mRateScalar = 1.0;
        ts.mFlags = 1; /* kAudioTimeStampSampleTimeValid */
        g_sample_cursor += kAudioChunkFrames;

        static float s_tmp_L[kAudioChunkFrames];
        static float s_tmp_R[kAudioChunkFrames];

        for (uint32_t bi = 0; bi < active_count; ++bi) {
            const ActiveAudioBus &b = active[bi];
            BusResamplerState &rs = s_bus_rs[b.bus_idx];
            if (rs.last_reset_gen != b.reset_gen) {
                rs.fifo_count = 0;
                rs.pos = 0.0;
                rs.last_reset_gen = b.reset_gen;
            }

            const double eff_rate = static_cast<double>(b.sample_rate) * static_cast<double>(b.playback_rate);
            const double step = eff_rate / static_cast<double>(out_rate);

            /* Fast 1:1 path when bus rate * pitch matches AAudio hardware rate */
            if (std::fabs(step - 1.0) < 0.002 && rs.fifo_count == 0) {
                if (!pull_bus_chunk(b, ts, s_tmp_L, s_tmp_R)) continue;
                const float vol = b.volume;
                if (vol <= 0.00001f) continue;
                if (b.channels == 1) {
                    const float gL = vol * b.pan_L;
                    const float gR = vol * b.pan_R;
                    for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                        const float s = s_tmp_L[i];
                        s_mix_L[i] += s * gL;
                        s_mix_R[i] += s * gR;
                    }
                } else {
                    for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                        s_mix_L[i] += s_tmp_L[i] * vol;
                        s_mix_R[i] += s_tmp_R[i] * vol;
                    }
                }
                continue;
            }

            /* Per-bus resampled path (accurate sample rate conversion + dynamic pitch shift) */
            const size_t needed = static_cast<size_t>(rs.pos + static_cast<double>(kAudioChunkFrames) * step) + 2;
            bool bus_silent = false;
            while (rs.fifo_count < needed && rs.fifo_count + kAudioChunkFrames <= kBusFifoCap) {
                if (!pull_bus_chunk(b, ts, rs.fifo_L + rs.fifo_count, rs.fifo_R + rs.fifo_count)) {
                    bus_silent = true;
                    break;
                }
                rs.fifo_count += kAudioChunkFrames;
            }
            if (rs.fifo_count == 0) continue;

            const float vol = b.volume;
            if (vol > 0.00001f) {
                if (b.channels == 1) {
                    const float gL = vol * b.pan_L;
                    const float gR = vol * b.pan_R;
                    double p = rs.pos;
                    for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                        size_t idx = static_cast<size_t>(p);
                        if (idx >= rs.fifo_count) idx = rs.fifo_count - 1;
                        const size_t idx1 = (idx + 1 < rs.fifo_count) ? (idx + 1) : idx;
                        const float frac = static_cast<float>(p - static_cast<double>(idx));
                        const float s = rs.fifo_L[idx] + (rs.fifo_L[idx1] - rs.fifo_L[idx]) * frac;
                        s_mix_L[i] += s * gL;
                        s_mix_R[i] += s * gR;
                        p += step;
                    }
                    rs.pos = p;
                } else {
                    double p = rs.pos;
                    for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
                        size_t idx = static_cast<size_t>(p);
                        if (idx >= rs.fifo_count) idx = rs.fifo_count - 1;
                        const size_t idx1 = (idx + 1 < rs.fifo_count) ? (idx + 1) : idx;
                        const float frac = static_cast<float>(p - static_cast<double>(idx));
                        const float l = rs.fifo_L[idx] + (rs.fifo_L[idx1] - rs.fifo_L[idx]) * frac;
                        const float r = rs.fifo_R[idx] + (rs.fifo_R[idx1] - rs.fifo_R[idx]) * frac;
                        s_mix_L[i] += l * vol;
                        s_mix_R[i] += r * vol;
                        p += step;
                    }
                    rs.pos = p;
                }
            } else {
                rs.pos += static_cast<double>(kAudioChunkFrames) * step;
            }

            const size_t consumed = std::min(rs.fifo_count, static_cast<size_t>(rs.pos));
            if (consumed > 0) {
                const size_t left = rs.fifo_count - consumed;
                if (left > 0) {
                    std::memmove(rs.fifo_L, rs.fifo_L + consumed, left * sizeof(float));
                    if (b.channels == 2) {
                        std::memmove(rs.fifo_R, rs.fifo_R + consumed, left * sizeof(float));
                    }
                }
                rs.fifo_count = left;
                rs.pos -= static_cast<double>(consumed);
            }
            if (bus_silent && rs.fifo_count == 0) {
                rs.pos = 0.0;
            }
        }

        uint64_t cur_w = w_pos;
        for (uint32_t i = 0; i < kAudioChunkFrames; ++i) {
            const size_t slot = static_cast<size_t>(cur_w & kRingMask);
            s_ring_L[slot] = soft_limit_audio(s_mix_L[i] * master_vol);
            s_ring_R[slot] = soft_limit_audio(s_mix_R[i] * master_vol);
            cur_w++;
        }
        s_ring_write.store(cur_w, std::memory_order_release);
    }
}

static void ensure_pump_thread_started() {
    bool expected = false;
    if (g_pump_running.compare_exchange_strong(expected, true)) {
        g_pump_stop.store(false, std::memory_order_release);
        g_pump_thread = std::thread(audio_pump_thread_fn);
    }
}

static aaudio_data_callback_result_t aaudio_cb(AAudioStream *stream, void *user_data,
                                               void *audio_data, int32_t num_frames) {
    (void)stream; (void)user_data;
    if (!audio_data || num_frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    const uint32_t out_ch = (g_active_channels >= 1 && g_active_channels <= 2) ? g_active_channels : 2u;
    float *out = static_cast<float *>(audio_data);
    std::memset(out, 0, static_cast<size_t>(num_frames) * out_ch * sizeof(float));
    if (g_audio_paused.load(std::memory_order_acquire)) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    const uint64_t w_pos = s_ring_write.load(std::memory_order_acquire);
    uint64_t r_pos = s_ring_read.load(std::memory_order_relaxed);
    const size_t avail = (w_pos >= r_pos) ? static_cast<size_t>(w_pos - r_pos) : 0;
    const size_t to_read = std::min<size_t>(avail, static_cast<size_t>(num_frames));

    for (size_t i = 0; i < to_read; ++i) {
        const size_t slot = static_cast<size_t>((r_pos + i) & kRingMask);
        if (out_ch == 1) {
            out[i] = 0.5f * (s_ring_L[slot] + s_ring_R[slot]);
        } else {
            out[i * 2 + 0] = s_ring_L[slot];
            out[i * 2 + 1] = s_ring_R[slot];
        }
    }
    s_ring_read.store(r_pos + to_read, std::memory_order_release);
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
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, 4096);
    AAudioStreamBuilder_setDataCallback(builder, aaudio_cb, nullptr);

    AAudioStream *new_stream = nullptr;
    aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &new_stream);
    AAudioStreamBuilder_delete(builder);
    if (res != AAUDIO_OK || !new_stream) {
        AUDIO_LOGE("AAudioStreamBuilder_openStream failed: %d", res);
        return false;
    }

    int32_t burst = AAudioStream_getFramesPerBurst(new_stream);
    if (burst > 0) {
        AAudioStream_setBufferSizeInFrames(new_stream, std::max<int32_t>(1024, burst * 4));
    }

    g_aaudio_stream = new_stream;
    g_last_requested_rate = rate;
    g_aaudio_sample_rate = static_cast<uint32_t>(AAudioStream_getSampleRate(new_stream));
    g_active_channels = static_cast<uint32_t>(AAudioStream_getChannelCount(new_stream));
    g_stream_gen.fetch_add(1, std::memory_order_release);
    ensure_pump_thread_started();

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
    u->sub_type = desc ? desc->componentSubType : 0;
    g->units.push_back(u);
    *outNode = static_cast<int32_t>(u->node_id);

    if (u->sub_type == kSubType3DMixerEmbedded) {
        std::lock_guard<std::mutex> lk(g_audio_mutex);
        g_graph_uses_3d_mixer = true;
    } else if (u->sub_type == kSubTypeMultiChannelMix) {
        std::lock_guard<std::mutex> lk(g_audio_mutex);
        g_graph_uses_3d_mixer = false;
    }
    AUDIO_LOGI("AUGraphAddNode node=%u type=0x%x sub=0x%x (3d_mixer=%d)",
               u->node_id, u->type, u->sub_type, g_graph_uses_3d_mixer ? 1 : 0);
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
    bool is_3d = g_graph_uses_3d_mixer;
    if (g && dstNode > 0 && static_cast<size_t>(dstNode) <= g->units.size()) {
        FakeAudioUnit *u = g->units[static_cast<size_t>(dstNode - 1)];
        u->cb = proc;
        u->cb_ref = ref;
        if (u->sub_type == kSubType3DMixerEmbedded) is_3d = true;
        else if (u->sub_type == kSubTypeMultiChannelMix) is_3d = false;
    }
    if (dstBus < kMaxAudioBuses) {
        AudioBus &b = g_buses[dstBus];
        b.cb = proc;
        b.ref = ref;
        b.has_cb = (proc != nullptr);
        b.is_3d_mixer = is_3d;
        /* For AU3DMixerEmbedded, UE4 explicitly enables buses via k3DMixerParam_Enable (5) on Play().
           For non-3D mixers, default enabled=true, volume=1.0f. */
        if (!is_3d && proc != nullptr) {
            b.enabled = true;
            if (b.volume <= 0.0f) b.volume = 1.0f;
        }
        b.reset_gen++;
    }
    static uint32_t s_cb_logs = 0;
    if (s_cb_logs < 8) {
        s_cb_logs++;
        AUDIO_LOGI("AUGraphSetNodeInputCallback node=%d bus=%u cb=%p (3d=%d)",
                   dstNode, dstBus, reinterpret_cast<void *>(proc), is_3d ? 1 : 0);
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
    if (propID == 2 && inDataSize >= sizeof(double)) {
        /* kAudioUnitProperty_SampleRate */
        double sr = *static_cast<const double *>(inData);
        uint32_t rate = (sr > 0.0) ? static_cast<uint32_t>(sr + 0.5) : 0u;
        if (scope == 2 && (rate == 44100 || rate == 48000)) {
            bool changed = (g_ue4_sample_rate != rate);
            g_ue4_sample_rate = rate;
            if (g_aaudio_stream && changed && g_ue4_sample_rate != g_last_requested_rate) {
                start_aaudio_locked(lk, g_ue4_sample_rate);
            }
        } else if (scope == 1 && element < kMaxAudioBuses && rate >= 8000 && rate <= 96000) {
            g_buses[element].sample_rate = rate;
        }
    } else if (propID == 8 && inDataSize >= sizeof(DarwinAudioStreamBasicDescription)) {
        /* kAudioUnitProperty_StreamFormat */
        const auto *asbd = static_cast<const DarwinAudioStreamBasicDescription *>(inData);
        uint32_t rate = (asbd->mSampleRate > 0.0) ? static_cast<uint32_t>(asbd->mSampleRate + 0.5) : 0u;
        uint32_t ch = (asbd->mChannelsPerFrame >= 1 && asbd->mChannelsPerFrame <= 2) ? asbd->mChannelsPerFrame : 1u;
        bool is_float = (asbd->mFormatFlags & 1U) != 0; /* kAudioFormatFlagIsFloat */
        bool is_non_int = (asbd->mFormatFlags & (1U << 5)) != 0; /* kAudioFormatFlagIsNonInterleaved */

        bool rate_changed = false;
        if (scope == 2 && (rate == 44100 || rate == 48000)) {
            if (g_ue4_sample_rate != rate) rate_changed = true;
            g_ue4_sample_rate = rate;
        }
        if (scope == 1 && element < kMaxAudioBuses) { /* kAudioUnitScope_Input */
            AudioBus &b = g_buses[element];
            b.sample_rate = (rate >= 8000 && rate <= 96000) ? rate : g_ue4_sample_rate;
            b.channels = ch;
            b.is_float = is_float;
            b.is_non_interleaved = is_non_int;
            b.has_input_asbd = true;
            b.reset_gen++;
        } else if (scope == 2 && element == 0 && !g_buses[0].has_input_asbd) { /* kAudioUnitScope_Output fallback */
            g_buses[0].sample_rate = (rate >= 8000 && rate <= 96000) ? rate : g_ue4_sample_rate;
            g_buses[0].channels = ch;
            g_buses[0].is_float = is_float;
            g_buses[0].is_non_interleaved = is_non_int;
        }
        if (u) {
            u->sample_rate = (rate >= 8000 && rate <= 96000) ? rate : g_ue4_sample_rate;
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
        bool is_3d = (u && u->sub_type == kSubType3DMixerEmbedded) ? true :
                     (u && u->sub_type == kSubTypeMultiChannelMix) ? false : g_graph_uses_3d_mixer;
        g_buses[bus].cb = cbs->inputProc;
        g_buses[bus].ref = cbs->inputProcRefCon;
        g_buses[bus].has_cb = (cbs->inputProc != nullptr);
        g_buses[bus].is_3d_mixer = is_3d;
        if (!is_3d && cbs->inputProc != nullptr) {
            g_buses[bus].enabled = true;
            if (g_buses[bus].volume <= 0.0f) g_buses[bus].volume = 1.0f;
        }
        g_buses[bus].reset_gen++;
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
    (void)bufferOffset;
    auto *u = static_cast<FakeAudioUnit *>(inUnit);
    std::lock_guard<std::mutex> lk(g_audio_mutex);

    bool is_3d = g_graph_uses_3d_mixer;
    if (u) {
        if (u->sub_type == kSubType3DMixerEmbedded) is_3d = true;
        else if (u->sub_type == kSubTypeMultiChannelMix) is_3d = false;
    }

    if (is_3d) {
        /* Apple AU3DMixerEmbedded parameter ABI (used by UE4 FIOSAudioDevice):
         *   0 = k3DMixerParam_Azimuth      (-180..+180 deg; -90=Left, 0=Center, +90=Right)
         *   1 = k3DMixerParam_Elevation    (-90..+90 deg)
         *   2 = k3DMixerParam_Distance     (meters)
         *   3 = k3DMixerParam_Gain         (decibels: -120..+20 dB)
         *   4 = k3DMixerParam_PlaybackRate (0.01..4.0 pitch/speed ratio)
         *   5 = k3DMixerParam_Enable       (0=off, 1=on)
         */
        if (paramID == 0) {
            if (scope == 1 && element < kMaxAudioBuses) {
                constexpr float kDegToRad = 0.017453292519943295f;
                float pan_norm = std::sin(value * kDegToRad);
                set_bus_pan_from_norm(g_buses[element], pan_norm);
            }
        } else if (paramID == 3) {
            float lin = db_to_linear(value);
            if (scope == 2) {
                g_master_volume = lin;
            } else if (scope == 1 && element < kMaxAudioBuses) {
                g_buses[element].volume = lin;
            }
        } else if (paramID == 4) {
            if (scope == 1 && element < kMaxAudioBuses) {
                g_buses[element].playback_rate = std::max(0.05f, std::min(4.0f, value));
            }
        } else if (paramID == 5) {
            if (scope == 1 && element < kMaxAudioBuses) {
                bool now_en = (value > 0.5f);
                if (now_en && !g_buses[element].enabled) {
                    g_buses[element].reset_gen++;
                }
                g_buses[element].enabled = now_en;
            }
        }
    } else {
        /* Apple MultiChannelMixer parameter ABI:
         *   0 = kMultiChannelMixerParam_Volume (linear 0..1+)
         *   1 = kMultiChannelMixerParam_Enable (0/1)
         *   2 = kMultiChannelMixerParam_Pan    (-1..+1)
         */
        if (paramID == 0) {
            float clamped = std::max(0.0f, std::min(2.0f, value));
            if (scope == 2) {
                g_master_volume = clamped;
            } else if (scope == 1 && element < kMaxAudioBuses) {
                g_buses[element].volume = clamped;
            }
        } else if (paramID == 1) {
            if (scope == 1 && element < kMaxAudioBuses) {
                bool now_en = (value > 0.5f);
                if (now_en && !g_buses[element].enabled) {
                    g_buses[element].reset_gen++;
                }
                g_buses[element].enabled = now_en;
            }
        } else if (paramID == 2) {
            if (scope == 1 && element < kMaxAudioBuses) {
                set_bus_pan_from_norm(g_buses[element], value);
            }
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
    g_pump_stop.store(true, std::memory_order_release);
    if (g_pump_thread.joinable()) {
        g_pump_thread.join();
    }
    g_pump_running.store(false, std::memory_order_release);
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
