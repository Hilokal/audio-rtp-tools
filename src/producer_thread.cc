#include <node_api.h>
#include <uv.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/threadmessage.h>
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <libavcodec/avcodec.h>
}

#include "util.h"
#include "producer_thread.h"
#include "thread_with_promise_result.h"
#include "time_util.h"

#define OPUS_SAMPLE_RATE 48000

// This is the maximum amount of audio we will send into the future. Previously,
// I had this set to half a second, but this was causing playback to happen too fast.
// Setting it to 1/10 of a second seems to work well.
#define MAX_FUTURE (OPUS_SAMPLE_RATE / 10)

static int ThreadMain(AVThreadMessageQueue *message_queue, const ProducerThreadParams &params) {
  AVFormatContext *output_ctx = NULL;
  AVStream *out_stream = NULL;
  const AVCodec *codec = NULL;
  ThreadMessage thread_message;
  AVDictionary *options = NULL;

  // Copy to a non-const point because it's easier to track when it's freed
  char *url = params.url;

  int64_t stream_start = av_gettime_relative();
  int64_t rebase_pts = AV_NOPTS_VALUE;
  int64_t last_pts = AV_NOPTS_VALUE;
  int64_t next_expected_pts = AV_NOPTS_VALUE;
  int ret = 0;

  // The options dictionary will take ownership of all the strdup'd strings
  av_dict_set(&options, "ssrc", params.ssrc, AV_DICT_DONT_STRDUP_VAL);
  av_dict_set(&options, "payload_type", params.payloadType, AV_DICT_DONT_STRDUP_VAL);
  av_dict_set(&options, "cname", params.cname, AV_DICT_DONT_STRDUP_VAL);

  if (params.cryptoSuite) {
    av_dict_set(&options, "srtp_out_suite", params.cryptoSuite, AV_DICT_DONT_STRDUP_VAL);
  }
  if (params.keyBase64) {
    av_dict_set(&options, "srtp_out_params", params.keyBase64, AV_DICT_DONT_STRDUP_VAL);
  }

  avformat_alloc_output_context2(&output_ctx, NULL, "rtp", url);
  if (!output_ctx) {
    fprintf(stderr, "Could not create output context.\n");
    ret = AVERROR(ENOMEM);
    goto cleanup;
  }

  codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
  if (!codec) {
    fprintf(stderr, "avcodec_find_encoder failed\n");
    ret = AVERROR(EILSEQ); // Same as AVFORMAT_NOFMT
    goto cleanup;
  }

  out_stream = avformat_new_stream(output_ctx, codec);
  if (!out_stream) {
    ret = AVERROR(ENOMEM);
    goto cleanup;
  }

  // Cleaned up by avformat_free_context
  out_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  out_stream->codecpar->codec_id = AV_CODEC_ID_OPUS;
  out_stream->codecpar->sample_rate = OPUS_SAMPLE_RATE;
  out_stream->codecpar->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  out_stream->codecpar->bit_rate = 64000;

  // The RTP muxer doesn't need this
  out_stream->codecpar->extradata = NULL;
  out_stream->codecpar->extradata_size = 0;

  ret = avio_open2(&output_ctx->pb, url, AVIO_FLAG_WRITE, NULL, &options);
  if (ret < 0) {
    fprintf(stderr, "avio_open2 failed [%d]\n", ret);
    goto cleanup;
  }

  av_freep(&url);

  ret = avformat_write_header(output_ctx, &options);
  if (ret < 0) {
    fprintf(stderr, "avformat_write_header failed [%d]\n", ret);
    goto cleanup;
  }

  while (true) {
    ret = av_thread_message_queue_recv(message_queue, &thread_message, 0);
    if (ret < 0) {
      // This error is expected when shutting down
      if (ret == AVERROR_EOF) {
        av_write_trailer(output_ctx);
        ret = 0;
      }
      goto cleanup;
    }

    if (thread_message.type == POST_PACKET) {
      AVPacket *pkt = thread_message.param.pkt;
      int64_t now = av_gettime_relative();
      int64_t now_pts = av_rescale(OPUS_SAMPLE_RATE, (now - stream_start), MICROSECONDS);

      if (rebase_pts == AV_NOPTS_VALUE || pkt->pts <= last_pts) {
        // We allow up to MAX_FUTURE to be sent ahead of time, so it's possible that the
        // last_rebased_pts is greater than now_pts.
        if (next_expected_pts != AV_NOPTS_VALUE && next_expected_pts > now_pts) {
          int64_t max_pts = now_pts + MAX_FUTURE;
          if (next_expected_pts > max_pts) {
            fprintf(stderr, "WARNING: next_expected_pts is too far ahead of now_pts. %lld > %lld\n", next_expected_pts, now_pts);
            now_pts = max_pts;
          } else {
            now_pts = next_expected_pts;
          }
        }

        fprintf(
          stderr,
          "resetting to wallclock time: old_rebase_pts: %lld, new_rebase_pts: %lld, incoming pts: %lld <= %lld\n",
          rebase_pts, now_pts, pkt->pts, last_pts
        );
        rebase_pts = now_pts;
        // Reset next_expected_pts so the drop check (below) doesn't compare
        // the new stream's PTS against the old stream's expected PTS.
        next_expected_pts = AV_NOPTS_VALUE;
      }

      last_pts = pkt->pts;

      pkt->pts += rebase_pts;
      pkt->dts += rebase_pts;

      int64_t future = pkt->pts - now_pts;
      if (future > MAX_FUTURE) {
        int64_t sleep_for = av_rescale(MICROSECONDS, future - MAX_FUTURE, OPUS_SAMPLE_RATE);
        //fprintf(stderr, "Delaying packet by %f\n", sleep_for / (float)MICROSECONDS);
        av_usleep(sleep_for);
      }

      // There is a small chance (although I can't reproduce it) that that if the user stops playback on one
      // track and then immediately starts another, that the pts can go backwards.
      // In this rare case, we should drop the packet. Sending pts packets out of order will cause the
      // muxer to stop, so we must avoid this.
      if (next_expected_pts != AV_NOPTS_VALUE && pkt->pts < next_expected_pts) {
        fprintf(stderr, "WARNING: dropping packet with pts < next_expected_pts. %lld <= %lld\n", pkt->pts, next_expected_pts);
        thread_message_free_func(&thread_message);
        continue;
      }

      next_expected_pts = pkt->pts + pkt->duration;

      ret = av_write_frame(output_ctx, pkt);
      if (ret < 0) {
        fprintf(stderr, "av_write_frame failed [%d]\n", ret);
        thread_message_free_func(&thread_message);
        goto cleanup;
      }
    }

    thread_message_free_func(&thread_message);
  }

cleanup:

  av_dict_free(&options);

  av_freep(&url);

  if (output_ctx != NULL) {
    avio_closep(&output_ctx->pb);
    avformat_free_context(output_ctx);
    output_ctx = NULL;
  }

  return ret;
}

