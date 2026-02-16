#pragma once

#include <node_api.h>

#include "thread_messages.h"
#include "node_errors.h"
#include "buffer_ready_node_callback.h"

struct DrainCallback {
  napi_env env;
  napi_ref callback_ref;
};

static void drain_async_callback(uv_async_t *async) {
  DrainCallback *data = (DrainCallback *)async->data;
  napi_handle_scope scope;
  napi_status status = napi_open_handle_scope(data->env, &scope);
  if (status != napi_ok) {
    fprintf(stderr, "drain_async_callback: napi_open_handle_scope failed [%d]\n", status);
    return;
  }

  napi_value callback, global, result;
  napi_get_reference_value(data->env, data->callback_ref, &callback);
  napi_get_global(data->env, &global);
  napi_call_function(data->env, global, callback, 0, NULL, &result);

  napi_close_handle_scope(data->env, scope);
}

static void drain_close_callback(uv_handle_t *handle) {
  DrainCallback *data = (DrainCallback *)handle->data;
  napi_delete_reference(data->env, data->callback_ref);
  uv_async_t *async = (uv_async_t *)handle;
  delete data;
  delete async;
}

template<class THREAD_PARAMS>
class ThreadData {
  public:
  AVThreadMessageQueue *message_queue;

  // This is a reference to an external object that wraps the message queue
  napi_ref message_queue_ref;

  // This is an optional reference to a js object that will be held for the
  // duration of the thread's execution.
  // TODO: There is a lot of overlap between this and the message queue ref. They could
  // probably be refactored into one.
  napi_ref js_input_ref;

  THREAD_PARAMS params;
  pthread_t thread;

  uv_async_t thread_finished_async;
  uv_async_t *buffer_ready_async;
  uv_async_t *drain_async;

  napi_env env;
  napi_deferred deferred;
  int thread_ret;

  int (*thread_main)(AVThreadMessageQueue *message_queue, uv_async_t *buffer_ready_async, uv_async_t *drain_async, const THREAD_PARAMS &params);

  ~ThreadData() {
    napi_delete_reference(env, message_queue_ref);
  }
};

static napi_value abort_signal_handler(napi_env env, napi_callback_info info) {
  napi_ref js_message_queue_ref;
  napi_value js_message_queue;
  AVThreadMessageQueue *message_queue;

  napi_status status;
  status = napi_get_cb_info(env, info, NULL, NULL, NULL, (void**)&js_message_queue_ref);
  if (status != napi_ok) {
    GET_AND_THROW_LAST_ERROR(env);
    return NULL;
  }

  status = napi_get_reference_value(env, js_message_queue_ref, &js_message_queue);
  if (status != napi_ok) {
    GET_AND_THROW_LAST_ERROR(env);
    return NULL;
  }

  status = napi_get_value_external(env, js_message_queue, (void**)&message_queue);
  if (status != napi_ok) {
    GET_AND_THROW_LAST_ERROR(env);
    return NULL;
  }

  av_thread_message_queue_set_err_send(message_queue, AVERROR_EOF);
  av_thread_message_queue_set_err_recv(message_queue, AVERROR_EOF);
  return NULL;
}

static void abort_signal_handler_finalizer(napi_env env, void* finalize_data, void* finalize_hint) {
  napi_ref js_message_queue_ref = (napi_ref)finalize_data;

  napi_delete_reference(env, js_message_queue_ref);
}

