#pragma once

#include <pthread.h>
#include <node_api.h>

extern "C" {
#include <libavutil/threadmessage.h>
}

struct ProducerThreadParams {
  char *url;
  char *cname;
  char *cryptoSuite;
  char *keyBase64;
  char *ssrc;
  char *payloadType;
};

// NAPI-based API for use from Node.js
napi_status start_producer_thread(
  napi_env env,
  ProducerThreadParams &params,
  napi_value abort_signal,
  napi_value *external,
  napi_value *promise
);

struct ProducerThreadData {
  pthread_t thread;
  AVThreadMessageQueue *message_queue;
  ProducerThreadParams params;
  int thread_ret;
};

// Raw pthread API for use from encoder thread
int start_producer_thread_raw(
  const ProducerThreadParams &params,
  unsigned int queue_size,
  ProducerThreadData **thread_data
);

int stop_producer_thread_raw(ProducerThreadData *thread_data);
