#pragma once

#include <node_api.h>

extern "C" {
#include <libavutil/threadmessage.h>
#include <libavformat/avformat.h>
}

struct DemuxerThreadData;

int start_rtp_demuxer(char *sdp_base_64, int64_t tick_duration, AVThreadMessageQueue *output_message_queue, DemuxerThreadData **thread_data);
napi_status start_file_demuxer(napi_env env, napi_value js_output_message_queue, napi_value abort_signal, napi_value *external, napi_value *promise);
int post_file_buffer(DemuxerThreadData *thread_data, AVBufferRef *buffer_ref);
int stop_rtp_demuxer(DemuxerThreadData *output_message_queue);