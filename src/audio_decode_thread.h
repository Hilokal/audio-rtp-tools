#pragma once

#include <node_api.h>

struct AudioDecodeThreadParams {
  char *sdpBase64;

  // TODO: These are currently ignored by the decoder
  int32_t sampleRate;   // Output sample rate (e.g., 24000 for OpenAI)
  int32_t channels;     // Output channels (e.g., 1 for mono)
};

napi_status start_audio_decode_thread(
  napi_env env,
  const AudioDecodeThreadParams &params,
  napi_value abort_signal,
  napi_value on_audio_callback,
  napi_value *external,
  napi_value *promise
);
