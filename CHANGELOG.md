# Changelog

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
