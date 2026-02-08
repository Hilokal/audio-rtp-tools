#pragma once

#include <node_api.h>

struct AudioEncodeThreadParams {
  char *rtpUrl;       // "rtp://127.0.0.1:port" or "srtp://..."
  char *ssrc;
  char *payloadType;
  char *cname;
  char *cryptoSuite;  // e.g., "AES_CM_128_HMAC_SHA1_80" or NULL
  char *keyBase64;    // base64-encoded SRTP key or NULL
  int32_t bitrate;    // e.g., 32000 for speech
  bool enableFec;
  int32_t packetLossPercent;
};

napi_status start_audio_encode_thread(
  napi_env env,
  const AudioEncodeThreadParams &params,
  napi_value abort_signal,
  napi_value *external,  // Returns message queue for posting PCM
  napi_value *promise
);
