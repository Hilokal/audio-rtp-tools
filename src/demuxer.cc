#include <node_api.h>
#include <uv.h>

#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/threadmessage.h>
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <libavcodec/avcodec.h>
}

#include "demuxer.h"
#include "thread_messages.h"
#include "util.h"
#include "thread_with_promise_result.h"

enum DemumerThreadMode { DEMUXER_MODE_RTP, DEMUXER_MODE_FILE };

struct DemuxerThreadData {
  enum DemumerThreadMode mode;

  int shutdown;
  pthread_t thread;
  AVThreadMessageQueue *output_message_queue;

  // Only valid for DEMUXER_MODE_FILE
  AVThreadMessageQueue *input_message_queue;

  // Only valid for DEMUXER_MODE_RTP
  int should_tick;
  int64_t last_tick;
  int64_t tick_duration;
  char *sdpBase64;
  int should_reset;
};

int stop_rtp_demuxer(DemuxerThreadData *thread_data) {
  thread_data->shutdown = 1;

  int ret;

  // SIGUSR2 is used to interrupt the blocking poll() call inside av_read_frame.
  // We don't use SIGUSR1, because this is used by node for something else. See node.cc
  ret = pthread_kill(thread_data->thread, SIGUSR2);
  if (ret != 0) {
    fprintf(stderr, "pthread_kill error [%d]\n", ret);
  }

  void *value=NULL;
  ret = pthread_join(thread_data->thread, (void**)&value);
  if (ret != 0) {
    fprintf(stderr, "pthread_join error [%d]\n", ret);
    return ret;
  }

  delete thread_data;

  return static_cast<int>(reinterpret_cast<intptr_t>(value));
}

// the ffmpeg thread will poll this function periodically while blocking on IO. If it returns true,
// the I/O will become unblocked.
int interrupt_callback(void *opaque) {
  DemuxerThreadData *thread_data = (DemuxerThreadData*)opaque;

  if (thread_data->shutdown) {
    return 1;
  }

  if (thread_data->tick_duration > 0 && !thread_data->should_tick) {
    int64_t now = av_gettime_relative();
    if (now - thread_data->last_tick > thread_data->tick_duration) {
      thread_data->should_tick = 1;
      thread_data->last_tick = now;
      return 1;
    }
  }

  return 0;
}


// Take from ffmpeg oggparseopus.c. For more information, see https://www.rfc-editor.org/rfc/rfc6716#section-3
static int opus_duration(uint8_t *src, int size)
{
    unsigned nb_frames  = 1;
    unsigned toc        = src[0];
    unsigned toc_config = toc >> 3;
    unsigned toc_count  = toc & 3;
    unsigned frame_size = toc_config < 12 ? FFMAX(480, 960 * (toc_config & 3)) :
                          toc_config < 16 ? 480 << (toc_config & 1) :
                                            120 << (toc_config & 3);
    if (toc_count == 3) {
        if (size<2)
            return AVERROR_INVALIDDATA;
        nb_frames = src[1] & 0x3F;
    } else if (toc_count) {
        nb_frames = 2;
    }

    return frame_size * nb_frames;
}

#define MAX_WARNING_COUNT 10

