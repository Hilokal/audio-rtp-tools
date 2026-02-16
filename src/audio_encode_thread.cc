#include <node_api.h>

#include "audio_encode_thread.h"
#include "producer_thread.h"
#include "thread_messages.h"
#include "node_errors.h"
#include "util.h"
#include "thread_with_promise_result.h"

extern "C" {
#include <libavutil/time.h>
#include <libavutil/mem.h>
#include <opus/opus.h>
}

// Opus RTP timestamps are always at 48kHz
#define OUTPUT_SAMPLE_RATE 48000
#define CHANNELS 2
// 20ms frame at 48kHz for PTS = 960 samples
#define FRAME_SIZE_OUTPUT 960
// Maximum opus encoded frame size
#define MAX_OPUS_FRAME_SIZE 1275
// Max 20ms frame at 48kHz input
#define MAX_FRAME_SIZE_INPUT 960

// Producer queue size - just needs to absorb burst from encoding while the
// producer paces at real-time. Small so that backpressure surfaces promptly.
#define PRODUCER_QUEUE_SIZE 256

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

static int ThreadMain(AVThreadMessageQueue *message_queue, uv_async_t *buffer_ready_async, uv_async_t *drain_async, const AudioEncodeThreadParams &params) {
  set_thread_name("audio_encode_thread");

  const int input_sample_rate = params.sampleRate;
  const int frame_size_input = input_sample_rate * 20 / 1000;  // 20ms frame

  int ret = 0;
  ThreadMessage thread_message;
  ProducerThreadData *producer_thread = NULL;

  // Opus encoder state
  OpusEncoder *opus_encoder = NULL;
  int16_t mono_accum[MAX_FRAME_SIZE_INPUT];
  int16_t stereo_frame[MAX_FRAME_SIZE_INPUT * CHANNELS];
  uint8_t opus_data[MAX_OPUS_FRAME_SIZE];
  int accum_pos = 0;
  int64_t pts = 0;

  int64_t total_samples_encoded = 0;
  int64_t total_frames_encoded = 0;

  //
  // Start producer thread with RTP parameters
  //
  {
    ProducerThreadParams producer_params;
    producer_params.url = params.rtpUrl;
    producer_params.ssrc = params.ssrc;
    producer_params.payloadType = params.payloadType;
    producer_params.cname = params.cname;
    producer_params.cryptoSuite = params.cryptoSuite;
    producer_params.keyBase64 = params.keyBase64;

    ret = start_producer_thread_raw(producer_params, PRODUCER_QUEUE_SIZE, &producer_thread);
    if (ret != 0) {
      fprintf(stderr, "audio_encode_thread: failed to start producer thread [%d]\n", ret);
      goto cleanup;
    }
  }

  //
  // 3. Create Opus encoder at specified sample rate
  //
  {
    int opus_err;
    opus_encoder = opus_encoder_create(input_sample_rate, CHANNELS, OPUS_APPLICATION_VOIP, &opus_err);
    if (opus_err != OPUS_OK) {
      fprintf(stderr, "audio_encode_thread: failed to create opus encoder: %s\n", opus_strerror(opus_err));
      ret = ff_opus_error_to_averror(opus_err);
      goto cleanup;
    }

    // Set bitrate
    opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(params.bitrate > 0 ? params.bitrate : 32000));

    // Set FEC
    opus_encoder_ctl(opus_encoder, OPUS_SET_INBAND_FEC(params.enableFec ? 1 : 0));

    // Set expected packet loss percentage
    opus_encoder_ctl(opus_encoder, OPUS_SET_PACKET_LOSS_PERC(params.packetLossPercent));
  }

  fprintf(stderr, "audio_encode_thread: started, bitrate=%d\n", params.bitrate);

  //
  // 4. Main loop - receive PCM, encode, post to producer
  //
  while (true) {
    ret = av_thread_message_queue_recv(message_queue, &thread_message, 0);
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        ret = 0;
      }
      goto cleanup;
    }

    if (drain_async != NULL) {
      uv_async_send(drain_async);
    }

    if (thread_message.type == POST_PCM_BUFFER) {
      int16_t *input = (int16_t *)thread_message.param.buf->data;
      int remaining = thread_message.param.buf->size / sizeof(int16_t);

      while (remaining > 0) {
        // Copy mono samples to accumulator
        int to_copy = remaining;
        if (to_copy > frame_size_input - accum_pos) {
          to_copy = frame_size_input - accum_pos;
        }

        memcpy(mono_accum + accum_pos, input, to_copy * sizeof(int16_t));
        accum_pos += to_copy;
        input += to_copy;
        remaining -= to_copy;

        // When we have a full frame, encode it
        if (accum_pos >= frame_size_input) {
          // Convert mono to stereo (duplicate each sample)
          for (int i = 0; i < frame_size_input; i++) {
            stereo_frame[i * 2] = mono_accum[i];      // Left
            stereo_frame[i * 2 + 1] = mono_accum[i];  // Right
          }

          // Encode stereo frame (480 samples at 24kHz)
          int encoded_len = opus_encode(opus_encoder, stereo_frame, frame_size_input, opus_data, MAX_OPUS_FRAME_SIZE);

          if (encoded_len < 0) {
            fprintf(stderr, "audio_encode_thread: opus_encode error: %s\n", opus_strerror(encoded_len));
            accum_pos = 0;
            continue;
          }

          // Create AVPacket - PTS is at 48kHz!
          AVPacket *pkt = av_packet_alloc();
          if (pkt == NULL) {
            fprintf(stderr, "audio_encode_thread: av_packet_alloc failed\n");
            accum_pos = 0;
            continue;
          }

          ret = av_new_packet(pkt, encoded_len);
          if (ret != 0) {
            av_packet_free(&pkt);
            fprintf(stderr, "audio_encode_thread: av_packet_new failed\n");
            accum_pos = 0;
            continue;
          }

          memcpy(pkt->data, opus_data, encoded_len);
          pkt->size = encoded_len;
          pkt->pts = pts;
          pkt->dts = pts;
          pkt->duration = FRAME_SIZE_OUTPUT;  // 960 at 48kHz = 20ms

          // Post to producer thread (blocking — safe since we're on a dedicated pthread)
          int post_ret = post_packet_to_thread(producer_thread->message_queue, pkt, 0);
          if (post_ret < 0) {
            fprintf(stderr, "audio_encode_thread: post_packet_to_thread failed [%d]\n", post_ret);
          }

          av_packet_free(&pkt);

          pts += FRAME_SIZE_OUTPUT;  // Increment at 48kHz rate
          accum_pos = 0;
          total_frames_encoded++;
          total_samples_encoded += frame_size_input;
        }
      }

      // Free the PCM buffer
      thread_message_free_func(&thread_message);
    } else if (thread_message.type == FLUSH_OPUS_ENCODER) {
      // Encode any remaining accumulated PCM with zero-padding
      if (accum_pos > 0) {
        // Zero-pad the rest of the frame
        memset(mono_accum + accum_pos, 0, (frame_size_input - accum_pos) * sizeof(int16_t));

        // Convert mono to stereo (duplicate each sample)
        for (int i = 0; i < frame_size_input; i++) {
          stereo_frame[i * 2] = mono_accum[i];
          stereo_frame[i * 2 + 1] = mono_accum[i];
        }

        int encoded_len = opus_encode(opus_encoder, stereo_frame, frame_size_input, opus_data, MAX_OPUS_FRAME_SIZE);
        if (encoded_len > 0) {
          AVPacket *pkt = av_packet_alloc();
          if (pkt != NULL && av_new_packet(pkt, encoded_len) == 0) {
            memcpy(pkt->data, opus_data, encoded_len);
            pkt->size = encoded_len;
            pkt->pts = pts;
            pkt->dts = pts;
            pkt->duration = FRAME_SIZE_OUTPUT;

            int post_ret = post_packet_to_thread(producer_thread->message_queue, pkt, 0);
            if (post_ret < 0) {
              fprintf(stderr, "audio_encode_thread: flush post_packet_to_thread failed [%d]\n", post_ret);
            }
          }
          if (pkt != NULL) {
            av_packet_free(&pkt);
          }
        }

        accum_pos = 0;
      }
      pts = 0;
    } else if (thread_message.type == CLEAR_PRODUCER_QUEUE) {
      // Drain the producer thread's packet queue
      if (producer_thread != NULL) {
        ThreadMessage producer_msg;
        while (true) {
          int drain_ret = av_thread_message_queue_recv(producer_thread->message_queue, &producer_msg, AV_THREAD_MESSAGE_NONBLOCK);
          if (drain_ret < 0) {
            break;
          }
          thread_message_free_func(&producer_msg);
        }
      }
    } else if (thread_message.type == SET_ENCODER_BITRATE) {
      opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(
        thread_message.param.int_value > 0 ? thread_message.param.int_value : OPUS_AUTO));
    } else if (thread_message.type == SET_ENCODER_FEC) {
      opus_encoder_ctl(opus_encoder, OPUS_SET_INBAND_FEC(thread_message.param.int_value));
    } else if (thread_message.type == SET_ENCODER_PACKET_LOSS_PERC) {
      opus_encoder_ctl(opus_encoder, OPUS_SET_PACKET_LOSS_PERC(thread_message.param.int_value));
    } else {
      thread_message_free_func(&thread_message);
    }
  }

cleanup:
  fprintf(stderr, "audio_encode_thread: stopping, encoded %lld frames (%lld samples, %.2f sec)\n",
          (long long)total_frames_encoded,
          (long long)total_samples_encoded,
          (double)total_samples_encoded / input_sample_rate);

  if (producer_thread != NULL) {
    int producer_ret = stop_producer_thread_raw(producer_thread);
    if (producer_ret != 0) {
      fprintf(stderr, "audio_encode_thread: producer thread returned error [%d]\n", producer_ret);
    }
  }

  // Cleanup resources
  if (opus_encoder != NULL) {
    opus_encoder_destroy(opus_encoder);
  }

  return ret;
}

napi_status start_audio_encode_thread(
  napi_env env,
  const AudioEncodeThreadParams &params,
  napi_value abort_signal,
  napi_value on_drain_callback,
  unsigned int queue_depth,
  napi_value *external,
  napi_value *promise
) {
  size_t stack_size = get_stack_size_for_thread("ENCODER");

  return start_thread_with_promise_result<AudioEncodeThreadParams>(
    env,
    ThreadMain,
    params,
    abort_signal,
    NULL,
    stack_size,
    queue_depth,
    external,
    NULL,
    on_drain_callback,
    promise
  );
}
