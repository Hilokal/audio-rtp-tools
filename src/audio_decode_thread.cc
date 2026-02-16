#include <node_api.h>

#include "audio_decode_thread.h"
#include "buffer_ready_node_callback.h"
#include "demuxer.h"
#include "thread_messages.h"
#include "node_errors.h"
#include "util.h"
#include "thread_with_promise_result.h"
#include "time_util.h"

extern "C" {
  #include "libavutil/time.h"
  #include <opus/opus.h>
}

// Opus RTP timestamps are always at 48kHz
#define OUTPUT_SAMPLE_RATE 48000
#define OPUS_FRAME_DURATION_MS 20
#define OPUS_MAX_FRAME_SIZE (960 * 6)  // Max frame size for opus

// Copied from libavcodec/libopus.c
static int ff_opus_error_to_averror(int err) {
  switch (err) {
    case OPUS_BAD_ARG:
      return AVERROR(EINVAL);
    case OPUS_BUFFER_TOO_SMALL:
      return AVERROR_UNKNOWN;
    case OPUS_INTERNAL_ERROR:
      return AVERROR(EFAULT);
    case OPUS_INVALID_PACKET:
      return AVERROR_INVALIDDATA;
    case OPUS_UNIMPLEMENTED:
      return AVERROR(ENOSYS);
    case OPUS_INVALID_STATE:
      return AVERROR_UNKNOWN;
    case OPUS_ALLOC_FAIL:
      return AVERROR(ENOMEM);
    default:
      return AVERROR(EINVAL);
  }
}

static size_t secondsToPacketCount(double seconds) {
  return (size_t)(seconds / 0.02);
}


