#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thread_messages.h"

int post_packet_to_thread(AVThreadMessageQueue *message_queue, AVPacket *pkt, int flags) {
  AVPacket *clone = av_packet_clone(pkt);

  ThreadMessage thread_message = {
    .type = POST_PACKET,
    .param = {
      .pkt = clone
    },
    .async = NULL
  };

  int ret = av_thread_message_queue_send(message_queue, &thread_message, flags);

  if (ret != 0) {
    av_packet_free(&clone);
  }

  return ret;
}

int post_ogg_buffer_to_thread(AVThreadMessageQueue *message_queue, void *buffer, size_t buffer_length) {
  AVBufferRef *buffer_ref = av_buffer_alloc(buffer_length);
  if (buffer_ref == NULL) {
    return AVERROR(ENOMEM);
  }
  memcpy(buffer_ref->data, buffer, buffer_length);

  ThreadMessage thread_message = {
    .type = OGG_BUFFER,
    .param = {
      .buf = buffer_ref
    },
    .async = NULL
  };

  int ret = av_thread_message_queue_send(message_queue, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);

  if (ret != 0) {
    if (ret == AVERROR(EAGAIN)) {
      fprintf(stderr, "WARNING: Dropping OGG buffer because demuxer queue full [%p]\n", message_queue);
    }

    av_buffer_unref(&buffer_ref);
  }

  return ret;
}

int post_ogg_reset_demuxer_to_thread(AVThreadMessageQueue *message_queue) {
  ThreadMessage thread_message = {
    .type = OGG_RESET_DEMUXER,
    .param = {
      .buf = NULL
    },
    .async = NULL
  };

  int ret = av_thread_message_queue_send(message_queue, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);

  if (ret == AVERROR(EAGAIN)) {
    fprintf(stderr, "WARNING: message queue full while posting OGG_RESET_DEMUXER [%p]\n", message_queue);
  }

  return ret;
}

int post_pcm_buffer_to_thread(AVThreadMessageQueue *message_queue, void *buffer, size_t buffer_length) {
  AVBufferRef *buffer_ref = av_buffer_alloc(buffer_length);
  if (buffer_ref == NULL) {
    return AVERROR(ENOMEM);
  }
  memcpy(buffer_ref->data, buffer, buffer_length);

  ThreadMessage thread_message = {
    .type = POST_PCM_BUFFER,
    .param = {
      .buf = buffer_ref
    },
    .async = NULL
  };

  int ret = av_thread_message_queue_send(message_queue, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);

  if (ret != 0) {
    if (ret == AVERROR(EAGAIN)) {
      fprintf(stderr, "WARNING: Dropping PCM buffer because encoder queue full [%p]\n", message_queue);
    }
    av_buffer_unref(&buffer_ref);
  }

  return ret;
}

int post_set_bitrate_to_thread(AVThreadMessageQueue *mq, int32_t bitrate) {
  ThreadMessage thread_message = {
    .type = SET_ENCODER_BITRATE,
    .param = {
      .int_value = bitrate
    },
    .async = NULL
  };

  return av_thread_message_queue_send(mq, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);
}

int post_set_fec_to_thread(AVThreadMessageQueue *mq, bool enable) {
  ThreadMessage thread_message = {
    .type = SET_ENCODER_FEC,
    .param = {
      .int_value = enable ? 1 : 0
    },
    .async = NULL
  };

  return av_thread_message_queue_send(mq, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);
}

int post_set_packet_loss_perc_to_thread(AVThreadMessageQueue *mq, int32_t percent) {
  ThreadMessage thread_message = {
    .type = SET_ENCODER_PACKET_LOSS_PERC,
    .param = {
      .int_value = percent
    },
    .async = NULL
  };

  return av_thread_message_queue_send(mq, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);
}

void thread_message_free_func(void *opaque) {
  ThreadMessage *thread_message = (ThreadMessage *)opaque;
  if (thread_message->type == POST_CODEC_PARAMETERS) {
    avcodec_parameters_free(&thread_message->param.codecpar);
  } else if (thread_message->type == POST_PACKET) {
    av_packet_free(&thread_message->param.pkt);
  } else if (thread_message->type == OGG_BUFFER || thread_message->type == POST_PCM_BUFFER) {
    av_buffer_unref(&thread_message->param.buf);
  }
}

int post_start_time_to_thread(AVThreadMessageQueue *message_queue, int64_t start_time_realtime) {
  ThreadMessage thread_message = {
    .type = POST_START_TIME_REALTIME,
    .param = {
      .start_time_realtime = start_time_realtime
    },
    .async = NULL
  };

  int ret = av_thread_message_queue_send(message_queue, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);

  if (ret == AVERROR(EAGAIN)) {
    fprintf(stderr, "WARNING: message queue full while posting POST_START_TIME_REALTIME [%p]\n", message_queue);
  }

  return ret;
}

int post_start_time_local_to_thread(AVThreadMessageQueue *message_queue, int64_t start_time_localtime) {
  ThreadMessage thread_message = {
    .type = POST_START_TIME_LOCALTIME,
    .param = {
      .start_time_localtime = start_time_localtime
    },
    .async = NULL
  };

  int ret = av_thread_message_queue_send(message_queue, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);

  if (ret == AVERROR(EAGAIN)) {
    fprintf(stderr, "WARNING: message queue full while posting POST_START_TIME_LOCALTIME[%p]\n", message_queue);
  }

  return ret;
}

int post_tick_to_thread(AVThreadMessageQueue *message_queue) {
  ThreadMessage thread_message = {
    .type = TICK,
    .param = {
      .pkt = NULL
    },
    .async = NULL
  };

  int ret = av_thread_message_queue_send(message_queue, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);
  if (ret == AVERROR(EAGAIN)) {
    fprintf(stderr, "WARNING: message queue full while posting TICK [%p]\n", message_queue);
  }
  return ret;
}

int post_codec_parameters_to_thread(AVThreadMessageQueue *message_queue, AVCodecParameters *codecpar) {
  // Should be freed by the receiving thread
  AVCodecParameters *copy = avcodec_parameters_alloc();

  int ret = avcodec_parameters_copy(copy, codecpar);
  if (ret < 0) {
    avcodec_parameters_free(&copy);
    return ret;
  }

  ThreadMessage thread_message = {
    .type = POST_CODEC_PARAMETERS,
    .param = {
      .codecpar = copy
    }
  };

  ret = av_thread_message_queue_send(message_queue, &thread_message, 0);
  if (ret < 0) {
    avcodec_parameters_free(&copy);
  }
  return ret;
}
