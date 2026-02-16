# audio-rtp-tools

A Node.js native addon for real-time audio RTP streaming with Opus encoding/decoding.

Encode PCM audio to Opus and send it over RTP, or receive an RTP Opus stream and decode it back to PCM. All encoding, decoding, and network I/O runs on dedicated native threads — nothing blocks the Node.js event loop.

Built for connecting WebRTC infrastructure like [mediasoup](https://mediasoup.org/) to AI audio models, but has no hard dependency on either. Anything that produces or consumes PCM audio can use it.

## Prerequisites

The native addon links against FFmpeg and Opus libraries. Install them before building:

**macOS (Homebrew)**

```bash
brew install ffmpeg opus
```

**Linux (apt)**

```bash
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libopus-dev
```

## Installation

Make sure you have the [prerequisites](#prerequisites) installed first — `npm install` compiles the native addon from source and needs the FFmpeg and Opus libraries available.

```bash
npm install audio-rtp-tools
```

## Quick start

### Sending audio over RTP

```js
const {
  produceRtp,
  createRtpParameters,
  createSrtpParameters,
} = require("audio-rtp-tools");

const rtpParameters = createRtpParameters();
const srtpParameters = createSrtpParameters();

// This is a signalling function you write.
// Send rtpParameters and srtpParameters to the consuming
// peer, maybe Mediasoup or something else.
// Optionally, use createSDP to convert to SDP format.
const { ipAddress, rtpPort, rtcpPort } = await sendToConsumingPeer({ rtpParameters, srtpParameters })

// This should match the sample rate that you pass into
// write(). This is NOT the same as the sample rate sent
// over RTP. RTP-Opus packets are always 48000.
const sampleRate = 24000;

const producer = produceRtp({
  ipAddress,
  rtpPort,
  rtcpPort,
  rtpParameters,
  srtpParameters,
  sampleRate,
  onError: (err) => console.error(err),
});

// Setup event handlers on an imaginary AI model.
aiModel.on("audio.delta", () => {
  // Queue up 16-bit mono PCM data for sending over RTP
  producer.write(pcmBuffer);
});

aiModel.on("audio.done", () => {
  // This is necessary to keep timestamps consistent
// between separate segments of audio.
  producer.endSegment();
});

aiModel.on("finished", () => {
  // Signal that no more audio will be sent.
  producer.end();
});

await producer.done();
```

### Receiving audio from RTP

```js
const {
  consumeRtp,
  createSDP,
} = require("audio-rtp-tools");

// Pick some local ports that are available. Make sure they
// don't conflict with any other RTP streams or services.
const rtpPort = 10000;
const rtcpPort = 10001;
const ipAddress = "127.0.0.1"

// This is a signalling function you write
const { rtpParameters, srtpParameters } = await sendToProducingPeer({ rtpPort, rtcpPort, ipAddress })

const sdp = createSDP({
  destinationIpAddress: ipAddress,
  rtpPort,
  rtcpPort,
  rtpParameters,
  srtpParameters,
});

const abortController = new AbortController();

// Most AI models want something like 16000.
// OpenAI usually wants 24000. This is the sample rate
// the decoder will generate and pass to onAudioData,
// NOT the sample rate sent over RTP.
// RTP-Opus packets are always 48000.
const sampleRate = 16000;

const consumer = consumeRtp({
  sdp,
  sampleRate,
  signal: abortController.signal,
  onAudioData: ({ buffer, pts }) => {
    // buffer is 16-bit mono PCM at the requested sample rate
    aiModel.processPCMData(buffer);
  },
  onError: (err) => console.error(err),
});

// Called when you want to stop receiving audio.
abortController.abort();
await consumer.done();

```

## API

### `produceRtp(options): ProduceReturn`

Encodes PCM audio to Opus and sends it as an RTP stream.

**Options**

| Name | Type | Description |
|------|------|-------------|
| `ipAddress` | `string` | Destination IP address (IPv4 or IPv6) |
| `rtpPort` | `number` | Destination RTP port |
| `rtcpPort` | `number` | Destination RTCP port |
| `rtpParameters` | `RtpParameters` | RTP codec and encoding configuration |
| `sampleRate` | `number` | Sample rate of the input PCM data |
| `onError` | `(error: Error) => void?` | Error callback (optional) |
| `srtpParameters` | `SrtpParameters?` | SRTP encryption parameters (optional) |
| `signal` | `AbortSignal?` | Abort signal for immediate shutdown (optional) |
| `queueDepth` | `number?` | Encoder message queue depth (default 8192) |
| `onDrain` | `() => void?` | Called when the queue has room after `write()` returned `false`. For advanced backpressure handling (optional) |
| `opus.bitrate` | `number \| null?` | Opus encoder bitrate in bps, or `null` for auto |
| `opus.enableFec` | `boolean?` | Enable forward error correction |
| `opus.packetLossPercent` | `number?` | Expected packet loss percentage (helps FEC) |

**Returns** an object with:

- **`write(data: Buffer): boolean`** — Queue PCM data for encoding. Data should be 16-bit signed mono PCM at the sample rate specified in options. Returns `false` if the queue is full (see [Backpressure](#backpressure)).
- **`endSegment(): void`** — Signal the end of a contiguous audio segment. Flushes any partial frame and resets timing so the next `write()` starts a fresh segment with timestamps rebased to wall-clock time. Call this between distinct stretches of audio (e.g. between AI model turns).
- **`end(): void`** — Signal end of stream. The thread will finish sending queued data before shutting down.
- **`done(): Promise<void>`** — Resolves when the thread has exited.
- **`setBitrate(bitrate: number | null): void`** — Change encoder bitrate at runtime.
- **`setEnableFec(enableFec: boolean): void`** — Toggle FEC at runtime.
- **`setPacketLossPercent(percent: number): void`** — Update expected packet loss at runtime.

### `consumeRtp(options): ConsumeReturn`

Receives an RTP Opus stream and decodes it to PCM.

**Options**

| Name | Type | Description |
|------|------|-------------|
| `sdp` | `string` | SDP describing the RTP stream to receive |
| `sampleRate` | `number` | Output sample rate (8000, 12000, 16000, 24000, or 48000) |
| `signal` | `AbortSignal` | Abort signal to stop the consumer |
| `onAudioData` | `(data: { buffer: Buffer; pts: number \| null }) => void` | Called for each decoded audio frame |
| `onError` | `(error: Error) => void?` | Error callback (optional) |

**Returns** an object with:

- **`done(): Promise<void>`** — Resolves when the thread has exited.

### `createRtpParameters(): RtpParameters`

Creates a default set of RTP parameters for Opus audio with a random SSRC and CNAME. Uses payload type 111 (the WebRTC convention for Opus), 48kHz clock rate, stereo, with FEC enabled.

### `createSrtpParameters(): SrtpParameters`

Generates SRTP encryption parameters with a random 30-byte key and the `AES_CM_128_HMAC_SHA1_80` crypto suite.

### `createSDP(options): string`

Generates an SDP string describing an RTP session. Supports both plain RTP (`RTP/AVPF`) and encrypted SRTP (`RTP/SAVPF`).

**Options**

| Name | Type | Description |
|------|------|-------------|
| `destinationIpAddress` | `string` | **Required.** Connection address |
| `rtpPort` | `number` | **Required.** RTP port |
| `rtpParameters` | `RtpParameters` | **Required.** RTP codec configuration |
| `srtpParameters` | `SrtpParameters?` | SRTP parameters (enables encryption if provided) |
| `rtcpPort` | `number?` | RTCP port |
| `originIpAddress` | `string?` | Origin address (default `"127.0.0.1"`) |
| `userName` | `string?` | SDP user name (default `"-"`) |
| `subject` | `string?` | SDP session subject (default `"audio-rtp-tools"`) |
| `sessionId` | `string?` | SDP session ID (random if omitted) |
| `language` | `string?` | Language tag (e.g. `"en"`) |

### Network utilities

Exported from `audio-rtp-tools/dist/network`:

- **`choosePorts(): Promise<[number, number]>`** — Pick an available even/odd UDP port pair for RTP/RTCP. Configurable via `MIN_RTP_PORT` (default 10000) and `MAX_RTP_PORT` (default 10100) environment variables.
- **`releasePorts(port1, port2): void`** — Return a port pair to the pool.
- **`getListenIp(): string`** — Detect a local IPv4 address suitable for listening.
- **`isLoopback(ipAddress): boolean`** — Check whether an address is a loopback address.

### Types

Both `RtpParameters` and `SrtpParameters` are API-compatible with the types used in [mediasoup](https://mediasoup.org/documentation/v3/mediasoup/rtp-parameters-and-capabilities/) — you can pass them directly without conversion.

## How it works

All audio processing runs on native pthreads, completely off the Node.js event loop:

- **Producer thread**: Receives `AVPacket`s from the encoder and muxes them into an RTP/SRTP output stream using FFmpeg's libavformat.
- **Encoder thread**: Accumulates PCM into 20ms frames, converts mono to stereo, encodes with libopus, and passes packets to the producer thread.
- **Decoder thread**: Receives RTP via SDP, decodes Opus to PCM with libopus, resamples to the requested output sample rate, and delivers audio buffers back to JavaScript via a `uv_async_t` callback.

Communication between JavaScript and native threads uses FFmpeg's `AVThreadMessageQueue`. Lifecycle is managed through `AbortController` — aborting sends `AVERROR_EOF` on the message queue, which causes the thread to exit cleanly and resolve its JavaScript promise.

### Backpressure

The producer thread sends RTP packets at real-time speed, so if your source generates audio faster than real-time (common with AI models that stream in bursts), data queues up internally. The default queue depth of 8192 can absorb roughly 2–3 minutes of audio ahead of real-time playback (assuming typical 20ms writes) — more than enough for typical AI streaming use cases. You can call `write()` without checking its return value and everything will work fine.

For advanced scenarios where you need explicit flow control, `write()` returns a boolean: `true` means the queue accepted the data, `false` means the queue is full and the data was dropped. You can pass an `onDrain` callback in the options to be notified when the queue has room again, and a `queueDepth` to control how deep the queue is. This follows the same convention as Node.js writable streams.

## Building from source

```bash
git clone <repo-url>
cd audio-rtp-tools
npm install
node-gyp rebuild
```

`npm install` installs dependencies and compiles the TypeScript. `node-gyp rebuild` compiles the native addon (this step is automatic when installing from npm, but must be run manually when building from a git clone).

## Tests

```bash
npm test
```

## AI disclosure

The core of this project — the native C++ addon, threading architecture, encode/decode pipelines, and RTP/SRTP integration — was written by hand and runs in production. [Claude Code](https://claude.ai/claude-code) was used to clean up the TypeScript API, write documentation (including this README), and prepare the project for open source publishing.

## License

ISC