int readAndWritePacket(DemuxerThreadData *thread_data, AVFormatContext *ifmt_ctx, int stream_idx, int64_t *pts_offset) {
  int ret = 0;

  bool received_start_time = false;
  int64_t first_packet_at = 0;

  int warning_count = 0;
  int64_t prev_pts = AV_NOPTS_VALUE;
  int64_t next_expected_pts = AV_NOPTS_VALUE;

  int64_t pts_correction = AV_NOPTS_VALUE;

  AVPacket *pkt = av_packet_alloc();
  if (pkt == NULL) {
    ret = AVERROR(ENOMEM);
    goto cleanup;
  }

  while (1) {
    av_packet_unref(pkt);
    ret = av_read_frame(ifmt_ctx, pkt);

    if (thread_data->should_tick) {
      if (thread_data->output_message_queue) {
        post_tick_to_thread(thread_data->output_message_queue);
      } else {
        printf("WARNING: output_message_queue is NULL\n");
      }
      thread_data->should_tick = 0;
    }

    if (thread_data->shutdown) {
      ret = 0;
      goto cleanup;
    }

    if (thread_data->should_reset) {
      ret = 0;
      goto cleanup;
    }

    if (ret < 0) {
      if (ret == AVERROR_EXIT) {
        continue;
      } else {
        if (ret != AVERROR_EOF) {
          fprintf(stderr, "av_read_frame fail error [%d]\n", ret);
        }
        goto cleanup;
      }
    }

    if (first_packet_at == 0) {
      first_packet_at = av_gettime();
      post_start_time_local_to_thread(thread_data->output_message_queue, first_packet_at);
    }

    if (ifmt_ctx->start_time_realtime != AV_NOPTS_VALUE && !received_start_time) {
      received_start_time = true;
      post_start_time_to_thread(thread_data->output_message_queue, ifmt_ctx->start_time_realtime);
    }

    // WebRTC M89 on Android sends out empty RTP packets after 5 seconds with duplicate timestamps.
    // The duplicate timestamps cause the downstream muxers to raise an error.
    // https://mediasoup.discourse.group/t/help-debugging-duplicate-rtp-timestamps-from-webrtc/2643
    if (pkt->size == 0) {
      continue;
    }

    // The RTP demuxer doesn't assign a duration to the packets, but the OGG muxer needs this to
    // pack ogg pages properly.
    if (pkt->duration == 0 && pkt->data != NULL) {
      int found_duration = opus_duration(pkt->data, pkt->size);
      if (found_duration < 0) {
        // Malformed packet
        continue;
      }
      pkt->duration = found_duration;
    }

    if (pkt->stream_index != stream_idx) {
      continue;
    }

    // Sometimes packets come out of the demuxer out of order. This is rare, only 3 or 4 times a day in production.
    // We should drop these packets though, because the downstream muxer will choke on out of order packets.
    if (prev_pts != AV_NOPTS_VALUE && pkt->pts < prev_pts) {
      if (warning_count < MAX_WARNING_COUNT) {
        warning_count++;
        fprintf(
          stderr,
          "WARNING: dumuxer received packet with timestamps out of order prev_pts=%lld pts=%lld dts=%lld duration=%lld size=%d ctx=%p\n",
          prev_pts,
          pkt->pts,
          pkt->dts,
          pkt->duration,
          pkt->size,
          ifmt_ctx
        );
      }

      continue;
    }
    prev_pts = pkt->pts;

    // I'm pretty sure this never happens, because the rtp demuxer only calculates a pts, and then copies it to dts.
    if (pkt->pts != pkt->dts) {
      if (warning_count < MAX_WARNING_COUNT) {
        warning_count++;
        fprintf(stderr, "WARNING: dumuxer received packet with mismatched timestamps pts=%lld dts=%lld ctx=%p\n", pkt->pts, pkt->dts, ifmt_ctx);
      }
      continue;
    }

    // It's very common for opus files to have negative pts timestamps for the first packet. This is a bug
    // It's caused when packets that are missing duration attributes are passed into the ffmpeg ogg muxer.
    // OpenAI seems to have this bug as well. We can apply a simple correction here if we detect these packets.
    if (pts_correction == AV_NOPTS_VALUE) {
      if (pkt->pts < 0) {
        pts_correction = -pkt->pts;
      } else {
        pts_correction = 0;
      }
    }
    pkt->pts += pts_correction;
    pkt->dts += pts_correction;

    //fprintf(stderr, "XXX: adding pts_offset: %lld + %lld = %lld\n", pkt->pts, *pts_offset, pkt->pts + *pts_offset);
    pkt->pts += *pts_offset;
    pkt->dts += *pts_offset;
    next_expected_pts = pkt->pts + pkt->duration;

    // When demuxing from an file stream, we want to to block so that we can put back-pressure
    // on the source. For RTP streams, we should just drop the packet if this happens.
    // The message queue should be large enough that this never happens.
    int flags = thread_data->mode == DEMUXER_MODE_RTP ? AV_THREAD_MESSAGE_NONBLOCK : 0;

    ret = post_packet_to_thread(thread_data->output_message_queue, pkt, flags);

    if (ret < 0) {
      if (ret == AVERROR(EAGAIN)) {
        fprintf(stderr, "WARNING: dropping packet because message queue full while posting POST_PACKET [%p]\n", thread_data->output_message_queue);
      } else {
        goto cleanup;
      }
    }
  }

cleanup:
  av_packet_free(&pkt);

  if ((ret == 0 || ret == AVERROR_EOF) && next_expected_pts != AV_NOPTS_VALUE) {
    //fprintf(stderr, "XXX: Adjusting pts_offset by [%d], +%lld (%lld -> %lld)\n", ret, next_expected_pts, *pts_offset, *pts_offset + next_expected_pts);
    *pts_offset = next_expected_pts;
  }

  return ret;
}

