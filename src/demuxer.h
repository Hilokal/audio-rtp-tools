#pragma once

#include <node_api.h>

extern "C" {
#include <libavutil/threadmessage.h>
#include <libavformat/avformat.h>
}

#include "rtp_stats.h"

struct DemuxerThreadData;

// max_delay is how long, in microseconds, to wait for a missing RTP packet
// before moving on without it; a negative value keeps the libavformat default
// (100ms). stats may be NULL, and must outlive the demuxer otherwise.
int start_rtp_demuxer(char *sdp_base_64, int64_t tick_duration, int64_t max_delay, RtpReceiveStats *stats, AVThreadMessageQueue *output_message_queue, DemuxerThreadData **thread_data);
napi_status start_file_demuxer(napi_env env, napi_value js_output_message_queue, napi_value abort_signal, napi_value *external, napi_value *promise);
int post_file_buffer(DemuxerThreadData *thread_data, AVBufferRef *buffer_ref);
int stop_rtp_demuxer(DemuxerThreadData *output_message_queue);