#include <node_api.h>

#include "save_audio_thread.h"
#include "demuxer.h"
#include "thread_messages.h"
#include "node_errors.h"
#include "util.h"
#include "thread_with_promise_result.h"

extern "C" {
  #include <libavformat/avformat.h>
  #include <libavcodec/avcodec.h>
  #include <libavutil/threadmessage.h>
}

static int ThreadMain(AVThreadMessageQueue *message_queue, uv_async_t *buffer_ready_async, uv_async_t *drain_async, const SaveAudioThreadParams &thread_data) {
  int thread_ret = 0;
  int demux_ret = 0;

  set_thread_name("save_audio_thread");

  char *outputPath = thread_data.outputPath;

  ThreadMessage thread_message;
  AVCodecParameters *codecpar = NULL;
  DemuxerThreadData *demuxer_thread = NULL;

  AVFormatContext *output_ctx = NULL;
  AVStream *out_stream = NULL;
  bool header_written = false;
  int64_t total_packets = 0;

  AVRational input_time_base = {0, 0};

  thread_ret = start_rtp_demuxer(thread_data.sdpBase64, 0, message_queue, &demuxer_thread);
  if (thread_ret != 0) {
    goto cleanup_thread;
  }

  while (true) {
    thread_ret = av_thread_message_queue_recv(message_queue, &thread_message, 0);

    if (thread_ret < 0) {
      if (thread_ret == AVERROR_EOF) {
        thread_ret = 0;
      }
      goto cleanup_thread;
    }

    if (thread_message.type == POST_CODEC_PARAMETERS) {
      avcodec_parameters_free(&codecpar);
      codecpar = thread_message.param.codecpar;

      // Derive the input timebase from the codec's sample rate
      input_time_base = (AVRational){1, codecpar->sample_rate};

      // Set up WebM output context when we receive codec parameters
      if (output_ctx == NULL) {
        int ret = avformat_alloc_output_context2(&output_ctx, NULL, "webm", outputPath);
        if (!output_ctx || ret < 0) {
          fprintf(stderr, "save_audio: could not create output context [%d]\n", ret);
          thread_ret = ret < 0 ? ret : AVERROR(ENOMEM);
          goto cleanup_thread;
        }

        out_stream = avformat_new_stream(output_ctx, NULL);
        if (!out_stream) {
          fprintf(stderr, "save_audio: could not create output stream\n");
          thread_ret = AVERROR(ENOMEM);
          goto cleanup_thread;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, codecpar);
        if (ret < 0) {
          fprintf(stderr, "save_audio: could not copy codec parameters [%d]\n", ret);
          thread_ret = ret;
          goto cleanup_thread;
        }

        ret = avio_open2(&output_ctx->pb, outputPath, AVIO_FLAG_WRITE, NULL, NULL);
        if (ret < 0) {
          fprintf(stderr, "save_audio: avio_open2 failed [%d]\n", ret);
          thread_ret = ret;
          goto cleanup_thread;
        }

        ret = avformat_write_header(output_ctx, NULL);
        if (ret < 0) {
          fprintf(stderr, "save_audio: avformat_write_header failed [%d]\n", ret);
          thread_ret = ret;
          goto cleanup_thread;
        }

        header_written = true;
      }
    } else if (thread_message.type == POST_PACKET) {
      AVPacket *pkt = thread_message.param.pkt;

      if (output_ctx != NULL && header_written) {
        av_packet_rescale_ts(pkt, input_time_base, out_stream->time_base);
        int ret = av_write_frame(output_ctx, pkt);
        if (ret < 0) {
          fprintf(stderr, "save_audio: av_write_frame failed [%d]\n", ret);
          av_packet_free(&pkt);
          thread_ret = ret;
          goto cleanup_thread;
        }
        total_packets++;
      }

      av_packet_free(&pkt);
    }
    // Ignore other message types (TICK, POST_START_TIME_*, etc.)
  }

cleanup_thread:
  if (header_written && output_ctx != NULL) {
    av_write_trailer(output_ctx);
  }

  if (output_ctx != NULL) {
    avio_closep(&output_ctx->pb);
    avformat_free_context(output_ctx);
    output_ctx = NULL;
  }

  avcodec_parameters_free(&codecpar);
  av_freep(&outputPath);

  if (total_packets > 0) {
    printf("save_audio: wrote %lld packets\n", (long long)total_packets);
  }

  av_thread_message_queue_set_err_send(message_queue, AVERROR_EOF);

  demux_ret = stop_rtp_demuxer(demuxer_thread);
  if (demux_ret != 0) {
    return demux_ret;
  } else {
    return thread_ret;
  }
}

napi_status start_save_audio_thread(napi_env env, const SaveAudioThreadParams &params, napi_value abort_signal, napi_value *external, napi_value *promise) {
  size_t stack_size = get_stack_size_for_thread("MUXER");

  return start_thread_with_promise_result<SaveAudioThreadParams>(
    env, ThreadMain, params, abort_signal,
    NULL, stack_size, DEFAULT_MESSAGE_QUEUE_SIZE,
    external, NULL, NULL, promise
  );
}
