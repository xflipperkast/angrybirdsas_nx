#ifndef ASNX_AAUDIO_BRIDGE_H
#define ASNX_AAUDIO_BRIDGE_H

#include <stdint.h>

int aaudio_bridge_create_stream_builder(void **builder);
int aaudio_bridge_builder_delete(void *builder);
void aaudio_bridge_builder_set_buffer_capacity(void *builder, int32_t frames);
void aaudio_bridge_builder_set_channel_count(void *builder, int32_t channels);
void aaudio_bridge_builder_set_data_callback(void *builder, void *callback, void *user_data);
void aaudio_bridge_builder_set_device_id(void *builder, int32_t device_id);
void aaudio_bridge_builder_set_direction(void *builder, int32_t direction);
void aaudio_bridge_builder_set_error_callback(void *builder, void *callback, void *user_data);
void aaudio_bridge_builder_set_format(void *builder, int32_t format);
void aaudio_bridge_builder_set_frames_per_callback(void *builder, int32_t frames);
void aaudio_bridge_builder_set_performance_mode(void *builder, int32_t mode);
void aaudio_bridge_builder_set_sample_rate(void *builder, int32_t sample_rate);
void aaudio_bridge_builder_set_session_id(void *builder, int32_t session_id);
void aaudio_bridge_builder_set_sharing_mode(void *builder, int32_t mode);
int aaudio_bridge_builder_open_stream(void *builder, void **stream_out);

int aaudio_bridge_stream_close(void *stream);
int aaudio_bridge_stream_request_start(void *stream);
int aaudio_bridge_stream_request_stop(void *stream);
int aaudio_bridge_stream_wait_for_state_change(void *stream, int32_t input_state,
                                                int32_t *next_state,
                                                int64_t timeout_nanoseconds);
int32_t aaudio_bridge_stream_get_state(void *stream);
int32_t aaudio_bridge_stream_get_buffer_capacity(void *stream);
int32_t aaudio_bridge_stream_get_buffer_size(void *stream);
int32_t aaudio_bridge_stream_set_buffer_size(void *stream, int32_t frames);
int32_t aaudio_bridge_stream_get_device_id(void *stream);
int32_t aaudio_bridge_stream_get_frames_per_burst(void *stream);
int32_t aaudio_bridge_stream_get_session_id(void *stream);
int32_t aaudio_bridge_stream_get_xrun_count(void *stream);
int32_t aaudio_bridge_stream_is_mmap_used(void *stream);
int32_t aaudio_bridge_stream_get_sample_rate(void *stream);
int32_t aaudio_bridge_stream_get_channel_count(void *stream);
int32_t aaudio_bridge_stream_get_format(void *stream);
int32_t aaudio_bridge_stream_get_direction(void *stream);
int32_t aaudio_bridge_stream_get_performance_mode(void *stream);
int32_t aaudio_bridge_stream_get_sharing_mode(void *stream);
int64_t aaudio_bridge_stream_get_frames_written(void *stream);
void aaudio_bridge_diag_snapshot(const char *reason);


#endif
