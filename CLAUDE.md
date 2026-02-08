# audio-rtp-tools

Node.js native addon for real-time audio RTP streaming with Opus encoding/decoding.

## Build

- Native C++ module: `node-gyp rebuild` (requires ffmpeg and opus libs via homebrew on macOS)
- TypeScript: `npm run build` (runs `tsc`, outputs to `dist/`)
- Entry point: `dist/index.js`, native binary: `build/Release/worker.node`

## Architecture

### TypeScript layer (`src/index.ts`)

Exports two functions:
- `produceRtp(options)` — Encodes PCM audio to Opus and sends over RTP
- `consumeRtp(options)` — Receives RTP Opus and decodes to PCM

Both use `AbortController` for lifecycle management. The native addon is loaded from `build/Release/worker.node`.

### Native addon (`src/worker.cc`)

NAPI entry point in the `hilokal` namespace. Registers all JS-callable functions in `init()`. Uses helper functions (`get_option_string`, `get_option_int32`, `get_option_bool`) to extract typed values from JS option objects.

### Thread architecture

All audio processing runs on dedicated pthreads, not the Node.js event loop. Communication between JS and threads uses `AVThreadMessageQueue` (from ffmpeg's libavutil).

**Thread lifecycle pattern** (`src/thread_with_promise_result.h`):
- `start_thread_with_promise_result<T>()` is a template that creates a message queue, spawns a pthread, and returns a NAPI promise + external (the message queue handle)
- The JS side posts messages to the queue via the external handle
- Thread shutdown is triggered by `AbortController.abort()` which sets `AVERROR_EOF` on the message queue
- When the thread exits, it signals back via `uv_async_t` to resolve the promise on the main thread
- The `ThreadData<T>` class owns the thread state and is cleaned up in the uv close callback

**Message system** (`src/thread_messages.h`, `src/thread_messages.cc`):
- `ThreadMessage` struct: type enum + union parameter + optional uv_async
- `ThreadMessageParameter` union holds: `AVPacket*`, `int64_t` timestamps, `AVCodecParameters*`, `AVBufferRef*` (for buffers), or `int32_t` (for scalar config values)
- Each message type has a `post_*_to_thread()` function
- `thread_message_free_func` handles cleanup based on message type — must be called for messages containing allocated resources (packets, buffers, codec params). Scalar messages (bitrate/FEC/packet loss) don't need cleanup.

### Encode pipeline (`src/audio_encode_thread.cc`)

`audio_encode_thread` → spawns a child `producer_thread` (raw pthread, not NAPI-managed):
1. Creates an Opus encoder (caller-specified sample rate via `params.sampleRate`, stereo, VOIP application)
2. Starts a producer thread with RTP/SRTP parameters
3. Main loop: receives `POST_PCM_BUFFER` messages, accumulates mono 16-bit PCM into 20ms frames, converts mono→stereo, encodes with libopus, posts `AVPacket` to producer thread
4. Also handles runtime config messages: `SET_ENCODER_BITRATE`, `SET_ENCODER_FEC`, `SET_ENCODER_PACKET_LOSS_PERC`

Key constants: output PTS at 48kHz, 20ms frames, max opus frame 1275 bytes. Input sample rate is configurable.

### Decode pipeline (`src/audio_decode_thread.cc`)

Receives RTP via SDP, decodes Opus to PCM, delivers audio data back to JS via a `uv_async_t` buffer-ready callback.

### Producer thread (`src/producer_thread.cc`)

Muxes `AVPacket`s into an RTP/SRTP output stream using ffmpeg's libavformat. Has both a NAPI API (for direct JS use) and a raw pthread API (for use as a child of the encoder thread).

### Demuxer (`src/demuxer.cc`)

Handles OGG demuxing for text-to-speech buffer streaming. Separate from the RTP encode/decode path.

## Key patterns

- Native code uses `av_strdup()` for strings passed across thread boundaries (freed with `av_free()`)
- Buffer data is copied into `AVBufferRef` when posting to threads (zero-copy within ffmpeg after that)
- Non-blocking sends (`AV_THREAD_MESSAGE_NONBLOCK`) with EAGAIN handling for queue-full scenarios
- The encoder thread uses `OPUS_AUTO` for bitrate when 0 is passed, falls back to 32000 otherwise
