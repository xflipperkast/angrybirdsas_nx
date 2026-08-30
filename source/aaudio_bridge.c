#include "aaudio_bridge.h"
#include "bionic.h"
#include "jni_fake.h"
#include "trace_log.h"
#include <switch.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

enum {
    AAUDIO_OK = 0,
    AAUDIO_ERROR_DISCONNECTED = -899,
    AAUDIO_ERROR_ILLEGAL_ARGUMENT = -898,
    AAUDIO_ERROR_INTERNAL = -896,
    AAUDIO_ERROR_INVALID_STATE = -895,
    AAUDIO_ERROR_INVALID_HANDLE = -892,
    AAUDIO_ERROR_UNIMPLEMENTED = -890,
    AAUDIO_ERROR_UNAVAILABLE = -889,
    AAUDIO_ERROR_NO_MEMORY = -887,
    AAUDIO_ERROR_NULL = -886,
    AAUDIO_ERROR_TIMEOUT = -885,
    AAUDIO_ERROR_OUT_OF_RANGE = -882,

    AAUDIO_DIRECTION_OUTPUT = 0,
    AAUDIO_DIRECTION_INPUT = 1,
    AAUDIO_SHARING_MODE_EXCLUSIVE = 0,
    AAUDIO_SHARING_MODE_SHARED = 1,
    AAUDIO_PERFORMANCE_MODE_NONE = 10,
    AAUDIO_FORMAT_UNSPECIFIED = 0,
    AAUDIO_FORMAT_PCM_I16 = 1,
    AAUDIO_FORMAT_PCM_FLOAT = 2,
    AAUDIO_STREAM_STATE_UNINITIALIZED = 0,
    AAUDIO_STREAM_STATE_UNKNOWN = 1,
    AAUDIO_STREAM_STATE_OPEN = 2,
    AAUDIO_STREAM_STATE_STARTING = 3,
    AAUDIO_STREAM_STATE_STARTED = 4,
    AAUDIO_STREAM_STATE_STOPPING = 9,
    AAUDIO_STREAM_STATE_STOPPED = 10,
    AAUDIO_STREAM_STATE_CLOSING = 11,
    AAUDIO_STREAM_STATE_CLOSED = 12,
    AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
    AAUDIO_CALLBACK_RESULT_STOP = 1
};

typedef int32_t (*AAudioDataCallback)(void *stream, void *user_data,
                                      void *audio_data, int32_t num_frames);
typedef void (*AAudioErrorCallback)(void *stream, void *user_data, int32_t error);

typedef struct {
    int32_t sample_rate;
    int32_t channels;
    int32_t format;
    int32_t direction;
    int32_t performance_mode;
    int32_t sharing_mode;
    int32_t buffer_capacity_frames;
    int32_t frames_per_callback;
    int32_t device_id;
    int32_t session_id;
    AAudioDataCallback data_callback;
    void *data_user;
    AAudioErrorCallback error_callback;
    void *error_user;
} NxAAudioBuilder;

#define NX_AAUDIO_MAGIC 0x41414e58u
#define NX_AUDOUT_BUFFER_COUNT 3u
#define NX_AUDOUT_WAIT_NS 100000000ull
#define NX_AUDOUT_RECOVERY_US 500000ull
#define NX_AUDOUT_HEARTBEAT_US 5000000ull

struct NxAAudioStream;
typedef struct NxAAudioStream NxAAudioStream;

struct NxAAudioStream {
    uint32_t magic;
    volatile int32_t state;
    volatile int stop_requested;
    int thread_started;
    volatile int thread_exited;
    uint64_t pthread_id;

    int32_t sample_rate;
    int32_t channels;
    int32_t format;
    int32_t direction;
    int32_t performance_mode;
    int32_t sharing_mode;
    int32_t buffer_capacity_frames;
    int32_t buffer_size_frames;
    int32_t frames_per_callback;
    int32_t frames_per_burst;
    int32_t device_id;
    int32_t session_id;
    volatile int32_t xruns;
    volatile int64_t frames_written;

    AAudioDataCallback data_callback;
    void *data_user;
    AAudioErrorCallback error_callback;
    void *error_user;

    uint8_t *callback_buffer;
    size_t callback_bytes;

    int audout_initialized;
    int audout_started;
    uint32_t audout_rate;
    uint32_t audout_channels;
    PcmFormat audout_format;
    AudioOutBuffer out[NX_AUDOUT_BUFFER_COUNT];
    void *out_mem[NX_AUDOUT_BUFFER_COUNT];
    size_t out_alloc_bytes;
    size_t out_capacity_frames;
    uint64_t resample_remainder;

    volatile uint32_t producer_peak_milli;
    volatile uint32_t output_peak_milli;
    volatile uint32_t invalid_samples;
    volatile uint64_t callback_count;
    volatile uint64_t callback_stop_count;
    volatile uint64_t submitted_buffers;
    volatile uint64_t released_buffers;
    volatile uint64_t wait_failures;
    volatile uint64_t append_failures;
    volatile uint64_t recoveries;
    volatile uint64_t last_callback_us;
    volatile uint64_t last_submit_us;
    volatile uint64_t last_release_us;
    volatile uint64_t last_nonzero_us;
    volatile uint64_t silent_since_us;
    volatile uint64_t silent_reports;
    volatile uint64_t last_heartbeat_us;

