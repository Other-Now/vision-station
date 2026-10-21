#include "core/shm_ring.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace vs {
static_assert(std::atomic<uint64_t>::is_always_lock_free, "ring needs lock-free 64-bit atomics across processes");
namespace {
constexpr uint32_t kMagic = 0x52535356;  // "VSSR"
size_t round_up(size_t v, size_t a) { return (v + a - 1) / a * a; }
}  // namespace

ShmRing::~ShmRing() {
  if (hdr_) munmap(hdr_, map_bytes_);
  if (owner_) shm_unlink(name_.c_str());
}

ShmSlotHeader* ShmRing::slot(uint64_t i) const {
  auto* base = reinterpret_cast<uint8_t*>(hdr_) + round_up(sizeof(ShmRingHeader), 64);
  return reinterpret_cast<ShmSlotHeader*>(base + (i % hdr_->slot_count) * stride_);
}

bool ShmRing::create(const std::string& name, uint32_t slot_count, uint32_t slot_bytes) {
  name_ = name;
  shm_unlink(name.c_str());  // stale segment from a crashed run
  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) { error_ = std::string("shm_open: ") + std::strerror(errno); return false; }
  stride_ = round_up(sizeof(ShmSlotHeader) + slot_bytes, 64);
  map_bytes_ = round_up(sizeof(ShmRingHeader), 64) + stride_ * slot_count;
  if (ftruncate(fd, static_cast<off_t>(map_bytes_)) != 0) {
    error_ = std::string("ftruncate: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  void* p = mmap(nullptr, map_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) { error_ = std::string("mmap: ") + std::strerror(errno); return false; }
  hdr_ = static_cast<ShmRingHeader*>(p);
  owner_ = true;
  hdr_->slot_count = slot_count;
  hdr_->slot_bytes = slot_bytes;
  hdr_->next_index.store(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < slot_count; ++i) slot(i)->seqlock.store(0, std::memory_order_relaxed);
  // Publish the magic last: a reader that sees it sees an initialised ring.
  std::atomic_thread_fence(std::memory_order_release);
  hdr_->magic = kMagic;
  return true;
}

bool ShmRing::open(const std::string& name) {
  name_ = name;
  int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) { error_ = std::string("shm_open: ") + std::strerror(errno); return false; }
  struct stat st{};
  fstat(fd, &st);
  map_bytes_ = static_cast<size_t>(st.st_size);
  void* p = mmap(nullptr, map_bytes_, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) { error_ = std::string("mmap: ") + std::strerror(errno); return false; }
  hdr_ = static_cast<ShmRingHeader*>(p);
  if (hdr_->magic != kMagic) { error_ = "bad magic (writer not ready)"; return false; }
  stride_ = round_up(sizeof(ShmSlotHeader) + hdr_->slot_bytes, 64);
  return true;
}

void ShmRing::publish(const uint8_t* data, uint32_t width, uint32_t height, uint64_t frame_seq,
                      uint64_t now_ns) {
  const uint64_t idx = hdr_->next_index.load(std::memory_order_relaxed);
  ShmSlotHeader* s = slot(idx);
  const uint32_t bytes = std::min(width * height, hdr_->slot_bytes);
  // seqlock write: odd -> body -> even (2*idx+2 identifies *which* frame).
  s->seqlock.store(2 * idx + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s->index = idx;
  s->frame_seq = frame_seq;
  s->publish_ns = now_ns;
  s->width = width;
  s->height = height;
  s->bytes = bytes;
  std::memcpy(payload(s), data, bytes);
  s->seqlock.store(2 * idx + 2, std::memory_order_release);
  hdr_->next_index.store(idx + 1, std::memory_order_release);
}

ShmRing::Read ShmRing::read(uint64_t index, uint8_t* out, ShmSlotHeader* meta) const {
  if (index >= published()) return Read::NotYet;
  ShmSlotHeader* s = slot(index);
  const uint64_t want = 2 * index + 2;
  const uint64_t a = s->seqlock.load(std::memory_order_acquire);
  if (a > want) return Read::Overrun;   // already reused for a later frame
  if (a != want) return Read::NotYet;   // still being written
  meta->index = s->index;
  meta->frame_seq = s->frame_seq;
  meta->publish_ns = s->publish_ns;
  meta->width = s->width;
  meta->height = s->height;
  meta->bytes = s->bytes;
  std::memcpy(out, payload(s), std::min(meta->bytes, hdr_->slot_bytes));
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint64_t b = s->seqlock.load(std::memory_order_relaxed);
  if (b != want) return b > want ? Read::Overrun : Read::Torn;
  return Read::Ok;
}

}  // namespace vs