// The producer is limited to sending packets in real-time, so we need a larger message queue
// to handle the backpressure.
#define PRODUCER_MESSAGE_QUEUE_SIZE 8192

static int ThreadMainWithPromise(AVThreadMessageQueue *message_queue, uv_async_t *buffer_ready_async, const ProducerThreadParams &params) {
    return ThreadMain(message_queue, params);
}

napi_status start_producer_thread(napi_env env, ProducerThreadParams &params, napi_value abort_signal, napi_value *external, napi_value *promise) {
  size_t stack_size = get_stack_size_for_thread("PRODUCER");

  napi_status status = start_thread_with_promise_result<ProducerThreadParams>(env, ThreadMainWithPromise, params, abort_signal, NULL, stack_size, PRODUCER_MESSAGE_QUEUE_SIZE, external, NULL, promise);

  if (status != napi_ok) {
    av_freep(&params.url);
    av_freep(&params.ssrc);
    av_freep(&params.payloadType);
    av_freep(&params.cname);
    av_freep(&params.cryptoSuite);
    av_freep(&params.keyBase64);
  }

  return status;
}

static void *ThreadMainRawWrapper(void *opaque) {
  ProducerThreadData* thread_data = (ProducerThreadData*)opaque;

  int ret = ThreadMain(thread_data->message_queue, thread_data->params);
  thread_data->thread_ret = ret;

  av_thread_message_queue_set_err_send(thread_data->message_queue, AVERROR_EOF);
  av_thread_message_queue_set_err_recv(thread_data->message_queue, AVERROR_EOF);

  return 0;
}

int start_producer_thread_raw(
  const ProducerThreadParams &params,
  ProducerThreadData **thread_data
) {
  int ret;

  pthread_attr_t attr;
  ret = pthread_attr_init(&attr);
  if (ret != 0) {
    fprintf(stderr, "pthread_attr_init fail error num [%d]\n", ret);
    return ret;
  }

  size_t stack_size = get_stack_size_for_thread("PRODUCER");
  if (stack_size != 0) {
    ret = pthread_attr_setstacksize(&attr, stack_size);
    if (ret != 0) {
      fprintf(stderr, "pthread_attr_setstacksize fail error num [%d]\n", ret);
    }
  }

  *thread_data = new ProducerThreadData();

  ret = av_thread_message_queue_alloc(&(*thread_data)->message_queue, PRODUCER_MESSAGE_QUEUE_SIZE, sizeof(ThreadMessage));
  if (ret != 0) {
    delete *thread_data;
    *thread_data = NULL;
    fprintf(stderr, "producer_thread: failed to alloc producer queue [%d]\n", ret);
    return ret;
  }
  av_thread_message_queue_set_free_func((*thread_data)->message_queue, thread_message_free_func);

  (*thread_data)->params = params;

  ret = pthread_create(&(*thread_data)->thread, &attr, ThreadMainRawWrapper, (void *)*thread_data);
  if (ret != 0) {
    av_thread_message_queue_free(&(*thread_data)->message_queue);
    delete *thread_data;
    *thread_data = NULL;
    fprintf(stderr, "pthread_create fail error num [%d]\n", ret);
    return ret;
  }

  pthread_attr_destroy(&attr);
  return 0;
}

int stop_producer_thread_raw(ProducerThreadData *thread_data) {
  if (thread_data == NULL) {
    return 0;
  }

  av_thread_message_queue_set_err_send(thread_data->message_queue, AVERROR_EOF);
  av_thread_message_queue_set_err_recv(thread_data->message_queue, AVERROR_EOF);

  void *value = NULL;
  int ret = pthread_join(thread_data->thread, &value);
  if (ret != 0) {
    fprintf(stderr, "pthread_join error [%d]\n", ret);
    delete thread_data;
    return ret;
  }

  int thread_ret = thread_data->thread_ret;

  av_thread_message_queue_free(&thread_data->message_queue);

  delete thread_data;

  return thread_ret;
}
