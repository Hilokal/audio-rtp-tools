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

```bash
npm install audio-rtp-tools
```

This runs `node-gyp rebuild` during install to compile the native addon.

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

const producer = produceRtp({
  ipAddress: "127.0.0.1",
  rtpPort: 10000,
  rtcpPort: 10001,
  rtpParameters,
  srtpParameters,
  sampleRate: 24000,
  onError: (err) => console.error(err),
});

// Write 16-bit mono PCM data
producer.write(pcmBuffer);
producer.end();

await producer.done();
```

### Receiving audio from RTP

```js
const {
  consumeRtp,
  createSDP,
  createRtpParameters,
  createSrtpParameters,
} = require("audio-rtp-tools");

const rtpParameters = createRtpParameters();
const srtpParameters = createSrtpParameters();

const sdp = createSDP({
  destinationIpAddress: "127.0.0.1",
  rtpPort: 10000,
  rtcpPort: 10001,
  rtpParameters,
  srtpParameters,
});

const abortController = new AbortController();

const consumer = consumeRtp({
  sdp,
  sampleRate: 16000,
  signal: abortController.signal,
  onAudioData: ({ buffer, pts }) => {
    // buffer is 16-bit mono PCM at the requested sample rate
    process.stdout.write(buffer);
  },
  onError: (err) => console.error(err),
});

// Later, when you want to stop:
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
| `opus.bitrate` | `number \| null?` | Opus encoder bitrate in bps, or `null` for auto |
| `opus.enableFec` | `boolean?` | Enable forward error correction |
| `opus.packetLossPercent` | `number?` | Expected packet loss percentage (helps FEC) |

**Returns** an object with:

- **`write(data: Buffer): boolean`** — Queue PCM data for encoding. Data should be 16-bit signed mono PCM at the sample rate specified in options.
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

## Building from source

```bash
git clone <repo-url>
cd audio-rtp-tools
npm install
node-gyp rebuild
npm run build
```

## Tests

```bash
npm test
```

## AI disclosure

The core of this project — the native C++ addon, threading architecture, encode/decode pipelines, and RTP/SRTP integration — was written by hand and runs in production. [Claude Code](https://claude.ai/claude-code) was used to clean up the TypeScript API, write documentation (including this README), and prepare the project for open source publishing.

## License

MIT
