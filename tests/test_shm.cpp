#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include "core/shm_ring.hpp"

using namespace vs;

namespace {
std::string unique_name() { return "/vs_test_" + std::to_string(getpid()); }
}

TEST(ShmRing, PublishAndReadInOrder) {
  ShmRing w;
  ASSERT_TRUE(w.create(unique_name(), 8, 64)) << w.error();
  ShmRing r;
  ASSERT_TRUE(r.open(unique_name())) << r.error();
  std::vector<uint8_t> px(64), out(64);
  for (int i = 0; i < 5; ++i) {
    std::fill(px.begin(), px.end(), static_cast<uint8_t>(i));
    w.publish(px.data(), 8, 8, 100 + i, 0);
  }
  for (int i = 0; i < 5; ++i) {
    ShmSlotHeader m;
    ASSERT_EQ(r.read(i, out.data(), &m), ShmRing::Read::Ok);
    EXPECT_EQ(m.frame_seq, 100u + i);
    EXPECT_EQ(out[17], i);
  }
  ShmSlotHeader m;
  EXPECT_EQ(r.read(5, out.data(), &m), ShmRing::Read::NotYet);
}

TEST(ShmRing, LappedReaderSeesOverrun) {
  ShmRing w;
  ASSERT_TRUE(w.create(unique_name(), 4, 16));
  ShmRing r;
  ASSERT_TRUE(r.open(unique_name()));
  std::vector<uint8_t> px(16), out(16);
  for (int i = 0; i < 10; ++i) w.publish(px.data(), 4, 4, i, 0);
  ShmSlotHeader m;
  EXPECT_EQ(r.read(0, out.data(), &m), ShmRing::Read::Overrun);
  EXPECT_EQ(r.read(5, out.data(), &m), ShmRing::Read::Overrun);
  EXPECT_EQ(r.read(6, out.data(), &m), ShmRing::Read::Ok);
  EXPECT_EQ(r.read(9, out.data(), &m), ShmRing::Read::Ok);
}

// A fast writer laps a slow reader constantly. Every read that reports Ok must
// contain one frame's bytes only (no tearing), and Ok frames must be in order.
TEST(ShmRing, ConcurrentReadsAreNeverTorn) {
  constexpr uint32_t kBytes = 4096;
  ShmRing w;
  ASSERT_TRUE(w.create(unique_name(), 4, kBytes));
  ShmRing r;
  ASSERT_TRUE(r.open(unique_name()));
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::vector<uint8_t> px(kBytes);
    for (uint64_t i = 0; !stop; ++i) {
      std::fill(px.begin(), px.end(), static_cast<uint8_t>(i));
      w.publish(px.data(), 64, 64, i, 0);
    }
  });
  std::vector<uint8_t> out(kBytes);
  uint64_t ok = 0, bad = 0, overrun = 0, last = 0;
  for (int k = 0; k < 200000; ++k) {
    const uint64_t pub = r.published();
    if (pub == 0) continue;
    ShmSlotHeader m;
    const auto res = r.read(pub - 1, out.data(), &m);
    if (res == ShmRing::Read::Overrun || res == ShmRing::Read::Torn) { ++overrun; continue; }
    if (res != ShmRing::Read::Ok) continue;
    ++ok;
    for (uint32_t i = 1; i < kBytes; ++i)
      if (out[i] != out[0]) { ++bad; break; }
    EXPECT_GE(m.index, last);
    last = m.index;
  }
  stop = true;
  writer.join();
  EXPECT_GT(ok, 0u);
  EXPECT_EQ(bad, 0u) << "torn frame returned as Ok";
}
