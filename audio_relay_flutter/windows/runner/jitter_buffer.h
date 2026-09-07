#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <mutex>
#include <vector>

namespace audio_relay {

// Sequence-aware jitter buffer for the desktop microphone receiver.
//
// Operates on mono int16 PCM frames that have already been resampled to the
// render device's sample rate. It mirrors the responsibilities of the Android
// receiver's `JitterBuffer` but is deliberately simpler: the desktop render
// path is paced by the output device clock, so the buffer only needs to
// reorder, conceal, bound latency, and recover from desynchronisation.
class JitterBuffer {
 public:
  explicit JitterBuffer(size_t target_depth_chunks = 3);

  void reset();

  // Adds one decoded packet's worth of mono PCM frames.
  // Silently drops late/duplicate packets and bounds memory.
  void push(uint32_t seq, const int16_t* frames, size_t count);

  // Returns up to `max_frames` mono samples into `out`, writing `0` for
  // anything still being pre-buffered or concealed. Returns the number of
  // frames actually written (0 while still pre-buffering).
  size_t pop(int16_t* out, size_t max_frames);

  size_t depth();

 private:
  void resync_locked();

  mutable std::mutex mutex_;
  std::map<uint32_t, std::vector<int16_t>> chunks_;
  uint32_t next_seq_ = 0;
  bool started_ = false;
  size_t chunk_frames_ = 0;  // learned size of one packet (for concealment)
  size_t chunk_pos_ = 0;     // frames already consumed from the current chunk
  uint32_t conceal_count_ = 0;
  size_t target_depth_chunks_;
};

}  // namespace audio_relay