    struct NxAAudioStream *diag_next;
};

static Mutex g_stream_lock;
static int g_stream_lock_ready;
static NxAAudioStream *g_streams;
static Mutex g_audout_owner_lock;
static int g_audout_owner_lock_ready;
static NxAAudioStream *g_audout_owner;

static size_t align_up_0x1000(size_t n) {
    return (n + 0xfffu) & ~(size_t)0xfffu;
}

static void stream_lock_init(void) {
    if (g_stream_lock_ready) return;
    mutexInit(&g_stream_lock);
    g_stream_lock_ready = 1;
}

static void audout_owner_lock_init(void) {
    if (g_audout_owner_lock_ready) return;
    mutexInit(&g_audout_owner_lock);
    g_audout_owner_lock_ready = 1;
}

static void stream_register(NxAAudioStream *s) {
    stream_lock_init();
    mutexLock(&g_stream_lock);
    s->diag_next = g_streams;
    g_streams = s;
    mutexUnlock(&g_stream_lock);
}

static void stream_unregister(NxAAudioStream *s) {
    stream_lock_init();
    mutexLock(&g_stream_lock);
    NxAAudioStream **pp = &g_streams;
    while (*pp) {
        if (*pp == s) {
            *pp = s->diag_next;
            s->diag_next = NULL;
            break;
        }
        pp = &(*pp)->diag_next;
    }
    mutexUnlock(&g_stream_lock);
}

static int stream_valid(const NxAAudioStream *s) {
    return s && s->magic == NX_AAUDIO_MAGIC;
}

static int16_t float_to_i16(float v, uint32_t *invalid) {
    if (!(v == v) || v > 16.0f || v < -16.0f) {
        if (invalid) (*invalid)++;
        return 0;
    }
    if (v >= 1.0f) return 32767;
    if (v <= -1.0f) return -32768;
    return (int16_t)(v * 32767.0f);
}

static int16_t input_sample_i16(const NxAAudioStream *s, size_t frame, int channel,
                                uint32_t *invalid) {
    int src_ch = channel;
    if (s->channels <= 1) src_ch = 0;
    else if (src_ch >= s->channels) src_ch = s->channels - 1;
    size_t idx = frame * (size_t)s->channels + (size_t)src_ch;
    if (s->format == AAUDIO_FORMAT_PCM_FLOAT)
        return float_to_i16(((const float *)s->callback_buffer)[idx], invalid);
    return ((const int16_t *)s->callback_buffer)[idx];
}

static void measure_callback(NxAAudioStream *s) {
    uint32_t peak = 0;
    uint32_t invalid = 0;
    size_t frames = (size_t)s->frames_per_callback;
    for (size_t f = 0; f < frames; ++f) {
        for (int ch = 0; ch < s->channels; ++ch) {
            int16_t v = input_sample_i16(s, f, ch, &invalid);
            int32_t a = v;
            if (a < 0) a = -a;
            uint32_t pm = (uint32_t)(((uint64_t)(uint32_t)a * 1000u) / 32768u);
            if (pm > peak) peak = pm;
        }
    }
    s->producer_peak_milli = peak;
    s->invalid_samples += invalid;
}

static size_t convert_callback_to_audout(NxAAudioStream *s, int16_t *dst,
                                         size_t dst_capacity_frames) {
    const uint64_t in_frames = (uint64_t)(uint32_t)s->frames_per_callback;
    const uint64_t in_rate = (uint64_t)(uint32_t)s->sample_rate;
    const uint64_t out_rate = (uint64_t)s->audout_rate;
    uint64_t numer = in_frames * out_rate + s->resample_remainder;
    size_t out_frames = (size_t)(numer / in_rate);
    s->resample_remainder = numer % in_rate;
    if (out_frames < 1) out_frames = 1;
    if (out_frames > dst_capacity_frames) out_frames = dst_capacity_frames;

    uint32_t peak = 0;
    uint32_t invalid = 0;
    for (size_t of = 0; of < out_frames; ++of) {
        size_t sf = (size_t)(((uint64_t)of * in_rate) / out_rate);
        if (sf >= (size_t)s->frames_per_callback) sf = (size_t)s->frames_per_callback - 1u;
        int16_t l = input_sample_i16(s, sf, 0, &invalid);
        int16_t r = input_sample_i16(s, sf, s->channels > 1 ? 1 : 0, &invalid);
        dst[of * 2u] = l;
        dst[of * 2u + 1u] = r;
        int32_t al = l, ar = r;
        if (al < 0) al = -al;
        if (ar < 0) ar = -ar;
        uint32_t pm = (uint32_t)(((uint64_t)(uint32_t)(al > ar ? al : ar) * 1000u) / 32768u);
        if (pm > peak) peak = pm;
    }
    s->output_peak_milli = peak;
    s->invalid_samples += invalid;
    return out_frames;
}

