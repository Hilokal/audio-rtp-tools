#pragma once

extern "C" {
#include <libavutil/threadmessage.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <uv.h>
}

enum ThreadMessageType {
  POST_PACKET, POST_START_TIME_REALTIME, POST_START_TIME_LOCALTIME, POST_CODEC_PARAMETERS, TICK,

  // Used for streaming OGG buffers from text-to-speech engine
  OGG_BUFFER,
  OGG_EOF,
  OGG_RESET_DEMUXER,

  // Used for streaming PCM buffers for encoding
  POST_PCM_BUFFER,

  // Runtime encoder config changes
  SET_ENCODER_BITRATE,
  SET_ENCODER_FEC,
  SET_ENCODER_PACKET_LOSS_PERC,
};

union ThreadMessageParameter {
  AVPacket *pkt;
  int64_t start_time_realtime;
  int64_t start_time_localtime;
  AVCodecParameters *codecpar;
  AVBufferRef *buf;
  int32_t int_value;
};

struct ThreadMessage {
  enum ThreadMessageType type;
  union ThreadMessageParameter param;
  uv_async_t *async;
};

int post_packet_to_thread(AVThreadMessageQueue *message_queue, AVPacket *pkt, int flags);
int post_start_time_to_thread(AVThreadMessageQueue *message_queue, int64_t start_time_realtime);
int post_start_time_local_to_thread(AVThreadMessageQueue *message_queue, int64_t start_time_localtime);
int post_codec_parameters_to_thread(AVThreadMessageQueue *message_queue, AVCodecParameters *codecpar);
int post_tick_to_thread(AVThreadMessageQueue *message_queue);
int post_ogg_buffer_to_thread(AVThreadMessageQueue *message_queue, void *buffer, size_t buffer_length);
int post_ogg_reset_demuxer_to_thread(AVThreadMessageQueue *message_queue);
int post_pcm_buffer_to_thread(AVThreadMessageQueue *message_queue, void *buffer, size_t buffer_length);
int post_set_bitrate_to_thread(AVThreadMessageQueue *mq, int32_t bitrate);
int post_set_fec_to_thread(AVThreadMessageQueue *mq, bool enable);
int post_set_packet_loss_perc_to_thread(AVThreadMessageQueue *mq, int32_t percent);


// This should be sent to av_thread_message_queue_set_free_func after initialization
void thread_message_free_func(void *thread_message);
