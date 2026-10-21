#pragma once
// Single-producer / single-consumer frame ring in POSIX shared memory.
//
// The station (writer) must never wait on the recorder (reader), so the writer
// always overwrites the oldest slot. Each slot carries a sequence number used
// as a seqlock: odd while being written, 2*frame_index+2 when complete. The
// reader detects (a) a slot overwritten before it got there -> counted as an
// overrun, and (b) a slot modified while it was copying -> torn, retried.
#include <atomic>
#include <cstdint>
#include <string>

namespace vs {

struct ShmRingHeader {
  uint32_t magic;
  uint32_t slot_count;
  uint32_t slot_bytes;  // max payload per slot
  uint32_t pad;
  alignas(64) std::atomic<uint64_t> next_index;  // frames published so far
};

struct ShmSlotHeader {
  alignas(64) std::atomic<uint64_t> seqlock;
  uint64_t index;      // writer frame index
  uint64_t frame_seq;  // rig sequence from the truth stamp
  uint64_t publish_ns;
  uint32_t width, height, bytes, pad;
};

class ShmRing {
 public:
  ShmRing() = default;
  ~ShmRing();
  ShmRing(const ShmRing&) = delete;
  ShmRing& operator=(const ShmRing&) = delete;

  // Writer creates (and owns/unlinks) the segment; reader opens it.
  bool create(const std::string& name, uint32_t slot_count, uint32_t slot_bytes);
  bool open(const std::string& name);
  const std::string& error() const { return error_; }

  // Writer side. Never blocks.
  void publish(const uint8_t* data, uint32_t width, uint32_t height, uint64_t frame_seq, uint64_t now_ns);

  // Reader side. Copies frame `index` into out (must hold slot_bytes).
  enum class Read { Ok, NotYet, Overrun, Torn };
  Read read(uint64_t index, uint8_t* out, ShmSlotHeader* meta) const;
  uint64_t published() const { return hdr_ ? hdr_->next_index.load(std::memory_order_acquire) : 0; }
  uint32_t slot_count() const { return hdr_ ? hdr_->slot_count : 0; }
  uint32_t slot_bytes() const { return hdr_ ? hdr_->slot_bytes : 0; }

 private:
  ShmSlotHeader* slot(uint64_t i) const;
  uint8_t* payload(ShmSlotHeader* s) const { return reinterpret_cast<uint8_t*>(s) + sizeof(ShmSlotHeader); }

  ShmRingHeader* hdr_ = nullptr;
  size_t map_bytes_ = 0;
  size_t stride_ = 0;
  std::string name_, error_;
  bool owner_ = false;
};

}  // namespace vs