int post_file_buffer(DemuxerThreadData *thread_data, AVBufferRef *buffer_ref) {
  int ret = av_thread_message_queue_send(thread_data->input_message_queue, &buffer_ref, AV_THREAD_MESSAGE_NONBLOCK);
  if (ret == AVERROR(EAGAIN)) {
    fprintf(stderr, "WARNING: message queue full while posting buffer to demuxer\n");
  }
  return ret;
}

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
  DemuxerThreadData *thread_data = (DemuxerThreadData *)opaque;

  ThreadMessage thread_message;

  int ret;

  //
  // Read from the message queue
  //
  while (true) {
    ret = av_thread_message_queue_recv(thread_data->input_message_queue, &thread_message, 0);
    if (ret < 0) {
      return ret;
    }

    // If the demuxer is already in the process of initializing, we should skip over any reset requests.
    // Returning an empty buffer might cause the av_open_input() to return AVERROR_EOF.
    if (thread_message.type == OGG_RESET_DEMUXER && thread_data->should_reset) {
      thread_message_free_func(&thread_message);
      continue;
    }

    break;
  }

  //
  // Handle the message
  //
  if (thread_message.type == OGG_BUFFER) {
    if (buf_size < 0 || thread_message.param.buf->size > (size_t)buf_size) {
      fprintf(
        stderr,
        "read_packet called with insufficient buffer_size %d. Need at least %ld.\n",
        buf_size,
        thread_message.param.buf->size
      );

      ret = AVERROR_INVALIDDATA;
    } else {
      memcpy(buf, thread_message.param.buf->data, thread_message.param.buf->size);

      ret = thread_message.param.buf->size;
    }
  } else if (thread_message.type == OGG_RESET_DEMUXER) {
    thread_data->should_reset = true;
    ret = 0;
  } else {
    fprintf(stderr, "Received unexpected message type %d\n", thread_message.type);
    ret = AVERROR_INVALIDDATA;
  }

  thread_message_free_func(&thread_message);
  return ret;
}

int openInputForFile(DemuxerThreadData *thread_data, AVFormatContext **ifmt_ctx) {
  const AVInputFormat *input_format = av_find_input_format("ogg");
  if (!input_format) {
    avformat_free_context(*ifmt_ctx);
    *ifmt_ctx = NULL;
    fprintf(stderr, "Could not find Ogg demuxer.\n");
    return AVERROR_DEMUXER_NOT_FOUND;
  }

  int ret;

  // This buffer isn't used for anything, but it's required by the avio api
  // It will be cleaned up by avio_close(), which is called by avformat_close_input()
  const int MIN_BUFFER_SIZE = 1024 * 8;
  unsigned char *avio_buffer = (unsigned char *)av_malloc(MIN_BUFFER_SIZE);
  if (!avio_buffer) {
    avformat_free_context(*ifmt_ctx);
    *ifmt_ctx = NULL;
    return AVERROR(ENOMEM);
  }

  AVIOContext *avio_ctx = avio_alloc_context(
    avio_buffer,
    MIN_BUFFER_SIZE,
    0, // buffer is not writable
    thread_data,
    read_packet,
    NULL, // write_packet
    NULL // seek
  );

  if (!avio_ctx) {
    av_freep(&avio_buffer);
    avformat_free_context(*ifmt_ctx);
    *ifmt_ctx = NULL;
    fprintf(stderr, "Could not allocate AVIOContext.\n");
    return AVERROR(ENOMEM);
  }

  (*ifmt_ctx)->pb = avio_ctx;
  (*ifmt_ctx)->flags |= AVFMT_FLAG_CUSTOM_IO;

  ret = avformat_open_input(ifmt_ctx, NULL, input_format, NULL);
  if (ret < 0) {
    // If there is an error, the ifmt_ctx is cleaned up but not the avio_ctx.
    av_freep(&avio_ctx->buffer);
    av_freep(&avio_ctx);

    if (ret != AVERROR_EOF) {
      fprintf(stderr, "avformat_open_input fail error [%d] %s\n", ret, av_err2str(ret));
    }
  }

  return ret;
}

