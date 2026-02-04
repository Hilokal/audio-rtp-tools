#include <node_api.h>
#include <uv.h>
#include <stdlib.h>

struct CallbackData {
  napi_env env;
  int value;
  napi_deferred deferred;

  void (*cleanup)(void *opaque);
  void* cleanup_opaque;
};

static void close_callback(uv_handle_t *handle) {
  uv_async_t *async = (uv_async_t*)handle;
  CallbackData *data = (CallbackData *)async->data;
  delete data;
  free(async);
}

static void async_callback(uv_async_t *async) {
  CallbackData *data = (CallbackData *)async->data;
  napi_env env = data->env;
  int status;

  if (data->cleanup != NULL) {
    data->cleanup(data->cleanup_opaque);
  }

  napi_handle_scope scope;
  status = napi_open_handle_scope(env, &scope);
  if (status != napi_ok) {
    fprintf(stderr, "napi_open_handle_scope is fail status [%d]", status);
    return;
  }

  napi_value js_value;
  status = napi_create_int32(env, data->value, &js_value);
  if (status != napi_ok) {
    goto cleanup;
  }

  status = napi_resolve_deferred(env, data->deferred, js_value);
  if (status != napi_ok) {
    goto cleanup;
  }

cleanup:
  status = napi_close_handle_scope(env, scope);
  if (status != napi_ok) {
    fprintf(stderr, "napi_close_handle_scope is fail status [%d]", status);
  }

  uv_close((uv_handle_t *)async, close_callback);
}

napi_status init_thread_finished_node_callback(napi_env env, napi_value *promise, void (*cleanup)(void *opaque), void *cleanup_opaque, uv_async_t **async) {
  napi_deferred deferred;
  napi_status status;

  status = napi_create_promise(env, &deferred, promise);
  if (status != napi_ok) {
    return status;
  }

  *async = (uv_async_t*)malloc(sizeof(uv_async_t));
  int ret = uv_async_init(uv_default_loop(), *async, async_callback);
  if (ret != 0) {
    return napi_throw_error(env, NULL, "uv_async_init failed");
  }

  CallbackData *data = new CallbackData();
  data->env = env;
  data->deferred = deferred;
  data->cleanup = cleanup;
  data->cleanup_opaque = cleanup_opaque;
  (**async).data = data;

  return napi_ok;
}

void resolve_thread_finished_node_callback(uv_async_t *async, int value) {
  CallbackData *data = (CallbackData *)async->data;
  data->value = value;
  uv_async_send(async);
}
