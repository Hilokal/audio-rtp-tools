#pragma once

#include <node_api.h>
#include <uv.h>

struct AudioBuffer {
  // Pointer to PCM audio data (int16_t samples)
  uint8_t *buf;
  unsigned int len;

  // Presentation timestamp in samples (at source sample rate)
  int64_t pts;

  // True on the first frame after a timestamp gap too long to conceal: the
  // audio before it ends more than the concealment cap earlier in the stream.
  bool discontinuity;
};

napi_status init_callback_for_many(napi_env env, napi_value on_buffer_ready_callback, uv_async_t **async);

int send_callback_for_many(uv_async_t *async, AudioBuffer *value);
int finish_callback_for_many(uv_async_t *async);

void cleanup_callback_for_many(uv_async_t *async);