static int run_data_callback(NxAAudioStream *s) {
    memset(s->callback_buffer, 0, s->callback_bytes);
    uint64_t begin = trace_now_us();
    int32_t cb = s->data_callback ? s->data_callback(s, s->data_user,
                                                      s->callback_buffer,
                                                      s->frames_per_callback)
                                  : AAUDIO_CALLBACK_RESULT_CONTINUE;
    uint64_t end = trace_now_us();
    s->last_callback_us = end;
    s->callback_count++;
    measure_callback(s);

    if (s->producer_peak_milli) {
        s->last_nonzero_us = end;
        s->silent_since_us = 0;
    } else if (!s->silent_since_us) {
        s->silent_since_us = end;
    } else if (end >= s->silent_since_us && end - s->silent_since_us >= 2000000ull &&
               (!s->silent_reports || end - s->silent_reports >= 5000000ull)) {
        s->silent_reports = end;
        trace_log_printf("AUDIO", "AAudio producer silent stream=%p silence=%llums callbacks=%llu",
                         (void *)s,
                         (unsigned long long)((end - s->silent_since_us) / 1000ull),
                         (unsigned long long)s->callback_count);
    }

    if (end >= begin && end - begin >= 50000ull)
        trace_log_printf("AUDIO", "AAudio slow callback stream=%p time=%lluus result=%d",
                         (void *)s, (unsigned long long)(end - begin), cb);

    if (cb != AAUDIO_CALLBACK_RESULT_CONTINUE) {
        s->callback_stop_count++;
        trace_log_printf("AUDIO", "AAudio callback requested stop stream=%p result=%d callbacks=%llu stop_count=%llu",
                         (void *)s, cb, (unsigned long long)s->callback_count,
                         (unsigned long long)s->callback_stop_count);
        return 1;
    }
    return 0;
}

static int fill_and_append(NxAAudioStream *s, AudioOutBuffer *out) {
    if (run_data_callback(s)) return 1;
    size_t frames = convert_callback_to_audout(s, (int16_t *)out->buffer,
                                               s->out_capacity_frames);
    out->data_offset = 0;
    out->data_size = frames * 2u * sizeof(int16_t);
    armDCacheFlush(out->buffer, out->buffer_size);
    Result rc = audoutAppendAudioOutBuffer(out);
    if (R_FAILED(rc)) {
        s->append_failures++;
        trace_log_printf("AUDIO", "audout append failed stream=%p rc=0x%x failures=%llu data=%llu",
                         (void *)s, (unsigned)rc,
                         (unsigned long long)s->append_failures,
                         (unsigned long long)out->data_size);
        return -1;
    }
    s->submitted_buffers++;
    s->last_submit_us = trace_now_us();
    s->frames_written += s->frames_per_callback;
    return 0;
}

static int prime_audout(NxAAudioStream *s) {
    for (unsigned i = 0; i < NX_AUDOUT_BUFFER_COUNT; ++i) {
        int r = fill_and_append(s, &s->out[i]);
        if (r != 0) return r;
    }
    Result rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        trace_log_printf("AUDIO", "audout start failed stream=%p rc=0x%x", (void *)s, (unsigned)rc);
        return -1;
    }
    s->audout_started = 1;
    s->last_release_us = trace_now_us();
    trace_log_printf("AUDIO", "audout started stream=%p queued=%u logical_rate=%d hw_rate=%u",
                     (void *)s, NX_AUDOUT_BUFFER_COUNT, s->sample_rate, s->audout_rate);
    return 0;
}

static int recover_audout(NxAAudioStream *s, const char *why, Result cause) {
    s->recoveries++;
    trace_log_printf("AUDIO", "audout recovery stream=%p reason=%s cause=0x%x recovery=%llu",
                     (void *)s, why ? why : "?", (unsigned)cause,
                     (unsigned long long)s->recoveries);
    (void)audoutStopAudioOut();
    s->audout_started = 0;
    bool flushed = false;
    Result frc = audoutFlushAudioOutBuffers(&flushed);
    trace_log_printf("AUDIO", "audout flush stream=%p rc=0x%x flushed=%d",
                     (void *)s, (unsigned)frc, flushed ? 1 : 0);
    s->resample_remainder = 0;
    int r = prime_audout(s);
    if (r < 0) return -1;
    if (r > 0) return 1;
    return 0;
}

static void log_heartbeat(NxAAudioStream *s) {
    uint64_t now = trace_now_us();
    if (s->last_heartbeat_us && now - s->last_heartbeat_us < NX_AUDOUT_HEARTBEAT_US) return;
    s->last_heartbeat_us = now;
    u32 queued = 0;
    u64 played = 0;
    AudioOutState state = AudioOutState_Stopped;
    Result qrc = audoutGetAudioOutBufferCount(&queued);
    Result prc = audoutGetAudioOutPlayedSampleCount(&played);
    Result src = audoutGetAudioOutState(&state);
    trace_log_printf("AUDIO", "heartbeat stream=%p state=%d state_rc=0x%x queued=%u qrc=0x%x played=%llu prc=0x%x callbacks=%llu submitted=%llu released=%llu peak=%u.%03u out_peak=%u.%03u wait_fail=%llu append_fail=%llu recoveries=%llu",
                     (void *)s, (int)state, (unsigned)src, queued, (unsigned)qrc,
                     (unsigned long long)played, (unsigned)prc,
                     (unsigned long long)s->callback_count,
                     (unsigned long long)s->submitted_buffers,
                     (unsigned long long)s->released_buffers,
                     s->producer_peak_milli / 1000u, s->producer_peak_milli % 1000u,
                     s->output_peak_milli / 1000u, s->output_peak_milli % 1000u,
                     (unsigned long long)s->wait_failures,
                     (unsigned long long)s->append_failures,
                     (unsigned long long)s->recoveries);
}

