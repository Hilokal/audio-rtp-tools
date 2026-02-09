#include <node_api.h>

extern "C" {
#include <math.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/threadmessage.h>
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <uv.h>
#include <time.h>
}

#include "demuxer.h"
#include "buffer_ready_node_callback.h"
#include "producer_thread.h"
#include "node_errors.h"
#include "thread_messages.h"
#include "audio_decode_thread.h"
#include "audio_encode_thread.h"
#define SDP_MAX_SIZE 2046

namespace hilokal {

  napi_status is_nullish(napi_env env, napi_value value, bool *result) {
    napi_status status;

    napi_value null_value, undefined_value;
    status = napi_get_null(env, &null_value);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_get_undefined(env, &undefined_value);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_strict_equals(env, value, null_value, result);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return status;
    }

    // Return early if null
    if (*result) {
      return status;
    }

    return napi_strict_equals(env, value, undefined_value, result);
  }

  napi_status get_option_double(napi_env env, napi_value options, const char *key, double *value) {
    napi_status status;
    napi_value prop_value;
    napi_value js_key;

    status = napi_create_string_utf8(env, key, NAPI_AUTO_LENGTH, &js_key);
    if (status != napi_ok)
      return status;

    status = napi_get_property(env, options, js_key, &prop_value);
    if (status != napi_ok) {
      return status;
    }

    status = napi_get_value_double(env, prop_value, value);
    if (status != napi_ok) {
      return status;
    }

    return status;
  }

  napi_status get_option_bool(napi_env env, napi_value options, const char *key, bool *value) {
    napi_status status;
    napi_value prop_value;
    napi_value js_key;

    status = napi_create_string_utf8(env, key, NAPI_AUTO_LENGTH, &js_key);
    if (status != napi_ok)
      return status;

    status = napi_get_property(env, options, js_key, &prop_value);
    if (status != napi_ok) {
      return status;
    }

    status = napi_get_value_bool(env, prop_value, value);
    if (status != napi_ok) {
      return status;
    }

    return status;
  }

  napi_status get_option_int32(napi_env env, napi_value options, const char *key, int32_t *value) {
    napi_status status;
    napi_value prop_value;
    napi_value js_key;

    status = napi_create_string_utf8(env, key, NAPI_AUTO_LENGTH, &js_key);
    if (status != napi_ok)
      return status;

    status = napi_get_property(env, options, js_key, &prop_value);
    if (status != napi_ok) {
      return status;
    }

    status = napi_get_value_int32(env, prop_value, value);
    if (status != napi_ok) {
      return status;
    }

    return status;
  }

  napi_status get_option_uint32(napi_env env, napi_value options, const char *key, uint32_t *value) {
    napi_status status;
    napi_value prop_value;
    napi_value js_key;

    status = napi_create_string_utf8(env, key, NAPI_AUTO_LENGTH, &js_key);
    if (status != napi_ok)
      return status;

    status = napi_get_property(env, options, js_key, &prop_value);
    if (status != napi_ok) {
      return status;
    }

    status = napi_get_value_uint32(env, prop_value, value);
    if (status != napi_ok) {
      return status;
    }

    return status;
  }

  // strings will be allocated with av_strdup(), so should be freed with av_free() or a similar api.
  napi_status get_option_string(napi_env env, napi_value options, const char *key, char **value) {
    napi_status status;
    napi_value prop_value;
    napi_value js_key;

    status = napi_create_string_utf8(env, key, NAPI_AUTO_LENGTH, &js_key);
    if (status != napi_ok)
      return status;

    status = napi_get_property(env, options, js_key, &prop_value);
    if (status != napi_ok) {
      return status;
    }

    bool nullish;
    status = is_nullish(env, prop_value, &nullish);
    if (status != napi_ok) {
      return status;
    }

    if (nullish) {
      *value = NULL;
      return status;
    }

    char string_buffer[2 * 1024];
    size_t string_size;

    status = napi_get_value_string_utf8(env, prop_value, string_buffer, sizeof(string_buffer), &string_size);
    if (status != napi_ok) {
      return status;
    }

    *value = av_strdup(string_buffer);

    return status;
  }

  napi_value startDemuxerJob(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_value ret;
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    napi_value abort_signal = args[1];
    napi_value external;
    napi_value promise;

    status = start_file_demuxer(env, args[0], abort_signal, &external, &promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_create_object(env, &ret);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "external", external);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "promise", promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    return ret;
  }

  napi_value startProducerJob(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_value ret;
    napi_status status = napi_ok;

    struct ProducerThreadParams params = {};

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "url", &params.url);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "cname", &params.cname);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "cryptoSuite", &params.cryptoSuite);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "keyBase64", &params.keyBase64);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "payloadType", &params.payloadType);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "ssrc", &params.ssrc);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    if (status != napi_ok) {
      av_freep(&params.url);
      av_freep(&params.cname);
      av_freep(&params.cryptoSuite);
      av_freep(&params.keyBase64);
      av_freep(&params.payloadType);
      av_freep(&params.ssrc);
      return NULL;
    }

    napi_value abort_signal = args[0];
    napi_value external;
    napi_value muxer_promise;

    status = start_producer_thread(env, params, abort_signal, &external, &muxer_promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_create_object(env, &ret);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "external", external);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "muxer_promise", muxer_promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    return ret;
  }

  napi_value clearMessageQueue(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 1;
    napi_value args[1];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    ThreadMessage thread_message;

    while (true) {
      int ret = av_thread_message_queue_recv(message_queue, &thread_message, AV_THREAD_MESSAGE_NONBLOCK);
      if (ret < 0) {
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
          fprintf(stderr, "clearMessageQueue: av_thread_message_queue_recv() returned error [%d]\n", ret);
        }
        break;
      } else {
        thread_message_free_func(&thread_message);
      }
    }

    return NULL;
  }

  napi_value postEndOfFile(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 1;
    napi_value args[1];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    av_thread_message_queue_set_err_send(message_queue, AVERROR_EOF);
    av_thread_message_queue_set_err_recv(message_queue, AVERROR_EOF);

    return NULL;
  }

  napi_value startAudioDecodeThread(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 4;
    napi_value args[4];
    napi_value ret;
    napi_status status = napi_ok;
    napi_value promise;

    struct AudioDecodeThreadParams params = {};

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    napi_valuetype valuetype1;
    napi_typeof(env, args[1], &valuetype1);
    if (valuetype1 != napi_function) {
      fprintf(stderr, "[TYPE ERROR] Expects a function as second argument. type [%d]\n", valuetype1);
    }

    char sdpBase64[SDP_MAX_SIZE];
    size_t sdpSize;

    status = napi_get_value_string_utf8(env, args[0], sdpBase64, sizeof(sdpBase64), &sdpSize);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    // This will get freed inside of thread cleanup code
    params.sdpBase64 = av_strdup(sdpBase64);

    status = get_option_int32(env, args[3], "sampleRate", &params.sampleRate);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_int32(env, args[3], "channels", &params.channels);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    if (status != napi_ok) {
      av_freep(&params.sdpBase64);
      return NULL;
    }

    napi_value on_audio_callback = args[1];
    napi_value abort_signal = args[2];
    napi_value external;

    status = start_audio_decode_thread(env, params, abort_signal, on_audio_callback, &external, &promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_create_object(env, &ret);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "external", external);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "promise", promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    return ret;
  }

  napi_value postDemuxerReset(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 1;
    napi_value args[1];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);

    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    int ret = post_ogg_reset_demuxer_to_thread(message_queue);
    if (ret != 0) {
      throw_ffmpeg_error(env, ret);
    }

    return NULL;
  }

  napi_value postOggBuffer(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);

    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    void *buffer;
    size_t buffer_length;
    status = napi_get_buffer_info(env, args[1], &buffer, &buffer_length);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    bool success = true;

    if (buffer_length > 0) {
      int ret = post_ogg_buffer_to_thread(message_queue, buffer, buffer_length);
      if (ret == AVERROR(EAGAIN)) {
        success = false;
      } else if (ret != 0) {
        throw_ffmpeg_error(env, ret);
        return NULL;
      }
    }

    napi_value return_value;
    status = napi_get_boolean(env, success, &return_value);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    return return_value;
  }

  napi_value postSetBitrate(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    int32_t bitrate;
    status = napi_get_value_int32(env, args[1], &bitrate);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    post_set_bitrate_to_thread(message_queue, bitrate);
    return NULL;
  }

  napi_value postSetEnableFec(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    bool enable;
    status = napi_get_value_bool(env, args[1], &enable);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    post_set_fec_to_thread(message_queue, enable);
    return NULL;
  }

  napi_value postSetPacketLossPercent(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    int32_t percent;
    status = napi_get_value_int32(env, args[1], &percent);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    post_set_packet_loss_perc_to_thread(message_queue, percent);
    return NULL;
  }

  napi_value startAudioEncodeThread(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_value ret;
    napi_status status = napi_ok;

    struct AudioEncodeThreadParams params = {};

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "rtpUrl", &params.rtpUrl);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "ssrc", &params.ssrc);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "payloadType", &params.payloadType);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "cname", &params.cname);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "cryptoSuite", &params.cryptoSuite);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_string(env, args[1], "keyBase64", &params.keyBase64);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);


    status = get_option_int32(env, args[1], "bitrate", &params.bitrate);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_bool(env, args[1], "enableFec", &params.enableFec);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_int32(env, args[1], "packetLossPercent", &params.packetLossPercent);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = get_option_int32(env, args[1], "sampleRate", &params.sampleRate);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    if (status != napi_ok) {
        av_freep(&params.rtpUrl);
        av_freep(&params.ssrc);
        av_freep(&params.payloadType);
        av_freep(&params.cname);
        av_freep(&params.cryptoSuite);
        av_freep(&params.keyBase64);
        return NULL;
    }

    napi_value abort_signal = args[0];
    napi_value external;
    napi_value promise;

    status = start_audio_encode_thread(env, params, abort_signal, &external, &promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_create_object(env, &ret);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "external", external);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, ret, "promise", promise);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    return ret;
  }

  napi_value postFlushEncoder(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 1;
    napi_value args[1];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    post_flush_encoder_to_thread(message_queue);
    return NULL;
  }

  napi_value postClearProducerQueue(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 1;
    napi_value args[1];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) { GET_AND_THROW_LAST_ERROR(env); return NULL; }

    post_clear_producer_queue_to_thread(message_queue);
    return NULL;
  }

  napi_value postPcmToEncoder(napi_env env, napi_callback_info cbinfo) {
    size_t argsLength = 2;
    napi_value args[2];
    napi_status status = napi_ok;

    status = napi_get_cb_info(env, cbinfo, &argsLength, args, NULL, 0);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    AVThreadMessageQueue *message_queue;
    status = napi_get_value_external(env, args[0], (void **)&message_queue);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    void *buffer;
    size_t buffer_length;
    status = napi_get_buffer_info(env, args[1], &buffer, &buffer_length);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    // TODO: I don't understand why we are returning a bool here, rather than throwing an exception.
    // I think this might be a hallucination. Or.. maybe we need to return false to indicate the message queue is full
    bool success = true;

    if (buffer_length > 0) {
      int ret = post_pcm_buffer_to_thread(message_queue, buffer, buffer_length);
      if (ret == AVERROR(EAGAIN)) {
        success = false;
      } else if (ret != 0) {
        throw_ffmpeg_error(env, ret);
        return NULL;
      }
    }

    napi_value return_value;
    status = napi_get_boolean(env, success, &return_value);
    if (status != napi_ok) {
      GET_AND_THROW_LAST_ERROR(env);
      return NULL;
    }

    return return_value;
  }

  napi_status create_function_property(napi_env env, napi_value object, const char *fnName, napi_callback cb) {
    napi_status status;
    napi_value js_function;

    status = napi_create_function(env, fnName, NAPI_AUTO_LENGTH, cb, nullptr, &js_function);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = napi_set_named_property(env, object, fnName, js_function);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    return status;
  }

  int dropped_packets = 0;

  void av_log_override_callback(void* ptr, int level, const char* fmt, va_list vl) {
    // The default ffmpeg logger can get spammy with dropped packets, so we supress some log items
    if (!strcmp(fmt, "max delay reached. need to consume packet\n")) {
      return;
    } else if (!strcmp(fmt, "RTP: dropping old packet received too late\n")) {
      return;
    } else if (!strcmp(fmt, "track %d: codec frame size is not set\n")) {
      // This is a harmless warning that comes when muxing and opus stream to mp4.
      // https://superuser.com/questions/1323387/ffmpeg-error-track-x-codec-frame-size-is-not-set
      // https://forum.videohelp.com/threads/404969-ffmpeg-MKV-to-MP4-error-track-1-codec-frame-size-is-not-set
      return;
    } else if (!strcmp(fmt, "RTP: missed %d packets\n")) {
      int count = va_arg(vl, int);
      dropped_packets += count;
    } else {
      av_log_default_callback(ptr, level, fmt, vl);
    }
  }

  sig_t old_siguser2_handler = NULL;

  void sigusr2_handler(int signal) {
    // This function doesn't need to do anything. It is is only set because the default handler will terminate the process
    if (old_siguser2_handler != NULL) {
      // We expect this value to be NULL, but check just in case.
      old_siguser2_handler(signal);
    }
  }

  napi_value init(napi_env env, napi_value exports) {
    napi_status status;

    status = create_function_property(env, exports, "postOggBuffer", postOggBuffer);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postDemuxerReset", postDemuxerReset);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "clearMessageQueue", clearMessageQueue);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postEndOfFile", postEndOfFile);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "startAudioDecodeThread", startAudioDecodeThread);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "startProducerJob", startProducerJob);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "startDemuxerJob", startDemuxerJob);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "startAudioEncodeThread", startAudioEncodeThread);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postPcmToEncoder", postPcmToEncoder);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postSetBitrate", postSetBitrate);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postSetEnableFec", postSetEnableFec);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postSetPacketLossPercent", postSetPacketLossPercent);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postFlushEncoder", postFlushEncoder);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    status = create_function_property(env, exports, "postClearProducerQueue", postClearProducerQueue);
    if (status != napi_ok) GET_AND_THROW_LAST_ERROR(env);

    av_log_set_callback(av_log_override_callback);

    old_siguser2_handler = signal(SIGUSR2, sigusr2_handler);

    return exports;
  }

  NAPI_MODULE(NODE_GYP_MODULE_NAME, init)

} // namespace hilokal