static int ThreadMain(AVThreadMessageQueue *message_queue, uv_async_t *buffer_ready_async, uv_async_t *drain_async, const AudioDecodeThreadParams &thread_data) {
  int thread_ret = 0;
  int demux_ret = 0;

  set_thread_name("audio_decode_thread");

  const int opus_sample_rate = thread_data.sampleRate;
  const int opus_channels = thread_data.channels;
  const int opus_samples_per_frame = opus_sample_rate * OPUS_FRAME_DURATION_MS / 1000;
  const int pts_scale = OUTPUT_SAMPLE_RATE / opus_sample_rate;

  ThreadMessage thread_message;

  int64_t start_time_realtime = AV_NOPTS_VALUE;
  int64_t start_time_localtime = 0;

  int64_t last_packet_realtime = AV_NOPTS_VALUE;
  int64_t last_packet_received_at = 0;

  AVCodecParameters *codecpar = NULL;
  DemuxerThreadData *demuxer_thread = NULL;

  // Opus decoder state
  OpusDecoder *opus_decoder = NULL;
  int16_t *decoder_output = NULL;
  int64_t expected_pts = AV_NOPTS_VALUE;
  int64_t total_samples_decoded = 0;
  int64_t total_packets_decoded = 0;
  int64_t total_missing_frames = 0;
  int last_frame_size = opus_samples_per_frame;

  AVPacket *pkt_clone = av_packet_alloc();
  if (pkt_clone == NULL) {
    thread_ret = AVERROR(ENOMEM);
    goto cleanup_thread;
  }

  // Allocate decoder output buffer
  decoder_output = (int16_t *)av_malloc(OPUS_MAX_FRAME_SIZE * opus_channels * sizeof(int16_t));
  if (decoder_output == NULL) {
    thread_ret = AVERROR(ENOMEM);
    goto cleanup_thread;
  }

  thread_ret = start_rtp_demuxer(thread_data.sdpBase64, 10 * MICROSECONDS, message_queue, &demuxer_thread);
  if (thread_ret != 0) {
    goto cleanup_thread;
  }

  while (true) {
    thread_ret = av_thread_message_queue_recv(message_queue, &thread_message, 0);

    if (thread_ret < 0) {
      // This error is expected when shutting down
      if (thread_ret == AVERROR_EOF) {
        thread_ret = 0;
      }

      goto cleanup_thread;
    }

    if (thread_message.type == POST_CODEC_PARAMETERS) {
      avcodec_parameters_free(&codecpar);
      codecpar = thread_message.param.codecpar;

      // Create opus decoder when we receive codec parameters
      if (opus_decoder == NULL) {
        int opus_err;
        opus_decoder = opus_decoder_create(opus_sample_rate, opus_channels, &opus_err);
        if (opus_err != OPUS_OK) {
          fprintf(stderr, "Failed to create opus decoder: %s\n", opus_strerror(opus_err));
          thread_ret = ff_opus_error_to_averror(opus_err);
          goto cleanup_thread;
        }
      }
    } else if (thread_message.type == POST_PACKET) {
      AVPacket *pkt = thread_message.param.pkt;

      // Skip if decoder not initialized yet
      if (opus_decoder == NULL) {
        av_packet_free(&pkt);
        continue;
      }

      int64_t pkt_pts = pkt->pts;

      // Detect gaps in PTS timestamps
      if (expected_pts != AV_NOPTS_VALUE && pkt_pts > expected_pts) {
        // Calculate how many frames were missed
        // PTS is at 48kHz, frame_size is at decode sample rate
        int64_t pts_gap = pkt_pts - expected_pts;
        int64_t pts_per_frame = last_frame_size * pts_scale;
        int missing_frames = (int)(pts_gap / pts_per_frame);

        if (missing_frames > 0) {
          total_missing_frames += missing_frames;

          // Decode missing frames using packet loss concealment
          for (int i = 0; i < missing_frames; i++) {
            int frame_size;
            if (i == missing_frames - 1) {
              // Last missing frame: use FEC from current packet if available
              frame_size = opus_decode(opus_decoder, pkt->data, pkt->size, decoder_output, last_frame_size, 1);
            } else {
              // Earlier missing frames: use PLC (NULL packet)
              frame_size = opus_decode(opus_decoder, NULL, 0, decoder_output, last_frame_size, 0);
            }

            if (frame_size < 0) {
              fprintf(stderr, "opus_decode error during PLC: %s\n", opus_strerror(frame_size));
              continue;
            }

            total_samples_decoded += frame_size;
            // Send PLC/FEC decoded frame to Node.js callback
            if (buffer_ready_async != NULL) {
              AudioBuffer audio_buffer;
              audio_buffer.buf = (uint8_t *)av_malloc(frame_size * sizeof(int16_t));
              if (audio_buffer.buf != NULL) {
                memcpy(audio_buffer.buf, decoder_output, frame_size * sizeof(int16_t));
                audio_buffer.len = frame_size * sizeof(int16_t);
                // PTS for recovered frames: interpolate from expected_pts
                audio_buffer.pts = expected_pts + (i * last_frame_size * pts_scale);
                send_callback_for_many(buffer_ready_async, &audio_buffer);
              }
            }
          }
        }
      }

      // Decode the actual packet
      int frame_size = opus_decode(opus_decoder, pkt->data, pkt->size, decoder_output, OPUS_MAX_FRAME_SIZE, 0);

      if (frame_size < 0) {
        fprintf(stderr, "opus_decode error: %s\n", opus_strerror(frame_size));
        av_packet_free(&pkt);
        continue;
      }

      last_frame_size = frame_size;
      total_samples_decoded += frame_size;
      total_packets_decoded++;

      // Update expected PTS for next packet
      // frame_size is at decode sample rate, PTS is at 48kHz
      expected_pts = pkt_pts + (frame_size * pts_scale);

      // Send decoded frame to Node.js callback
      if (buffer_ready_async != NULL) {
        AudioBuffer audio_buffer;
        audio_buffer.buf = (uint8_t *)av_malloc(frame_size * sizeof(int16_t));
        if (audio_buffer.buf != NULL) {
          memcpy(audio_buffer.buf, decoder_output, frame_size * sizeof(int16_t));
          audio_buffer.len = frame_size * sizeof(int16_t);
          audio_buffer.pts = pkt_pts;
          send_callback_for_many(buffer_ready_async, &audio_buffer);
        }
      }

      av_packet_free(&pkt);
    } else if (thread_message.type == POST_START_TIME_REALTIME) {
      start_time_realtime = thread_message.param.start_time_realtime;
    } else if (thread_message.type == POST_START_TIME_LOCALTIME) {
      start_time_localtime = thread_message.param.start_time_localtime;
    }
  }

cleanup_thread:
  // Signal end of audio stream to Node.js callback
  if (buffer_ready_async != NULL) {
    finish_callback_for_many(buffer_ready_async);
  }

  // Log decoding summary
  if (total_packets_decoded > 0) {
    double total_duration_sec = (double)total_samples_decoded / opus_sample_rate;
    printf("Opus decode finished: %lld packets, %lld samples (%.2f sec), %lld missing frames recovered\n",
           (long long)total_packets_decoded,
           (long long)total_samples_decoded,
           total_duration_sec,
           (long long)total_missing_frames);
  }

  avcodec_parameters_free(&codecpar);
  av_packet_free(&pkt_clone);
  av_free(decoder_output);

  if (opus_decoder != NULL) {
    opus_decoder_destroy(opus_decoder);
  }

  av_thread_message_queue_set_err_send(message_queue, AVERROR_EOF);

  demux_ret = stop_rtp_demuxer(demuxer_thread);
  if (demux_ret != 0) {
    return demux_ret;
  } else {
    return thread_ret;
  }

  // check_for_memory_leaks();
}

napi_status start_audio_decode_thread(napi_env env, const AudioDecodeThreadParams &params, napi_value abort_signal, napi_value on_audio_callback, napi_value *external, napi_value *promise) {
  size_t stack_size = get_stack_size_for_thread("MUXER");

  return start_thread_with_promise_result<AudioDecodeThreadParams>(env, ThreadMain, params, abort_signal, NULL, stack_size, DEFAULT_MESSAGE_QUEUE_SIZE, external, on_audio_callback, NULL, promise);
}
