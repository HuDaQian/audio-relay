#include "jitter_buffer.h"

#include <algorithm>

namespace audio_relay {
namespace {

// How far a packet's sequence number may sit from the play position before we
// stop calling it "jitter" and treat it as a desynchronisation.
constexpr uint32_t kResyncDistance = 200;

// Hard cap on buffered packets so a stalled consumer can't grow without bound.
constexpr size_t kMaxChunks = 64;

// Consecutive concealed chunks before we give up on the current play position.
// ~500ms at 10ms packets.
constexpr uint32_t kMaxConceal = 50;

}  // namespace

JitterBuffer::JitterBuffer(size_t target_depth_chunks)
    : target_depth_chunks_(target_depth_chunks == 0 ? 3 : target_depth_chunks) {}

void JitterBuffer::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  resync_locked();
}

void JitterBuffer::resync_locked() {
  chunks_.clear();
  next_seq_ = 0;
  started_ = false;
  chunk_frames_ = 0;
  chunk_pos_ = 0;
  conceal_count_ = 0;
}

void JitterBuffer::push(uint32_t seq, const int16_t* frames, size_t count) {
  if (count == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (started_) {
    // Signed distance handles uint32 wraparound for the ~49-day window we
    // actually care about (sessions are hours at most).
    int64_t dist = static_cast<int64_t>(seq) - static_cast<int64_t>(next_seq_);
    if (dist < -static_cast<int64_t>(kResyncDistance) ||
        dist > static_cast<int64_t>(kResyncDistance)) {
      // The stream is nowhere near our play position; rebuild from here.
      resync_locked();
    } else if (dist < 0) {
      return;  // late or duplicate
    }
  }

  if (chunks_.find(seq) != chunks_.end()) {
    return;  // duplicate
  }

  if (chunks_.size() >= kMaxChunks) {
    chunks_.erase(chunks_.begin());
  }

  if (chunk_frames_ == 0) {
    chunk_frames_ = count;
  }
  chunks_.emplace(seq, std::vector<int16_t>(frames, frames + count));
}

size_t JitterBuffer::depth() {
  std::lock_guard<std::mutex> lock(mutex_);
  return chunks_.size();
}

size_t JitterBuffer::pop(int16_t* out, size_t max_frames) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!started_) {
    if (chunks_.size() < target_depth_chunks_) {
      return 0;  // still pre-buffering
    }
    started_ = true;
    next_seq_ = chunks_.begin()->first;
    chunk_pos_ = 0;
    conceal_count_ = 0;
    if (chunk_frames_ == 0 && !chunks_.empty()) {
      chunk_frames_ = chunks_.begin()->second.size();
    }
  }

  size_t written = 0;
  while (written < max_frames) {
    // Shed standing backlog before it becomes permanent latency. This is the
    // "sender faster than the render device" drift case: rather than drifting
    // further behind, drop the excess buffered packets in one step.
    if (chunk_pos_ == 0 && chunks_.size() > target_depth_chunks_ * 2) {
      while (chunks_.size() > target_depth_chunks_) {
        chunks_.erase(chunks_.begin());
      }
      if (!chunks_.empty()) {
        next_seq_ = chunks_.begin()->first;
      }
      chunk_pos_ = 0;
      conceal_count_ = 0;
    }

    auto it = chunks_.find(next_seq_);
    if (it != chunks_.end()) {
      const auto& chunk = it->second;
      if (chunk_pos_ < chunk.size()) {
        out[written++] = chunk[chunk_pos_++];
      } else {
        chunks_.erase(it);
        next_seq_++;
        chunk_pos_ = 0;
        conceal_count_ = 0;
      }
    } else {
      // Missing packet: conceal with exactly one packet's worth of silence so
      // the play head keeps advancing and later packets are not discarded.
      size_t conceal_len = (chunk_frames_ == 0) ? 480 : chunk_frames_;
      out[written++] = 0;
      chunk_pos_++;
      if (chunk_pos_ >= conceal_len) {
        next_seq_++;
        chunk_pos_ = 0;
        if (++conceal_count_ > kMaxConceal) {
          resync_locked();
          return written;
        }
      }
    }
  }

  return written;
}

}  // namespace audio_relay
