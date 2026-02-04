#pragma once

#include <node_api.h>

napi_status init_thread_finished_node_callback(napi_env env, napi_value *promise, void (*cleanup)(void *opaque), void *cleanup_opaque, uv_async_t **async);
void resolve_thread_finished_node_callback(uv_async_t *async, int value);
