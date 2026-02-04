#include "node_errors.h"


napi_status create_ffmpeg_error(napi_env env, int errnum, napi_value *result) {
  napi_status status;

  napi_value js_code;
  napi_value js_msg;

  char msg[AV_ERROR_MAX_STRING_SIZE] = {0};
  char code[32];

  snprintf(code, sizeof(code), "%d", errnum);
  av_make_error_string(msg, AV_ERROR_MAX_STRING_SIZE, errnum);

  status = napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &js_code);
  if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

  status = napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &js_msg);
  if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

  return napi_create_error(env, js_code, js_msg, result);
}

napi_status throw_ffmpeg_error(napi_env env, int errnum) {
  napi_value error;
  napi_status status = create_ffmpeg_error(env, errnum, &error);
  if (status == napi_ok) {
    return napi_throw(env, error);
  } else {
    GET_AND_THROW_LAST_ERROR(env);
    return napi_ok;
  }
}
