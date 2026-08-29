#include "aaudio_bridge.h"
#include "bionic.h"
#include "jni_fake.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    uint32_t magic;
    volatile int32_t state;
    volatile int stop_requested;
    int thread_started;
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
    SDL_AudioDeviceID device;
    SDL_AudioFormat sdl_format;
    uint8_t *buffer;
    size_t buffer_bytes;
    uint64_t pthread_id;
} NxAAudioStream;

#define NX_AAUDIO_MAGIC 0x41414e58u

static int g_sdl_audio_ready;

static int aaudio_stream_is_valid(const NxAAudioStream *s) {
    return s && s->magic == NX_AAUDIO_MAGIC;
}

static void *aaudio_output_thread(void *arg) {
    NxAAudioStream *s = (NxAAudioStream *)arg;
    s->state = AAUDIO_STREAM_STATE_STARTED;

    const size_t bytes_per_sample = s->format == AAUDIO_FORMAT_PCM_FLOAT ? 4u : 2u;
    const size_t bytes_per_frame = bytes_per_sample * (size_t)s->channels;
    int had_audio = 0;

    while (!s->stop_requested && !jni_quit_requested) {
        const Uint32 queued = SDL_GetQueuedAudioSize(s->device);
        const Uint32 target = (Uint32)((size_t)s->buffer_size_frames * bytes_per_frame);
        if (queued >= target) {
            svcSleepThread(1000000ll);
            continue;
        }
        if (had_audio && queued == 0) s->xruns++;

        memset(s->buffer, 0, s->buffer_bytes);
        int32_t cb = AAUDIO_CALLBACK_RESULT_CONTINUE;
        if (s->data_callback)
            cb = s->data_callback(s, s->data_user, s->buffer, s->frames_per_callback);
        if (cb != AAUDIO_CALLBACK_RESULT_CONTINUE) {
            break;
        }
        if (SDL_QueueAudio(s->device, s->buffer, (Uint32)s->buffer_bytes) != 0) {
            if (s->error_callback)
                s->error_callback(s, s->error_user, AAUDIO_ERROR_DISCONNECTED);
            break;
        }
        had_audio = 1;
        s->frames_written += s->frames_per_callback;
    }

    s->state = AAUDIO_STREAM_STATE_STOPPED;
    return NULL;
}

int aaudio_bridge_create_stream_builder(void **builder) {
    if (!builder) return AAUDIO_ERROR_NULL;
    NxAAudioBuilder *b = calloc(1, sizeof *b);
    if (!b) { *builder = NULL; return AAUDIO_ERROR_NO_MEMORY; }
    b->sample_rate = 0;
    b->channels = 0;
    b->format = AAUDIO_FORMAT_UNSPECIFIED;
    b->direction = AAUDIO_DIRECTION_OUTPUT;
    b->performance_mode = AAUDIO_PERFORMANCE_MODE_NONE;
    b->sharing_mode = AAUDIO_SHARING_MODE_SHARED;
    b->buffer_capacity_frames = 0;
    b->frames_per_callback = 0;
    b->device_id = 0;
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

    if (!g_sdl_audio_ready) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            free(s);
            return AAUDIO_ERROR_UNAVAILABLE;
        }
        g_sdl_audio_ready = 1;
    }

    s->frames_per_callback = b->frames_per_callback > 0 ? b->frames_per_callback : 1024;
    if (s->frames_per_callback < 32) s->frames_per_callback = 32;
    if (s->frames_per_callback > 8192) s->frames_per_callback = 8192;
    s->sdl_format = s->format == AAUDIO_FORMAT_PCM_FLOAT ? AUDIO_F32SYS : AUDIO_S16SYS;

    SDL_AudioSpec want;
    SDL_AudioSpec got;
    memset(&want, 0, sizeof want);
    memset(&got, 0, sizeof got);
    want.freq = s->sample_rate;
    want.format = s->sdl_format;
    want.channels = (Uint8)s->channels;
    want.samples = (Uint16)(s->frames_per_callback > 4096 ? 4096 : s->frames_per_callback);
    want.callback = NULL;
    s->device = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!s->device) {
        free(s);
        return AAUDIO_ERROR_UNAVAILABLE;
    }

    if (got.freq <= 0 || got.channels == 0 || got.format != s->sdl_format) {
        SDL_CloseAudioDevice(s->device);
        free(s);
        return AAUDIO_ERROR_UNAVAILABLE;
    }
    s->sample_rate = got.freq;
    s->channels = got.channels;
    s->frames_per_burst = got.samples > 0 ? got.samples : s->frames_per_callback;
    if (s->frames_per_callback <= 0) s->frames_per_callback = s->frames_per_burst;
    s->buffer_capacity_frames = b->buffer_capacity_frames > 0 ? b->buffer_capacity_frames
                                                               : s->frames_per_burst * 4;
    if (s->buffer_capacity_frames < s->frames_per_callback * 2)
        s->buffer_capacity_frames = s->frames_per_callback * 2;
    s->buffer_size_frames = s->frames_per_burst * 2;
    if (s->buffer_size_frames > s->buffer_capacity_frames)
        s->buffer_size_frames = s->buffer_capacity_frames;

    const size_t bytes_per_sample = s->format == AAUDIO_FORMAT_PCM_FLOAT ? 4u : 2u;
    s->buffer_bytes = (size_t)s->frames_per_callback * (size_t)s->channels * bytes_per_sample;
    s->buffer = memalign(64, s->buffer_bytes);
    if (!s->buffer) {
        SDL_CloseAudioDevice(s->device);
        free(s);
        return AAUDIO_ERROR_NO_MEMORY;
    }
    memset(s->buffer, 0, s->buffer_bytes);

    SDL_PauseAudioDevice(s->device, 1);
    s->state = AAUDIO_STREAM_STATE_OPEN;
    *stream_out = s;
    return AAUDIO_OK;
}

