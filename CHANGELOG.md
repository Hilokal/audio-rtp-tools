# Changelog

## [1.5.0] - 2026-10-20

- `consumeRtp()` caps packet-loss concealment at 3 seconds per RTP timestamp gap. Previously every missing frame was synthesized, so a speaker returning from a long DTX silence or a paused producer arrived as one burst of concealed frames covering the whole gap (hours, in production). Gaps longer than the cap now skip to the next packet with the decoder state reset.
- `onAudioData` receives a `discontinuity: boolean` field: true on the first frame after a gap that exceeded the cap, false otherwise (additive; existing callers are unaffected).
- `onAudioData` receives a `concealed: boolean` field: true for frames the decoder synthesized rather than decoded from audio the sender sent (PLC or FEC filling a timestamp gap, and DTX / keep-alive packets of 2 bytes or less), false otherwise (additive; existing callers are unaffected). During Opus DTX silence this separates the ~400 ms comfort-noise refreshes, which carry the sender's real background, from the concealment around them.
- The decode summary line reports a gap histogram (up to 400 ms, up to 5 s, longer) and the total time skipped.
- Aborting the `signal` passed to `produceRtp()` now drops the packets already queued for sending, so `done()` resolves right away, including when `end()` was called first. Previously only unencoded PCM was dropped: the request to clear the producer thread's queue was posted through the encoder's message queue after the abort had closed it, so it never arrived and queued audio played out in real time. The abort handler now flushes the queue and posts an `ABORT_SIGNAL` message before closing it.

## [1.4.0] - 2026-08-14

- **Breaking**: FEC and DTX are now configured via the Opus codec parameters in `rtpParameters` (`useinbandfec` and `usedtx`) instead of the `opus.enableFec` / `opus.enableDtx` options on `produceRtp()`, which have been removed. This keeps the encoder and the signaled parameters in sync by construction.
- `createRtpParameters()` accepts `{ enableFec?, enableDtx? }` options (defaults: FEC on, DTX off). `enableDtx` also sets `dtx: true` on the encoding, which disables mediasoup's RTP inactivity checks.
- Behavior change: `produceRtp()` now honors `useinbandfec` from `rtpParameters` — with `createRtpParameters()` defaults, FEC is actually enabled on the encoder (previously it was signaled but only enabled if `opus.enableFec` was passed).

## [1.3.0] - 2026-08-14

- Add DTX-style keep-alive packets to `produceRtp()`, enabled by default. After ~400ms with no audio written, a 1-byte Opus DTX packet is sent to keep NAT bindings, RTCP, and the receiver's jitter buffer warm during gaps between audio segments. Disable with `keepAlive: false`.
- Add Opus DTX support to `produceRtp()` via the `opus.enableDtx` option and `setEnableDtx()` runtime toggle. When enabled, silent input frames are suppressed except for a periodic comfort-noise refresh, mirroring WebRTC's DTX wire behavior.

## [1.2.0] - 2026-04-29

- Add `localRtpPort` and `localRtcpPort` options to `produceRtp()` for binding specific local UDP ports

## [1.1.0] - 2026-03-21

- Add `saveAudio()` function for recording RTP Opus streams directly to a WebM file without decoding

## [1.0.2] - 2026-02-24

- Fix audio corruption bug when encoding PCM buffers of non-even sizes

## [1.0.1] - 2026-02-16

- Add API for backpressure

## [1.0.0] - 2026-02-04

- Initial release
