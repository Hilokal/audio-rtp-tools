#pragma once

#include <node_api.h>

extern "C" {
#include <libavutil/buffer.h>
}

struct AudioDecodeThreadParams {
  char *sdpBase64;

  // TODO: These are currently ignored by the decoder
  int32_t sampleRate;   // Output sample rate (e.g., 24000 for OpenAI)
  int32_t channels;     // Output channels (e.g., 1 for mono)

  // Deliver silence in place of synthesized audio (PLC, and the comfort noise
  // decoded from DTX / keep-alive packets). FEC-recovered frames are kept.
  bool concealWithSilence;

  // How long to wait for a missing RTP packet, in milliseconds. Negative keeps
  // the libavformat default (100ms).
  int32_t maxReorderDelayMs;

  // Reference to an RtpReceiveStats (see rtp_stats.h), released when the
  // thread exits. May be NULL.
  AVBufferRef *statsRef;
};

napi_status start_audio_decode_thread(
  napi_env env,
  const AudioDecodeThreadParams &params,
  napi_value abort_signal,
  napi_value on_audio_callback,
  napi_value *external,
  napi_value *promise
);
