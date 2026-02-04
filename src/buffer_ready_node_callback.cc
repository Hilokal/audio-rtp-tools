#include <node_api.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/threadmessage.h>
#include <libavutil/error.h>
#include <uv.h>
}

#include "buffer_ready_node_callback.h"
#include "node_errors.h"

napi_status create_js_pts(napi_env env, int64_t value, napi_value *result) {
  if (value == AV_NOPTS_VALUE) {
    return napi_get_null(env, result);
  } else {
    return napi_create_int64(env, value, result);
  }
}

static void finalize_external_buffer(napi_env env, void* finalize_data, void* finalize_hint) {
  av_free(finalize_data);
}

napi_status create_js_result(napi_env env, AudioBuffer *buffer, napi_value *object) {
  napi_status status;
  napi_value js_buffer;
  napi_value js_pts;

  // Convert into node Buffer object
  status = napi_create_external_buffer(env, buffer->len, buffer->buf, finalize_external_buffer, NULL, &js_buffer);
  if (status != napi_ok) {
    av_free(buffer->buf);
    buffer->buf = NULL;
    return status;
  }

  status = create_js_pts(env, buffer->pts, &js_pts);
  if (status != napi_ok)
    return status;

  status = napi_create_object(env, object);
  if (status != napi_ok)
    return status;

  status = napi_set_named_property(env, *object, "buffer", js_buffer);
  if (status != napi_ok)
    return status;

  status = napi_set_named_property(env, *object, "pts", js_pts);
  if (status != napi_ok)
    return status;

  return napi_ok;
}

struct CallbackMany {
  napi_env env;
  int error;
  AVThreadMessageQueue *message_queue;
  uv_async_t async;
  napi_ref on_buffer_ready_callback;
};

static void close_callback2(uv_handle_t *handle) {
  CallbackMany *data = (CallbackMany *)handle->data;
  av_thread_message_queue_free(&data->message_queue);
  delete data;
}

void handle_all_messages_in_queue(CallbackMany *thread_data) {
  napi_env env = thread_data->env;
  napi_status status;

  while (true) {
    AudioBuffer audio_buffer;
    int ret = av_thread_message_queue_recv(thread_data->message_queue, &audio_buffer, AV_THREAD_MESSAGE_NONBLOCK);

    if (ret == AVERROR(EAGAIN)) {
      // queue is empty. exit while loop
      break;
    } else if (ret == AVERROR_EOF) {
      // Thread is shutting down
      if (thread_data->on_buffer_ready_callback != NULL) {
        status = napi_delete_reference(env, thread_data->on_buffer_ready_callback);
        if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);
        thread_data->on_buffer_ready_callback = NULL;
      }
      break;
    } else if (ret < 0) {
      // This is an unexpected error.
      fprintf(stderr, "av_thread_message_queue_recv failed with error [%d]", ret);
      break;
    } else {
      napi_value callback_function;
      status = napi_get_reference_value(env, thread_data->on_buffer_ready_callback, &callback_function);
      if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

      size_t argc = 1;
      napi_value argv[1];
      napi_value js_ret;
      napi_value global;
      status = napi_get_global(env, &global);
      if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

      status = create_js_result(env, &audio_buffer, &argv[0]);
      if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

      status = napi_call_function(env, global, callback_function, argc, argv, &js_ret);
      if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);
    }
  }
}

void async_callback_for_many(uv_async_t *async) {
  CallbackMany *thread_data = (CallbackMany *)async->data;

  napi_handle_scope scope;
  napi_status status;
  napi_env env = thread_data->env;

  status = napi_open_handle_scope(env, &scope);
  if (status != napi_ok) {
    fprintf(stderr, "napi_open_handle_scope is fail status [%d]", status);
    return;
  }

  handle_all_messages_in_queue(thread_data);

  status = napi_close_handle_scope(env, scope);
  if (status != napi_ok) {
    fprintf(stderr, "napi_close_handle_scope is fail status [%d]", status);
  }
}

napi_status init_callback_for_many(napi_env env, napi_value on_buffer_ready_callback, uv_async_t **async) {
  CallbackMany *thread_data = new CallbackMany;
  thread_data->env = env;

  int ret;
  napi_status status;

  status = napi_create_reference(env, on_buffer_ready_callback, 1, &thread_data->on_buffer_ready_callback);
  if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

  ret = uv_async_init(uv_default_loop(), &thread_data->async, async_callback_for_many);
  if (ret != 0) {
    napi_throw_error(env, NULL, "uv_async_init failed");
    return napi_pending_exception;
  }

  ret = av_thread_message_queue_alloc(&thread_data->message_queue, 1024, sizeof(AudioBuffer));
  if (ret < 0) {
    throw_ffmpeg_error(env, ret);
    delete thread_data;
    return napi_pending_exception;
  }

  thread_data->async.data = thread_data;
  *async = &thread_data->async;

  return napi_ok;
}

void cleanup_callback_for_many(uv_async_t *async) {
  CallbackMany *thread_data = (CallbackMany *)async->data;
  handle_all_messages_in_queue(thread_data);
  uv_close((uv_handle_t *)&thread_data->async, close_callback2);
}

int send_callback_for_many(uv_async_t *async, AudioBuffer *value) {
  CallbackMany *thread_data = (CallbackMany *)async->data;

  int ret = av_thread_message_queue_send(thread_data->message_queue, value, AV_THREAD_MESSAGE_NONBLOCK);
  if (ret < 0) {
    if (ret == AVERROR(EAGAIN)) {
      fprintf(stderr, "WARNING: message queue full while posting AudioBuffer to libav thread");
    }

    av_free(value->buf);
    value->buf = NULL;
    return ret;
  }

  uv_async_send(&thread_data->async);

  return napi_ok;
}

int finish_callback_for_many(uv_async_t *async) {
  CallbackMany *thread_data = (CallbackMany *)async->data;

  av_thread_message_queue_set_err_recv(thread_data->message_queue, AVERROR_EOF);

  uv_async_send(&thread_data->async);

  return napi_ok;
}