static void *aaudio_output_thread(void *arg) {
    NxAAudioStream *s = (NxAAudioStream *)arg;
    s->thread_exited = 0;
    s->state = AAUDIO_STREAM_STATE_STARTED;
    trace_log_printf("AUDIO", "AAudio thread start stream=%p logical_rate=%d ch=%d fmt=%d cb_frames=%d hw_rate=%u hw_ch=%u mode=libnx-audout",
                     (void *)s, s->sample_rate, s->channels, s->format,
                     s->frames_per_callback, s->audout_rate, s->audout_channels);

    int prime = prime_audout(s);
    if (prime < 0) {
        if (s->error_callback) s->error_callback(s, s->error_user, AAUDIO_ERROR_DISCONNECTED);
    } else if (prime == 0) {
        unsigned consecutive_wait_failures = 0;
        while (!s->stop_requested && !jni_quit_requested) {
            AudioOutBuffer *released = NULL;
            u32 released_count = 0;
            Result rc = audoutWaitPlayFinish(&released, &released_count, NX_AUDOUT_WAIT_NS);
            uint64_t now = trace_now_us();
            if (R_FAILED(rc)) {
                s->wait_failures++;
                consecutive_wait_failures++;
                if (s->wait_failures <= 4 || (s->wait_failures % 16u) == 0)
                    trace_log_printf("AUDIO", "audout wait failed stream=%p rc=0x%x consecutive=%u total=%llu",
                                     (void *)s, (unsigned)rc, consecutive_wait_failures,
                                     (unsigned long long)s->wait_failures);
                if (s->stop_requested || jni_quit_requested) break;
                AudioOutState state = AudioOutState_Stopped;
                Result src = audoutGetAudioOutState(&state);
                if ((now >= s->last_release_us && now - s->last_release_us >= NX_AUDOUT_RECOVERY_US) ||
                    (R_SUCCEEDED(src) && state != AudioOutState_Started)) {
                    int rr = recover_audout(s, state == AudioOutState_Started ? "no-release" : "device-stopped", rc);
                    consecutive_wait_failures = 0;
                    if (rr < 0) {
                        if (s->error_callback) s->error_callback(s, s->error_user, AAUDIO_ERROR_DISCONNECTED);
                        break;
                    }
                    if (rr > 0) break;
                }
                log_heartbeat(s);
                continue;
            }

            consecutive_wait_failures = 0;
            if (!released_count || !released) {
                log_heartbeat(s);
                continue;
            }
            unsigned drained = 0;
            while (released && released_count && !s->stop_requested && !jni_quit_requested) {
                s->released_buffers++;
                s->last_release_us = trace_now_us();
                int r = fill_and_append(s, released);
                if (r < 0) {
                    int rr = recover_audout(s, "append-failed", 0);
                    if (rr < 0) {
                        if (s->error_callback) s->error_callback(s, s->error_user, AAUDIO_ERROR_DISCONNECTED);
                        s->stop_requested = 1;
                    } else if (rr > 0) {
                        s->stop_requested = 1;
                    }
                    break;
                } else if (r > 0) {
                    s->stop_requested = 1;
                    break;
                }
                drained++;
                released = NULL;
                released_count = 0;
                if (drained < NX_AUDOUT_BUFFER_COUNT) {
                    Result grc = audoutGetReleasedAudioOutBuffer(&released, &released_count);
                    if (R_FAILED(grc)) {
                        s->wait_failures++;
                        trace_log_printf("AUDIO", "audout get-released failed stream=%p rc=0x%x total_wait_fail=%llu",
                                         (void *)s, (unsigned)grc,
                                         (unsigned long long)s->wait_failures);
                        break;
                    }
                }
            }
            log_heartbeat(s);
        }
    }

    if (s->audout_started) {
        (void)audoutStopAudioOut();
        s->audout_started = 0;
    }
    bool exit_flushed = false;
    (void)audoutFlushAudioOutBuffers(&exit_flushed);
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    __atomic_store_n(&s->thread_exited, 1, __ATOMIC_RELEASE);
    trace_log_printf("AUDIO", "AAudio thread exit stream=%p stop_requested=%d callbacks=%llu frames=%lld submitted=%llu released=%llu wait_fail=%llu append_fail=%llu recoveries=%llu",
                     (void *)s, s->stop_requested,
                     (unsigned long long)s->callback_count,
                     (long long)s->frames_written,
                     (unsigned long long)s->submitted_buffers,
                     (unsigned long long)s->released_buffers,
                     (unsigned long long)s->wait_failures,
                     (unsigned long long)s->append_failures,
                     (unsigned long long)s->recoveries);
    return NULL;
}

int aaudio_bridge_create_stream_builder(void **builder) {
    if (!builder) return AAUDIO_ERROR_NULL;
    NxAAudioBuilder *b = calloc(1, sizeof *b);
    if (!b) { *builder = NULL; return AAUDIO_ERROR_NO_MEMORY; }
    b->direction = AAUDIO_DIRECTION_OUTPUT;
    b->performance_mode = AAUDIO_PERFORMANCE_MODE_NONE;
    b->sharing_mode = AAUDIO_SHARING_MODE_SHARED;
    b->session_id = -1;
    *builder = b;
    return AAUDIO_OK;
}

