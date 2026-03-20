#pragma once
#include <node_api.h>

struct SaveAudioThreadParams {
  char *sdpBase64;
  char *outputPath;
};

napi_status start_save_audio_thread(
  napi_env env,
  const SaveAudioThreadParams &params,
  napi_value abort_signal,
  napi_value *external,
  napi_value *promise
);