int aaudio_bridge_stream_request_start(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!aaudio_stream_is_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (s->thread_started || (s->state != AAUDIO_STREAM_STATE_OPEN && s->state != AAUDIO_STREAM_STATE_STOPPED))
        return AAUDIO_ERROR_INVALID_STATE;
    s->stop_requested = 0;
    s->state = AAUDIO_STREAM_STATE_STARTING;

    uint64_t tid = 0;
    int pr = bionic_pthread_create(&tid, NULL, aaudio_output_thread, s);
    if (pr != 0 || !tid) {
        s->state = AAUDIO_STREAM_STATE_OPEN;
        return AAUDIO_ERROR_INTERNAL;
    }
    s->pthread_id = tid;
    s->thread_started = 1;
    SDL_PauseAudioDevice(s->device, 0);
    return AAUDIO_OK;
}

int aaudio_bridge_stream_request_stop(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!aaudio_stream_is_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (!s->thread_started) {
        if (s->state == AAUDIO_STREAM_STATE_OPEN || s->state == AAUDIO_STREAM_STATE_STOPPED)
            return AAUDIO_OK;
        return AAUDIO_ERROR_INVALID_STATE;
    }
    s->state = AAUDIO_STREAM_STATE_STOPPING;
    s->stop_requested = 1;
    if (s->pthread_id) (void)bionic_pthread_join(s->pthread_id, NULL);
    s->pthread_id = 0;
    s->thread_started = 0;
    SDL_PauseAudioDevice(s->device, 1);
    SDL_ClearQueuedAudio(s->device);
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    return AAUDIO_OK;
}

int aaudio_bridge_stream_close(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!aaudio_stream_is_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (s->thread_started) aaudio_bridge_stream_request_stop(s);
    s->state = AAUDIO_STREAM_STATE_CLOSING;
    if (s->device) SDL_CloseAudioDevice(s->device);
    free(s->buffer);
    s->buffer = NULL;
    s->magic = 0;
    s->state = AAUDIO_STREAM_STATE_CLOSED;
    free(s);
    return AAUDIO_OK;
}

int aaudio_bridge_stream_wait_for_state_change(void *stream, int32_t input_state,
                                                int32_t *next_state,
                                                int64_t timeout_nanoseconds) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!aaudio_stream_is_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    int64_t waited = 0;
    const int64_t quantum = 1000000ll;
    while (s->state == input_state) {
        if (timeout_nanoseconds == 0 || waited >= timeout_nanoseconds) {
            if (next_state) *next_state = s->state;
            return AAUDIO_ERROR_TIMEOUT;
        }
        int64_t sleep_ns = quantum;
        if (timeout_nanoseconds > 0 && sleep_ns > timeout_nanoseconds - waited)
            sleep_ns = timeout_nanoseconds - waited;
        if (sleep_ns <= 0) break;
        svcSleepThread(sleep_ns);
        waited += sleep_ns;
    }
    if (next_state) *next_state = s->state;
    return s->state == input_state ? AAUDIO_ERROR_TIMEOUT : AAUDIO_OK;
}

int32_t aaudio_bridge_stream_get_state(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->state : AAUDIO_STREAM_STATE_UNINITIALIZED;
}
int32_t aaudio_bridge_stream_get_buffer_capacity(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->buffer_capacity_frames : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_buffer_size(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->buffer_size_frames : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_set_buffer_size(void *stream, int32_t frames) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    if (!aaudio_stream_is_valid(s)) return AAUDIO_ERROR_INVALID_HANDLE;
    if (frames < 0) return AAUDIO_ERROR_OUT_OF_RANGE;
    if (frames > s->buffer_capacity_frames) frames = s->buffer_capacity_frames;
    if (frames < s->frames_per_burst) frames = s->frames_per_burst;
    s->buffer_size_frames = frames;
    return frames;
}
int32_t aaudio_bridge_stream_get_device_id(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->device_id : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_frames_per_burst(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->frames_per_burst : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_session_id(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->session_id : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_xrun_count(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->xruns : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_is_mmap_used(void *stream) {
    (void)stream;

    return 0;
}
int32_t aaudio_bridge_stream_get_sample_rate(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->sample_rate : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_channel_count(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->channels : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_format(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->format : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_direction(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->direction : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_performance_mode(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->performance_mode : AAUDIO_ERROR_INVALID_HANDLE;
}
int32_t aaudio_bridge_stream_get_sharing_mode(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->sharing_mode : AAUDIO_ERROR_INVALID_HANDLE;
}
int64_t aaudio_bridge_stream_get_frames_written(void *stream) {
    NxAAudioStream *s = (NxAAudioStream *)stream;
    return aaudio_stream_is_valid(s) ? s->frames_written : (int64_t)AAUDIO_ERROR_INVALID_HANDLE;
}