int openInputForRtp(DemuxerThreadData *thread_data, AVFormatContext **ifmt_ctx) {
  int ret = 0;

  AVDictionary *options = NULL;
  ret = av_dict_set(&options, "listen_timeout", "-1", 0);
  if (ret < 0) {
    avformat_free_context(*ifmt_ctx);
    *ifmt_ctx = NULL;
    av_freep(&thread_data->sdpBase64);
    fprintf(stderr, "av_dict_set fail error [%d]\n", ret);
    return ret;
  }

  (*ifmt_ctx)->interrupt_callback.callback = interrupt_callback;
  (*ifmt_ctx)->protocol_whitelist = av_strdup("data,udp,rtp");
  // Don't assign the url field. avformat_open_input will do this
  // ifmt_ctx->url = sdpBase64;

  ret = avformat_open_input(ifmt_ctx, thread_data->sdpBase64, NULL, &options);
  if (ret < 0) {
    fprintf(stderr, "avformat_open_input fail error [%d]\n", ret);
  }

  av_dict_free(&options);

  // This string has been copied into ifmt_ctx->url, so it's no longer needed
  av_freep(&thread_data->sdpBase64);

  return ret;
}


int initInputFormatContext(DemuxerThreadData *thread_data, int *stream_idx, AVFormatContext **ifmt_ctx) {
  const AVCodec *decoder;
  int ret = 0;

  *ifmt_ctx = avformat_alloc_context();
  if (*ifmt_ctx == NULL) {
    ret = AVERROR(ENOMEM);
    fprintf(stderr, "avformat_alloc_context fail error [%d]\n", ret);
    return ret;
  }
  (**ifmt_ctx).interrupt_callback.opaque = thread_data;

  if (thread_data->mode == DEMUXER_MODE_RTP) {
    ret = openInputForRtp(thread_data, ifmt_ctx);
  } else {
    ret = openInputForFile(thread_data, ifmt_ctx);
  }

  if (ret < 0) {
    return ret;
  }

  ret = av_find_best_stream(*ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if (ret < 0) {
    avformat_free_context(*ifmt_ctx);
    *ifmt_ctx = NULL;
    fprintf(stderr, "av_find_best_stream fail error [%d]\n", ret);
    return ret;
  }
  *stream_idx = ret;

  if (decoder->id != AV_CODEC_ID_OPUS) {
    printf("WARNING: Expected audio stream to be AV_CODEC_ID_OPUS (%d), but is %d\n", AV_CODEC_ID_OPUS, decoder->id);
  }

  return 0;
}

void custom_io_close_input(AVFormatContext **ifmt_ctx) {
  if (ifmt_ctx != NULL && *ifmt_ctx != NULL) {
    // The "File" version of the demuxer uses a custom IOContext.
    if ((*ifmt_ctx)->flags & AVFMT_FLAG_CUSTOM_IO) {
      if ((*ifmt_ctx)->pb != NULL) {
        av_freep(&((*ifmt_ctx)->pb->buffer));
        av_freep(&((*ifmt_ctx)->pb));
      } else {
        fprintf(stderr, "WARNING: custom_io_close_input pb is NULL\n");
      }
    }

    avformat_close_input(ifmt_ctx);
  }
}


static int ThreadMain(DemuxerThreadData *thread_data) {
  set_thread_name("demuxer");

  int ret = 0;
  int stream_idx = -1;
  int64_t pts_offset = 0;

  // Setting this flag to true will cause the read_packet function to ignore any
  // reset requests that are received while the demuxer is initializing.
  thread_data->should_reset = true;

  AVFormatContext *ifmt_ctx = NULL;

  ret = initInputFormatContext(thread_data, &stream_idx, &ifmt_ctx);
  if (ret != 0) {
    goto cleanup;
  }
  thread_data->should_reset = false;

  ret = post_codec_parameters_to_thread(
    thread_data->output_message_queue,
    ifmt_ctx->streams[stream_idx]->codecpar
  );
  if (ret < 0) {
    goto cleanup;
  }

  while (true) {
    if (thread_data->shutdown) {
      break;
    }

    // receive AVpackets
    ret = readAndWritePacket(thread_data, ifmt_ctx, stream_idx, &pts_offset);
    if (ret < 0) {
      goto cleanup;
    }

    if (thread_data->should_reset) {
      custom_io_close_input(&ifmt_ctx);
      ret = initInputFormatContext(thread_data, &stream_idx, &ifmt_ctx);
      if (ret != 0) {
        goto cleanup;
      }

      // Note, this flag should remain true for the duration of the initInputFormatContext so
      // that it will ignore any reset requests that are received while the demuxer is initializing.
      thread_data->should_reset = false;
    }
  }

cleanup:

  if (ifmt_ctx != NULL) {
    custom_io_close_input(&ifmt_ctx);
  }

  if (thread_data->mode == DEMUXER_MODE_RTP) {
    av_thread_message_queue_set_err_recv(thread_data->output_message_queue, AVERROR_EOF);
  }

  //check_for_memory_leaks();

  if (ret == AVERROR_EOF) {
    return 0;
  } else {
    return ret;
  }
}

int ThreadMainFile(AVThreadMessageQueue *message_queue, uv_async_t *buffer_ready_async, const DemuxerThreadData &params) {
  DemuxerThreadData params2(params);
  params2.input_message_queue = message_queue;
  return ThreadMain(&params2);
}

void *ThreadMainRtp(void *opaque) {
  DemuxerThreadData *thread_data = (DemuxerThreadData *)opaque;
  int ret = ThreadMain(thread_data);
  return reinterpret_cast<void*>(static_cast<intptr_t>(ret));
}


int start_rtp_demuxer(char *sdp_base_64, int64_t tick_duration, AVThreadMessageQueue *output_message_queue, DemuxerThreadData **thread_data) {
  int ret;

  pthread_attr_t attr;
  ret = pthread_attr_init(&attr);
  if (ret != 0) {
    fprintf(stderr, "pthread_attr_init fail error num [%d]\n", ret);
    return ret;
  }

  size_t stack_size = get_stack_size_for_thread("DEMUXER");
  if (stack_size != 0) {
    ret = pthread_attr_setstacksize(&attr, stack_size);
    if (ret != 0) {
      // This isn't a fatal error. Don't return
      fprintf(stderr, "pthread_attr_setstacksize fail error num [%d]\n", ret);
    }
  }

  (*thread_data) = new DemuxerThreadData();
  (*thread_data)->sdpBase64 = sdp_base_64;
  (*thread_data)->output_message_queue = output_message_queue;
  (*thread_data)->tick_duration = tick_duration;
  (*thread_data)->mode = DEMUXER_MODE_RTP;
  (*thread_data)->shutdown = 0;
  (*thread_data)->should_tick = 0;
  (*thread_data)->should_reset = 0;
  (*thread_data)->last_tick = av_gettime_relative();

  ret = pthread_create(&(*thread_data)->thread, &attr, ThreadMainRtp, (void *)*thread_data);
  if (ret != 0) {
    av_freep(&(*thread_data)->sdpBase64);
    delete (*thread_data);
    *thread_data = NULL;
    fprintf(stderr, "pthread_create fail error num [%d]\n", ret);
    return ret;
  }

  return ret;
}

// I've noticed when testing with long TTS responses that the message queue would fill up and generate warnings.
#define FILE_DEMUXER_MESSAGE_QUEUE_SIZE 2048

napi_status start_file_demuxer(napi_env env, napi_value js_output_message_queue, napi_value abort_signal, napi_value *external, napi_value *promise) {
  AVThreadMessageQueue *output_message_queue;
  napi_status status = napi_get_value_external(env, js_output_message_queue, (void **)&output_message_queue);
  if (status != napi_ok) {
    return status;
  }

  DemuxerThreadData thread_data;
  thread_data.output_message_queue = output_message_queue;
  thread_data.mode = DEMUXER_MODE_FILE;

  // Unused in file demuxer
  thread_data.sdpBase64 = NULL;
  thread_data.shutdown = 0;
  thread_data.should_tick = 0;
  thread_data.should_reset = 0;
  thread_data.last_tick = av_gettime_relative();

  size_t stack_size = get_stack_size_for_thread("DEMUXER");

  return start_thread_with_promise_result(
    env,
    ThreadMainFile,
    thread_data,
    abort_signal,
    js_output_message_queue,
    stack_size,
    FILE_DEMUXER_MESSAGE_QUEUE_SIZE,
    external,
    NULL,
    promise
  );
}
