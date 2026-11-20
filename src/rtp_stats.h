#pragma once

#include <atomic>
#include <cstdint>

extern "C" {
#include <libavutil/buffer.h>
}

// Counters for one RTP receiver. Written by its demuxer thread, partly from the
// ffmpeg log callback, and read from the JS thread, so every field is atomic.
struct RtpReceiveStats {
  // Packets the demuxer handed to the decoder
  std::atomic<int64_t> packets_received{0};
  // Sequence numbers skipped: the packet never arrived, or not in time. Not
  // counted when reordering is off (max_delay 0).
  std::atomic<int64_t> packets_lost{0};
  // Packets dropped on arrival for being older than the last one passed on.
  // Mostly packets that came after the wait for them ran out (already counted
  // in packets_lost), but also an old duplicate, and every packet that follows
  // a backwards jump in the sender's sequence numbers.
  std::atomic<int64_t> packets_late{0};
  // Times the wait for a missing packet ran out (max_delay)
  std::atomic<int64_t> reorder_timeouts{0};
  // Times the reorder queue filled up before the wait ran out
  std::atomic<int64_t> reorder_queue_overflows{0};
};

// Allocates zeroed stats in a refcounted buffer, so the JS handle and the
// decode thread can each hold a reference and release it in either order.
AVBufferRef *rtp_stats_alloc();

static inline RtpReceiveStats *rtp_stats_from_ref(AVBufferRef *ref) {
  return ref != NULL ? (RtpReceiveStats *)ref->data : NULL;
}

// The ffmpeg log callback has no handle on the receiver a message is about,
// but it runs on the thread that made the libavformat call. A demuxer thread
// registers its stats here so the callback can find them.
void rtp_stats_set_for_current_thread(RtpReceiveStats *stats);
RtpReceiveStats *rtp_stats_for_current_thread();
