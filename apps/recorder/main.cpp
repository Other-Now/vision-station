// vs_recorder: a second process that consumes frames from the station's POSIX
// shared-memory ring. The station never waits for it; if the recorder falls
// behind, slots are overwritten and the recorder counts exactly how many
// frames it missed.
//
//   vs_recorder --shm /vs_frames --seconds 10 [--delay-us N] [--pgm-every N --out DIR] [--json f]
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/latency.hpp"
#include "core/scene.hpp"
#include "core/shm_ring.hpp"

using namespace vs;

int main(int argc, char** argv) {
  std::string name = "/vs_frames", out_dir, json;
  double seconds = 10;
  int delay_us = 0, pgm_every = 0;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto v = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (k == "--shm") name = v();
    else if (k == "--seconds") seconds = std::stod(v());
    else if (k == "--delay-us") delay_us = std::stoi(v());
    else if (k == "--pgm-every") pgm_every = std::stoi(v());
    else if (k == "--out") out_dir = v();
    else if (k == "--json") json = v();
    else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
  }

  ShmRing ring;
  const auto t_open = std::chrono::steady_clock::now();
  while (!ring.open(name)) {
    if (std::chrono::steady_clock::now() - t_open > std::chrono::seconds(10)) {
      std::fprintf(stderr, "recorder: %s: %s\n", name.c_str(), ring.error().c_str());
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::vector<uint8_t> buf(ring.slot_bytes());
  uint64_t next = ring.published();  // start at the live edge
  uint64_t read_ok = 0, overrun_frames = 0, torn = 0, seq_gaps = 0, last_seq = 0, written = 0;
  uint64_t checksum = 0;
  LatencyRecorder lat;
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);

  while (std::chrono::steady_clock::now() < end) {
    ShmSlotHeader meta;
    const auto r = ring.read(next, buf.data(), &meta);
    if (r == ShmRing::Read::NotYet) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    if (r == ShmRing::Read::Torn) { ++torn; continue; }
    if (r == ShmRing::Read::Overrun) {
      // Writer lapped us: jump to the oldest slot that is still intact.
      const uint64_t pub = ring.published();
      const uint64_t oldest = pub > ring.slot_count() ? pub - ring.slot_count() + 1 : 0;
      overrun_frames += oldest > next ? oldest - next : 1;
      next = std::max(oldest, next + 1);
      continue;
    }
    lat.add(static_cast<double>(now_ns() - meta.publish_ns) / 1e6);
    TruthStamp st;
    if (read_stamp(buf.data(), meta.width, &st)) {
      if (last_seq && st.seq > last_seq + 1) seq_gaps += st.seq - last_seq - 1;
      last_seq = st.seq;
    }
    for (uint32_t i = 0; i < meta.bytes; i += 64) checksum = checksum * 31 + buf[i];
    if (pgm_every > 0 && !out_dir.empty() && read_ok % static_cast<uint64_t>(pgm_every) == 0) {
      std::ofstream f(out_dir + "/frame_" + std::to_string(st.seq) + ".pgm", std::ios::binary);
      f << "P5\n" << meta.width << " " << meta.height << "\n255\n";
      f.write(reinterpret_cast<const char*>(buf.data()), meta.bytes);
      ++written;
    }
    ++read_ok;
    ++next;
    if (delay_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
  }

  const Percentiles p = lat.compute();
  std::printf("recorder: read %llu frames, overrun-skipped %llu, torn retries %llu, upstream seq gaps %llu, "
              "pgm written %llu\n  publish->read p50 %.3f ms p99 %.3f ms max %.3f ms\n",
              (unsigned long long)read_ok, (unsigned long long)overrun_frames, (unsigned long long)torn,
              (unsigned long long)seq_gaps, (unsigned long long)written, p.p50, p.p99, p.max);
  if (!json.empty()) {
    std::ostringstream j;
    j << "{\"read\":" << read_ok << ",\"overrun_skipped\":" << overrun_frames << ",\"torn\":" << torn
      << ",\"seq_gaps\":" << seq_gaps << ",\"delay_us\":" << delay_us << ",\"lat_p50_ms\":" << p.p50
      << ",\"lat_p99_ms\":" << p.p99 << ",\"lat_max_ms\":" << p.max << "}";
    std::ofstream(json) << j.str() << "\n";
  }
  return 0;
}