int aaudio_bridge_builder_delete(void *builder) {
    if (!builder) return AAUDIO_ERROR_NULL;
    free(builder);
    return AAUDIO_OK;
}

#define BUILDER_SETTER(name, field) \
    void name(void *builder, int32_t value) { \
        NxAAudioBuilder *b = (NxAAudioBuilder *)builder; \
        if (b) b->field = value; \
    }
BUILDER_SETTER(aaudio_bridge_builder_set_buffer_capacity, buffer_capacity_frames)
BUILDER_SETTER(aaudio_bridge_builder_set_channel_count, channels)
BUILDER_SETTER(aaudio_bridge_builder_set_device_id, device_id)
BUILDER_SETTER(aaudio_bridge_builder_set_direction, direction)
BUILDER_SETTER(aaudio_bridge_builder_set_format, format)
BUILDER_SETTER(aaudio_bridge_builder_set_frames_per_callback, frames_per_callback)
BUILDER_SETTER(aaudio_bridge_builder_set_performance_mode, performance_mode)
BUILDER_SETTER(aaudio_bridge_builder_set_sample_rate, sample_rate)
BUILDER_SETTER(aaudio_bridge_builder_set_session_id, session_id)
BUILDER_SETTER(aaudio_bridge_builder_set_sharing_mode, sharing_mode)
#undef BUILDER_SETTER

void aaudio_bridge_builder_set_data_callback(void *builder, void *callback, void *user_data) {
    NxAAudioBuilder *b = (NxAAudioBuilder *)builder;
    if (!b) return;
    b->data_callback = (AAudioDataCallback)callback;
    b->data_user = user_data;
}

void aaudio_bridge_builder_set_error_callback(void *builder, void *callback, void *user_data) {
    NxAAudioBuilder *b = (NxAAudioBuilder *)builder;
    if (!b) return;
    b->error_callback = (AAudioErrorCallback)callback;
    b->error_user = user_data;
}

static void free_output_buffers(NxAAudioStream *s) {
    for (unsigned i = 0; i < NX_AUDOUT_BUFFER_COUNT; ++i) {
        free(s->out_mem[i]);
        s->out_mem[i] = NULL;
        memset(&s->out[i], 0, sizeof(s->out[i]));
    }
}