static napi_status add_abort_event_listener(napi_env env, napi_value signal, napi_value js_message_queue) {
  napi_valuetype signal_type;
  napi_status status;

  status = napi_typeof(env, signal, &signal_type);
  if (status != napi_ok)
    return status;

  if (signal_type == napi_undefined || signal_type == napi_null)
    return napi_ok;

  napi_value js_key;
  napi_value js_add_event_listener;
  napi_value result;

  status = napi_create_string_utf8(env, "addEventListener", NAPI_AUTO_LENGTH, &js_key);
  if (status != napi_ok)
    return status;

  status = napi_get_property(env, signal, js_key, &js_add_event_listener);
  if (status != napi_ok) {
    return status;
  }

  size_t argc = 3;
  napi_value argv[3];

  status = napi_create_string_utf8(env, "abort", NAPI_AUTO_LENGTH, &argv[0]);
  if (status != napi_ok) {
    return status;
  }

  napi_ref js_message_queue_ref;

  status = napi_create_reference(env, js_message_queue, 1, &js_message_queue_ref);
  if (status != napi_ok) {
    return status;
  }

  status = napi_create_function(env, "abort_signal_handler", NAPI_AUTO_LENGTH, abort_signal_handler, (void*)js_message_queue_ref, &argv[1]);
  if (status != napi_ok) {
    return status;
  }

  status = napi_add_finalizer(env, argv[1], js_message_queue_ref, abort_signal_handler_finalizer, NULL, NULL);
  if (status != napi_ok) {
    return status;
  }

  status = napi_create_object(env, &argv[2]);
  if (status != napi_ok) {
    return status;
  }

  napi_value js_true;
  status = napi_get_boolean(env, true, &js_true);
  if (status != napi_ok) {
    return status;
  }

  status = napi_set_named_property(env, argv[2], "once", js_true);
  if (status != napi_ok) {
    return status;
  }

  status = napi_call_function(env, signal, js_add_event_listener, argc, argv, &result);
  if (status != napi_ok) {
    return status;
  }

  return status;
}

template<class THREAD_PARAMS>
static void close_callback(uv_handle_t *handle) {
  uv_async_t *async = (uv_async_t*)handle;
  ThreadData<THREAD_PARAMS> *thread_data = (ThreadData<THREAD_PARAMS> *)async->data;
  delete thread_data;
}

template<class THREAD_PARAMS>
static void async_callback(uv_async_t *async) {
  ThreadData<THREAD_PARAMS> *thread_data = (ThreadData<THREAD_PARAMS> *)async->data;

  napi_env env = thread_data->env;
  int status;

  napi_handle_scope scope;
  status = napi_open_handle_scope(env, &scope);
  if (status != napi_ok) {
    fprintf(stderr, "napi_open_handle_scope is fail status [%d]", status);
    return;
  }

  if (thread_data->buffer_ready_async != NULL) {
    cleanup_callback_for_many(thread_data->buffer_ready_async);
  }

  if (thread_data->drain_async != NULL) {
    uv_close((uv_handle_t *)thread_data->drain_async, drain_close_callback);
    thread_data->drain_async = NULL;
  }

  napi_value js_value;
  status = napi_create_int32(env, thread_data->thread_ret, &js_value);
  if (status != napi_ok) {
    goto cleanup;
  }

  status = napi_resolve_deferred(env, thread_data->deferred, js_value);
  if (status != napi_ok) {
    goto cleanup;
  }

  if (thread_data->js_input_ref != NULL) {
    napi_delete_reference(env, thread_data->js_input_ref);
  }

cleanup:
  status = napi_close_handle_scope(env, scope);
  if (status != napi_ok) {
    fprintf(stderr, "napi_close_handle_scope is fail status [%d]", status);
  }

  uv_close((uv_handle_t *)async, close_callback<THREAD_PARAMS>);
}

template<class THREAD_PARAMS>
void *ThreadMain(void *opaque) {
  ThreadData<THREAD_PARAMS>* thread_data = (ThreadData<THREAD_PARAMS>*)opaque;

  int ret = thread_data->thread_main(thread_data->message_queue, thread_data->buffer_ready_async, thread_data->drain_async, thread_data->params);
  thread_data->thread_ret = ret;

  av_thread_message_queue_set_err_send(thread_data->message_queue, AVERROR_EOF);
  av_thread_message_queue_set_err_recv(thread_data->message_queue, AVERROR_EOF);

  uv_async_send(&thread_data->thread_finished_async);

  return 0;
}

static void finalize(napi_env env, void* finalize_data, void* finalize_hint) {
  AVThreadMessageQueue *message_queue = (AVThreadMessageQueue *)finalize_data;
  av_thread_message_queue_free(&message_queue);
}

#define DEFAULT_MESSAGE_QUEUE_SIZE 1024

