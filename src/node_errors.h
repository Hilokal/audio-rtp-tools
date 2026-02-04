#pragma once

#include <node_api.h>
extern "C" {
#include <libavutil/error.h>
#include <libavutil/common.h>
}


#ifdef av_err2str
#undef av_err2str
av_always_inline char *av_err2str(int errnum) {
  // static char str[AV_ERROR_MAX_STRING_SIZE];
  // thread_local may be better than static in multi-thread circumstance
  thread_local char str[AV_ERROR_MAX_STRING_SIZE];
  memset(str, 0, sizeof(str));
  return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#endif

#define GET_AND_THROW_LAST_ERROR(env)                                    \
  do {                                                                   \
    const napi_extended_error_info *error_info;                          \
    napi_get_last_error_info((env), &error_info);                        \
    bool is_pending;                                                     \
    const char* err_message = error_info->error_message;                  \
    napi_is_exception_pending((env), &is_pending);                       \
    /* If an exception is already pending, don't rethrow it */           \
    if (!is_pending) {                                                   \
      const char* error_message = err_message != NULL ?                  \
                       err_message :                                     \
                      "empty error message";                             \
      napi_throw_error((env), NULL, error_message);                      \
    }                                                                    \
  } while (0)

#define THROW_FFMPEG_ERROR(env, errnum)                                  \
  do {                                                                   \
    const char* err_message = error_info->error_message;                 \
    napi_is_exception_pending((env), &is_pending);                       \
    /* If an exception is already pending, don't rethrow it */           \
    if (!is_pending) {                                                   \
      const char* error_message = err_message != NULL ?                  \
                       err_message :                                     \
                      "empty error message";                             \
      napi_throw_error((env), NULL, error_message);                      \
    }                                                                    \
  } while (0)

napi_status create_ffmpeg_error(napi_env env, int errnum, napi_value *result);
napi_status throw_ffmpeg_error(napi_env env, int errnum);