int aaudio_bridge_builder_open_stream(void *builder, void **stream_out) {
    if (!builder || !stream_out) return AAUDIO_ERROR_NULL;
    *stream_out = NULL;
    NxAAudioBuilder *b = (NxAAudioBuilder *)builder;
    if (b->direction != AAUDIO_DIRECTION_OUTPUT) return AAUDIO_ERROR_UNIMPLEMENTED;
    if (!b->data_callback) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;

    NxAAudioStream *s = calloc(1, sizeof *s);
    if (!s) return AAUDIO_ERROR_NO_MEMORY;
    s->magic = NX_AAUDIO_MAGIC;
    s->state = AAUDIO_STREAM_STATE_UNINITIALIZED;
    s->sample_rate = b->sample_rate > 0 ? b->sample_rate : 48000;
    s->channels = b->channels > 0 ? b->channels : 2;
    s->format = b->format == AAUDIO_FORMAT_UNSPECIFIED ? AAUDIO_FORMAT_PCM_FLOAT : b->format;
    s->direction = b->direction;
    s->performance_mode = b->performance_mode;
    s->sharing_mode = b->sharing_mode;
    s->device_id = b->device_id;
    s->session_id = b->session_id;
    s->data_callback = b->data_callback;
    s->data_user = b->data_user;
    s->error_callback = b->error_callback;
    s->error_user = b->error_user;

    if (s->sample_rate < 8000 || s->sample_rate > 192000 ||
        s->channels < 1 || s->channels > 8 ||
        (s->format != AAUDIO_FORMAT_PCM_FLOAT && s->format != AAUDIO_FORMAT_PCM_I16)) {
        free(s);
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }

    s->frames_per_callback = b->frames_per_callback > 0 ? b->frames_per_callback : 1024;
    if (s->frames_per_callback < 32) s->frames_per_callback = 32;
    if (s->frames_per_callback > 8192) s->frames_per_callback = 8192;
    s->frames_per_burst = 1024;
    s->buffer_capacity_frames = b->buffer_capacity_frames > 0 ? b->buffer_capacity_frames : s->frames_per_burst * 4;
    if (s->buffer_capacity_frames < s->frames_per_callback * 2)
        s->buffer_capacity_frames = s->frames_per_callback * 2;
    s->buffer_size_frames = s->frames_per_burst * 4;
    if (s->buffer_size_frames > s->buffer_capacity_frames)
        s->buffer_size_frames = s->buffer_capacity_frames;

    size_t bytes_per_sample = s->format == AAUDIO_FORMAT_PCM_FLOAT ? sizeof(float) : sizeof(int16_t);
    s->callback_bytes = (size_t)s->frames_per_callback * (size_t)s->channels * bytes_per_sample;
    s->callback_buffer = memalign(64, s->callback_bytes);
    if (!s->callback_buffer) {
        free(s);
        return AAUDIO_ERROR_NO_MEMORY;
    }
    memset(s->callback_buffer, 0, s->callback_bytes);

    audout_owner_lock_init();
    mutexLock(&g_audout_owner_lock);
    if (g_audout_owner) {
        mutexUnlock(&g_audout_owner_lock);
        free(s->callback_buffer);
        free(s);
        trace_log_printf("AUDIO", "AAudio open rejected: audout already owned stream=%p", (void *)g_audout_owner);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    Result arc = audoutInitialize();
    if (R_FAILED(arc)) {
        mutexUnlock(&g_audout_owner_lock);
        free(s->callback_buffer);
        free(s);
        trace_log_printf("AUDIO", "audoutInitialize failed rc=0x%x", (unsigned)arc);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    s->audout_initialized = 1;
    s->audout_rate = audoutGetSampleRate();
    s->audout_channels = audoutGetChannelCount();
    s->audout_format = audoutGetPcmFormat();
    AudioOutState initial_state = audoutGetDeviceState();
    if (s->audout_rate != 48000 || s->audout_channels != 2 || s->audout_format != PcmFormat_Int16) {
        trace_log_printf("AUDIO", "unsupported audout format rate=%u ch=%u fmt=%d state=%d",
                         s->audout_rate, s->audout_channels, (int)s->audout_format, (int)initial_state);
        audoutExit();
        s->audout_initialized = 0;
        mutexUnlock(&g_audout_owner_lock);
        free(s->callback_buffer);
        free(s);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    if (initial_state == AudioOutState_Started) (void)audoutStopAudioOut();
    bool flushed = false;
    Result frc = audoutFlushAudioOutBuffers(&flushed);
    g_audout_owner = s;
    mutexUnlock(&g_audout_owner_lock);

    uint64_t max_out_frames = ((uint64_t)(uint32_t)s->frames_per_callback * s->audout_rate +
                               (uint64_t)(uint32_t)s->sample_rate - 1ull) /
                              (uint64_t)(uint32_t)s->sample_rate;
    s->out_capacity_frames = (size_t)max_out_frames;
    size_t data_bytes = s->out_capacity_frames * 2u * sizeof(int16_t);
    s->out_alloc_bytes = align_up_0x1000(data_bytes);
    for (unsigned i = 0; i < NX_AUDOUT_BUFFER_COUNT; ++i) {
        s->out_mem[i] = memalign(0x1000, s->out_alloc_bytes);
        if (!s->out_mem[i]) {
            free_output_buffers(s);
            mutexLock(&g_audout_owner_lock);
            if (g_audout_owner == s) g_audout_owner = NULL;
            mutexUnlock(&g_audout_owner_lock);
            audoutExit();
            free(s->callback_buffer);
            free(s);
            return AAUDIO_ERROR_NO_MEMORY;
        }
        memset(s->out_mem[i], 0, s->out_alloc_bytes);
        s->out[i].next = NULL;
        s->out[i].buffer = s->out_mem[i];
        s->out[i].buffer_size = s->out_alloc_bytes;
        s->out[i].data_size = 0;
        s->out[i].data_offset = 0;
    }

    s->state = AAUDIO_STREAM_STATE_OPEN;
    stream_register(s);
    trace_log_printf("AUDIO", "AAudio open stream=%p logical_rate=%d ch=%d fmt=%d burst=%d callback=%d capacity=%d buffer=%d backend=libnx-audout hw_rate=%u hw_ch=%u hw_fmt=%d initial_state=%d flush_rc=0x%x flushed=%d slot_bytes=%u",
                     (void *)s, s->sample_rate, s->channels, s->format,
                     s->frames_per_burst, s->frames_per_callback,
                     s->buffer_capacity_frames, s->buffer_size_frames,
                     s->audout_rate, s->audout_channels, (int)s->audout_format,
                     (int)initial_state, (unsigned)frc, flushed ? 1 : 0,
                     (unsigned)s->out_alloc_bytes);
    *stream_out = s;
    return AAUDIO_OK;
}

int aaudio_bridge_stream_request_start(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!stream_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (s->thread_started && __atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
        trace_log_printf("AUDIO", "AAudio reaping stopped output thread stream=%p tid=%08llx",
                         (void *)s, (unsigned long long)s->pthread_id);
        if (s->pthread_id) (void)bionic_pthread_join(s->pthread_id, NULL);
        s->pthread_id = 0;
        s->thread_started = 0;
        s->thread_exited = 0;
    }
    if (s->thread_started || (s->state != AAUDIO_STREAM_STATE_OPEN && s->state != AAUDIO_STREAM_STATE_STOPPED))
        return AAUDIO_ERROR_INVALID_STATE;

    s->stop_requested = 0;
    s->thread_exited = 0;
    s->state = AAUDIO_STREAM_STATE_STARTING;
    s->resample_remainder = 0;
    if (s->audout_started) {
        (void)audoutStopAudioOut();
        s->audout_started = 0;
    }
    bool start_flushed = false;
    Result start_flush_rc = audoutFlushAudioOutBuffers(&start_flushed);
    trace_log_printf("AUDIO", "AAudio start reset stream=%p flush_rc=0x%x flushed=%d",
                     (void *)s, (unsigned)start_flush_rc, start_flushed ? 1 : 0);
    uint64_t tid = 0;
    int pr = bionic_pthread_create(&tid, NULL, aaudio_output_thread, s);
    if (pr != 0 || !tid) {
        s->state = AAUDIO_STREAM_STATE_OPEN;
        trace_log_printf("AUDIO", "AAudio start failed stream=%p pthread_rc=%d", (void *)s, pr);
        return AAUDIO_ERROR_INTERNAL;
    }
    s->pthread_id = tid;
    s->thread_started = 1;
    (void)bionic_pthread_setname_np(tid, "AAudioOutput");
    Result priority_rc = svcSetThreadPriority((Handle)tid, 0x24);
    trace_log_printf("AUDIO", "AAudio requestStart stream=%p tid=%08llx priority=0x24 rc=0x%x backend=libnx-audout",
                     (void *)s, (unsigned long long)tid, (unsigned)priority_rc);
    return AAUDIO_OK;
}

int aaudio_bridge_stream_request_stop(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!stream_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (!s->thread_started) {
        if (s->state == AAUDIO_STREAM_STATE_OPEN || s->state == AAUDIO_STREAM_STATE_STOPPED) return AAUDIO_OK;
        return AAUDIO_ERROR_INVALID_STATE;
    }
    trace_log_printf("AUDIO", "AAudio requestStop stream=%p state=%d tid=%08llx",
                     (void *)s, s->state, (unsigned long long)s->pthread_id);
    s->state = AAUDIO_STREAM_STATE_STOPPING;
    s->stop_requested = 1;
    if (s->pthread_id) (void)bionic_pthread_join(s->pthread_id, NULL);
    s->pthread_id = 0;
    s->thread_started = 0;
    s->thread_exited = 0;
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    return AAUDIO_OK;
}

int aaudio_bridge_stream_close(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!stream_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (s->thread_started) (void)aaudio_bridge_stream_request_stop(s);
    s->state = AAUDIO_STREAM_STATE_CLOSING;
    trace_log_printf("AUDIO", "AAudio close stream=%p callbacks=%llu frames=%lld stops=%llu submitted=%llu released=%llu wait_fail=%llu append_fail=%llu recoveries=%llu",
                     (void *)s, (unsigned long long)s->callback_count,
                     (long long)s->frames_written,
                     (unsigned long long)s->callback_stop_count,
                     (unsigned long long)s->submitted_buffers,
                     (unsigned long long)s->released_buffers,
                     (unsigned long long)s->wait_failures,
                     (unsigned long long)s->append_failures,
                     (unsigned long long)s->recoveries);
    stream_unregister(s);
    if (s->audout_initialized) {
        (void)audoutStopAudioOut();
        bool flushed = false;
        (void)audoutFlushAudioOutBuffers(&flushed);
        audout_owner_lock_init();
        mutexLock(&g_audout_owner_lock);
        if (g_audout_owner == s) g_audout_owner = NULL;
        mutexUnlock(&g_audout_owner_lock);
        audoutExit();
        s->audout_initialized = 0;
    }
    free_output_buffers(s);
    free(s->callback_buffer);
    s->callback_buffer = NULL;
    s->magic = 0;
    s->state = AAUDIO_STREAM_STATE_CLOSED;
    free(s);
    return AAUDIO_OK;
}

int aaudio_bridge_stream_wait_for_state_change(void *stream, int32_t input_state,
                                                int32_t *next_state,
                                                int64_t timeout_nanoseconds) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!stream_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    int64_t waited = 0;
    const int64_t quantum = 1000000ll;
    const int infinite = timeout_nanoseconds < 0;
    while (s->state == input_state) {
        if (!infinite && (timeout_nanoseconds == 0 || waited >= timeout_nanoseconds)) {
            if (next_state) *next_state = s->state;
            return AAUDIO_ERROR_TIMEOUT;
        }
        int64_t sleep_ns = quantum;
        if (!infinite && timeout_nanoseconds > 0 && sleep_ns > timeout_nanoseconds - waited)
            sleep_ns = timeout_nanoseconds - waited;
        if (sleep_ns <= 0) break;
        svcSleepThread(sleep_ns);
        waited += sleep_ns;
    }
    if (next_state) *next_state = s->state;
    return s->state == input_state ? AAUDIO_ERROR_TIMEOUT : AAUDIO_OK;
}

void aaudio_bridge_diag_snapshot(const char *reason) {
    typedef struct {
        void *ptr;
        int state, started, exited, stop, rate, ch, fmt, burst, cb_frames, buffer_frames, xruns;
        int hw_state;
        unsigned hw_rate, hw_ch, hw_fmt, producer_peak, output_peak, invalid_samples, queued;
        uint64_t callbacks, stops, submitted, released, wait_failures, append_failures, recoveries;
        uint64_t frames, last_cb, last_submit, last_release, last_nonzero, silent_since, played;
    } ASnap;
    ASnap snap[8];
    unsigned n = 0;
    uint64_t now = trace_now_us();
    stream_lock_init();
    mutexLock(&g_stream_lock);
    for (NxAAudioStream *s = g_streams; s && n < 8; s = s->diag_next) {
        ASnap *x = &snap[n++];
        memset(x, 0, sizeof(*x));
        x->ptr = s;
        x->state = s->state;
        x->started = s->thread_started;
        x->exited = s->thread_exited;
        x->stop = s->stop_requested;
        x->rate = s->sample_rate;
        x->ch = s->channels;
        x->fmt = s->format;
        x->burst = s->frames_per_burst;
        x->cb_frames = s->frames_per_callback;
        x->buffer_frames = s->buffer_size_frames;
        x->xruns = s->xruns;
        x->hw_rate = s->audout_rate;
        x->hw_ch = s->audout_channels;
        x->hw_fmt = (unsigned)s->audout_format;
        x->producer_peak = s->producer_peak_milli;
        x->output_peak = s->output_peak_milli;
        x->invalid_samples = s->invalid_samples;
        x->callbacks = s->callback_count;
        x->stops = s->callback_stop_count;
        x->submitted = s->submitted_buffers;
        x->released = s->released_buffers;
        x->wait_failures = s->wait_failures;
        x->append_failures = s->append_failures;
        x->recoveries = s->recoveries;
        x->frames = s->frames_written;
        x->last_cb = s->last_callback_us;
        x->last_submit = s->last_submit_us;
        x->last_release = s->last_release_us;
        x->last_nonzero = s->last_nonzero_us;
        x->silent_since = s->silent_since_us;
        AudioOutState st = AudioOutState_Stopped;
        (void)audoutGetAudioOutState(&st);
        x->hw_state = (int)st;
        (void)audoutGetAudioOutBufferCount(&x->queued);
        (void)audoutGetAudioOutPlayedSampleCount(&x->played);
    }
    mutexUnlock(&g_stream_lock);
    trace_log_printf("AUDIO", "snapshot reason=%s streams=%u", reason ? reason : "?", n);
    for (unsigned i = 0; i < n; ++i) {
        ASnap *x = &snap[i];
        trace_log_printf("AUDIO", "stream=%p state=%d thread_started=%d exited=%d stop=%d logical_rate=%d ch=%d fmt=%d hw_state=%d hw_rate=%u hw_ch=%u hw_fmt=%u queued=%u played=%llu burst=%d cb=%d buffer=%d peak=%u.%03u out_peak=%u.%03u invalid=%u xruns=%d callbacks=%llu callback_stops=%llu submitted=%llu released=%llu wait_fail=%llu append_fail=%llu recoveries=%llu frames=%llu last_cb_age=%llums last_submit_age=%llums last_release_age=%llums last_nonzero_age=%llums silent_for=%llums",
                         x->ptr, x->state, x->started, x->exited, x->stop,
                         x->rate, x->ch, x->fmt, x->hw_state, x->hw_rate, x->hw_ch,
                         x->hw_fmt, x->queued, (unsigned long long)x->played,
                         x->burst, x->cb_frames, x->buffer_frames,
                         x->producer_peak / 1000u, x->producer_peak % 1000u,
                         x->output_peak / 1000u, x->output_peak % 1000u,
                         x->invalid_samples, x->xruns,
                         (unsigned long long)x->callbacks,
                         (unsigned long long)x->stops,
                         (unsigned long long)x->submitted,
                         (unsigned long long)x->released,
                         (unsigned long long)x->wait_failures,
                         (unsigned long long)x->append_failures,
                         (unsigned long long)x->recoveries,
                         (unsigned long long)x->frames,
                         (unsigned long long)(x->last_cb && now >= x->last_cb ? (now - x->last_cb) / 1000ull : 0),
                         (unsigned long long)(x->last_submit && now >= x->last_submit ? (now - x->last_submit) / 1000ull : 0),
                         (unsigned long long)(x->last_release && now >= x->last_release ? (now - x->last_release) / 1000ull : 0),
                         (unsigned long long)(x->last_nonzero && now >= x->last_nonzero ? (now - x->last_nonzero) / 1000ull : 0),
                         (unsigned long long)(x->silent_since && now >= x->silent_since ? (now - x->silent_since) / 1000ull : 0));
    }
}

int32_t aaudio_bridge_stream_get_state(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->state : AAUDIO_STREAM_STATE_UNINITIALIZED;
}
int32_t aaudio_bridge_stream_get_buffer_capacity(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->buffer_capacity_frames : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_buffer_size(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->buffer_size_frames : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_set_buffer_size(void *stream, int32_t frames) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!stream_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (frames < 0) return AAUDIO_ERROR_OUT_OF_RANGE;
    if (frames > s->buffer_capacity_frames) frames = s->buffer_capacity_frames;
    if (frames < s->frames_per_burst) frames = s->frames_per_burst;
    s->buffer_size_frames = frames;
    return frames;
}
int32_t aaudio_bridge_stream_get_device_id(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->device_id : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_frames_per_burst(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->frames_per_burst : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_session_id(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->session_id : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_xrun_count(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->xruns : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_is_mmap_used(void *stream) {
    (void)stream;
    return 0;
}
int32_t aaudio_bridge_stream_get_sample_rate(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->sample_rate : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_channel_count(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->channels : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_format(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->format : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_direction(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->direction : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_performance_mode(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->performance_mode : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_sharing_mode(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->sharing_mode : AAUDIO_ERROR_INVALID_HANDLE;
}
int64_t aaudio_bridge_stream_get_frames_written(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return stream_valid(s) ? s->frames_written : (int64_t)AAUDIO_ERROR_INVALID_HANDLE;
}