template<class THREAD_PARAMS>
napi_status start_thread_with_promise_result(
    napi_env env,
    int (*thread_main)(AVThreadMessageQueue *message_queue, uv_async_t *buffer_ready_async, uv_async_t *drain_async, const THREAD_PARAMS &params),
    const THREAD_PARAMS &params,
    napi_value abort_signal,
    napi_value js_input_value,
    size_t stack_size,
    unsigned int message_queue_size,
    napi_value *external,
    napi_value on_buffer_ready_callback,
    napi_value on_drain_callback,
    napi_value *promise) {

  napi_status status;
  int ret;

  ThreadData<THREAD_PARAMS>* thread_data = new ThreadData<THREAD_PARAMS>();
  thread_data->params = params;
  thread_data->env = env;
  thread_data->thread_main = thread_main;

  ret = uv_async_init(uv_default_loop(), &thread_data->thread_finished_async, async_callback<THREAD_PARAMS>);
  if (ret != 0) {
    delete thread_data;
    return napi_throw_error(env, NULL, "uv_async_init failed");
  }
  thread_data->thread_finished_async.data = (void*)thread_data;

  status = napi_create_promise(env, &thread_data->deferred, promise);
  if (status != napi_ok) {
    delete thread_data;
    return status;
  }

  if (js_input_value != NULL) {
    status = napi_create_reference(env, js_input_value, 1, &thread_data->js_input_ref);
    if (status != napi_ok) {
      delete thread_data;
      return status;
    }
  }

  //
  // Create message queue
  //
  ret = av_thread_message_queue_alloc(&thread_data->message_queue, message_queue_size, sizeof(ThreadMessage));
  if (ret != 0) {
    delete thread_data;
    return throw_ffmpeg_error(env, ret);
  }

  av_thread_message_queue_set_free_func(thread_data->message_queue, thread_message_free_func);

  status = napi_create_external(env, thread_data->message_queue, finalize, NULL, external);
  if (status != napi_ok) {
    delete thread_data;
    return status;
  }

  status = napi_create_reference(env, *external, 1, &thread_data->message_queue_ref);
  if (status != napi_ok) {
    delete thread_data;
    return status;
  }

  status = add_abort_event_listener(env, abort_signal, *external);
  if (status != napi_ok) {
    delete thread_data;
    return status;
  }

  if (on_buffer_ready_callback == NULL) {
    thread_data->buffer_ready_async = NULL;
  } else {
    status = init_callback_for_many(env, on_buffer_ready_callback, &thread_data->buffer_ready_async);
    if (status != napi_ok) {
      delete thread_data;
      return status;
    }
  }

  if (on_drain_callback == NULL) {
    thread_data->drain_async = NULL;
  } else {
    DrainCallback *drain_data = new DrainCallback();
    drain_data->env = env;

    status = napi_create_reference(env, on_drain_callback, 1, &drain_data->callback_ref);
    if (status != napi_ok) {
      delete drain_data;
      delete thread_data;
      return status;
    }

    thread_data->drain_async = new uv_async_t();
    ret = uv_async_init(uv_default_loop(), thread_data->drain_async, drain_async_callback);
    if (ret != 0) {
      napi_delete_reference(env, drain_data->callback_ref);
      delete drain_data;
      delete thread_data->drain_async;
      delete thread_data;
      return napi_throw_error(env, NULL, "uv_async_init failed for drain");
    }
    thread_data->drain_async->data = drain_data;
  }

  //
  // Start thread
  //

  pthread_attr_t attr;
  ret = pthread_attr_init(&attr);
  if (ret != 0) {
    delete thread_data;
    fprintf(stderr, "pthread_attr_init fail error num [%d]\n", ret);
    return napi_throw_error(env, NULL, "pthread_attr_init failed");
  }

  if (stack_size != 0) {
    ret = pthread_attr_setstacksize(&attr, stack_size);
    if (ret != 0) {
      fprintf(stderr, "pthread_attr_setstacksize fail error num [%d]\n", ret);
    }
  }

  ret = pthread_create(&thread_data->thread, &attr, ThreadMain<THREAD_PARAMS>, (void *)thread_data);
  if (ret != 0) {
    delete thread_data;
    fprintf(stderr, "pthread_create fail error num [%d]\n", ret);
    return napi_throw_error(env, NULL, "pthread_create failed");
  }

  ret = pthread_detach(thread_data->thread);
  if (ret != 0) {
    fprintf(stderr, "pthread_detatch fail error num [%d]\n", ret);
  }

  return napi_ok;
}
