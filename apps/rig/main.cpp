// vs_rig: the simulated hardware rig.
//
//  * A GigE Vision camera: Aravis's ArvGvFakeCamera, which speaks real GVCP
//    (UDP 3956, control) and GVSP (UDP, streaming). Its fill-pattern hook is
//    replaced so each frame shows the part where the stage currently puts it,
//    blurred by any stage motion during the exposure, with the ground truth
//    stamped into row 0.
//  * A stage controller on TCP (default 5555) speaking the protocol in
//    src/core/stage_protocol.hpp.
//
// Both live in one process because the image depends on the stage position.
// The station is a separate process and only reaches them over the network.
#include <arv.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/latency.hpp"
#include "core/net.hpp"
#include "core/scene.hpp"
#include "core/stage_model.hpp"
#include "core/stage_protocol.hpp"

#if ARAVIS_MAJOR_VERSION != 0 || ARAVIS_MINOR_VERSION != 8
#error "apps/rig mirrors the Aravis 0.8 ArvBuffer layout; re-check arvbufferprivate.h"
#endif

namespace {

// --- The one hack -------------------------------------------------------------
// The fake camera's fill-pattern callback must set buffer->priv->received_size,
// or the camera sends a zero-length payload; there is no public setter. This
// mirrors the head of ArvBufferPrivate from Aravis 0.8.30 (arvbufferprivate.h)
// and is verified at startup by layout_ok() against public getters.
struct MirrorPriv {
  size_t allocated_size;
  gboolean is_preallocated;
  unsigned char* data;
  void* user_data;
  GDestroyNotify user_data_destroy_func;
  ArvBufferStatus status;
  size_t received_size;
  ArvBufferPayloadType payload_type;
  gboolean has_chunks;
  guint32 chunk_endianness;
  guint64 frame_id;
};
struct MirrorBuffer {
  GObject object;
  MirrorPriv* priv;
};
MirrorPriv* priv_of(ArvBuffer* b) { return reinterpret_cast<MirrorBuffer*>(b)->priv; }

bool layout_ok() {
  ArvBuffer* b = arv_buffer_new(4096, nullptr);
  arv_buffer_set_frame_id(b, 0x1234567890ull);
  size_t n = 1;
  const void* d = arv_buffer_get_data(b, &n);
  MirrorPriv* p = priv_of(b);
  const bool ok = p->allocated_size == 4096 && p->data == d && p->received_size == n &&
                  p->frame_id == 0x1234567890ull;
  g_object_unref(b);
  return ok;
}
// -----------------------------------------------------------------------------

std::atomic<bool> g_stop{false};

struct Rig {
  vs::SceneParams scene;
  vs::StageModel stage;
  std::mutex stage_mu;
  std::atomic<uint64_t> seq{0};
  std::atomic<uint64_t> render_ns{0}, last_fill_ns{0}, gap_ns{0};
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  double now_s() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
};

void fill(ArvBuffer* buffer, void* data, guint32 exposure_us, guint32 /*gain*/, ArvPixelFormat fmt) {
  auto* rig = static_cast<Rig*>(data);
  MirrorPriv* p = priv_of(buffer);
  const int w = static_cast<int>(arv_buffer_get_image_width(buffer));
  const int h = static_cast<int>(arv_buffer_get_image_height(buffer));
  const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (fmt != ARV_PIXEL_FORMAT_MONO_8 || bytes > p->allocated_size) {
    std::memset(p->data, 128, p->allocated_size);
    p->received_size = std::min(p->allocated_size, bytes * ARV_PIXEL_FORMAT_BIT_PER_PIXEL(fmt) / 8);
    return;
  }

  // Causal exposure: it starts now (the fake camera calls us when a trigger
  // is seen, or on a free-run slot), lasts exposure_us, and the stage is
  // sampled across that window, so ringing shows up as blur and as a
  // position error. The frame cannot leave before the exposure ends.
  const uint64_t start_ns = vs::now_ns();
  const double t = rig->now_s();
  const double exp_s = exposure_us * 1e-6;
  std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(start_ns)) +
                                std::chrono::microseconds(exposure_us));
  const uint64_t fill_ns = vs::now_ns();
  constexpr int kSamples = 8;
  double samples[kSamples];
  double mean = 0;
  bool inpos;
  {
    std::lock_guard lk(rig->stage_mu);
    for (int i = 0; i < kSamples; ++i) {
      samples[i] = rig->stage.position(t + exp_s * (i + 0.5) / kSamples);
      mean += samples[i] / kSamples;
    }
    inpos = rig->stage.in_position_over(t, t + exp_s);
  }
  vs::SceneParams sp = rig->scene;
  sp.width = w;
  sp.height = h;
  const uint64_t seq = rig->seq.fetch_add(1) + 1;
  vs::render_part(sp, samples, seq, p->data);

  vs::TruthStamp st;
  st.seq = seq;
  st.exposure_start_ns = start_ns;
  st.fill_ns = fill_ns;
  st.stage_x_nm = static_cast<int64_t>(mean * 1000.0);
  const vs::EdgeTruth e = vs::edges_at(sp, mean);
  st.left_px_e6 = static_cast<int64_t>(e.left_px * 1e6);
  st.right_px_e6 = static_cast<int64_t>(e.right_px * 1e6);
  st.exposure_us = exposure_us;
  st.stage_in_position = inpos ? 1 : 0;
  st.readout_done_ns = vs::now_ns();
  vs::write_stamp(st, p->data, static_cast<size_t>(w));
  p->received_size = bytes;
  const uint64_t done = vs::now_ns();
  rig->render_ns += done - fill_ns;  // readout/render only, exposure excluded
  const uint64_t prev = rig->last_fill_ns.exchange(fill_ns);
  if (prev) rig->gap_ns += fill_ns - prev;
}

const char* phase_name(vs::StagePhase ph) {
  switch (ph) {
    case vs::StagePhase::Idle: return "IDLE";
    case vs::StagePhase::Moving: return "MOVING";
    case vs::StagePhase::Settling: return "SETTLING";
    case vs::StagePhase::InPosition: return "INPOS";
    case vs::StagePhase::Fault: return "FAULT";
  }
  return "?";
}

// Blocks until `when_s` (rig time) or the timeout, re-reading `when_s` so a
// RESET from another connection is seen. Returns true if `when_s` was reached.
template <class WhenFn>
bool wait_until(Rig& rig, WhenFn when_s, double timeout_ms) {
  const double deadline = rig.now_s() + timeout_ms / 1000.0;
  for (;;) {
    double when;
    {
      std::lock_guard lk(rig.stage_mu);
      when = when_s();
    }
    const double now = rig.now_s();
    if (now >= when) return true;
    if (now >= deadline || g_stop) return false;
    const double next = std::min({when, deadline, now + 0.050});
    std::this_thread::sleep_for(std::chrono::duration<double>(next - now));
  }
}

void serve_client(Rig& rig, vs::net::socket_t c) {
  std::string buf, line;
  while (!g_stop) {
    bool timed_out = false;
    if (!vs::net::read_line(c, buf, line, 200, &timed_out)) {
      if (timed_out) continue;
      break;
    }
    auto req = vs::parse_request(line);
    vs::StageReply rep{vs::ReplyKind::Err, 0, "bad_request"};
    if (req) {
      switch (req->cmd) {
        case vs::StageCmd::Move: {
          std::lock_guard lk(rig.stage_mu);
          rep = rig.stage.move(req->value, rig.now_s()) ? vs::StageReply{vs::ReplyKind::Ok, 0, {}}
                                                        : vs::StageReply{vs::ReplyKind::Err, 0, "rejected"};
          break;
        }
        case vs::StageCmd::Arrive:
          rep.kind = wait_until(rig, [&] { return rig.stage.arrive_time(); }, req->value)
                         ? vs::ReplyKind::Arrived : vs::ReplyKind::Timeout;
          break;
        case vs::StageCmd::Settle:
          if (wait_until(rig, [&] { return rig.stage.settle_time(); }, req->value)) {
            std::lock_guard lk(rig.stage_mu);
            rep = {vs::ReplyKind::Done, rig.stage.position(rig.now_s()), {}};
          } else {
            rep.kind = vs::ReplyKind::Timeout;
          }
          break;
        case vs::StageCmd::Pos: {
          std::lock_guard lk(rig.stage_mu);
          const double t = rig.now_s();
          rep = {vs::ReplyKind::Pos, rig.stage.position(t), phase_name(rig.stage.phase(t))};
          break;
        }
        case vs::StageCmd::Reset: {
          std::lock_guard lk(rig.stage_mu);
          rig.stage.reset(rig.now_s());
          rep = {vs::ReplyKind::Ok, 0, {}};
          break;
        }
        case vs::StageCmd::Fault:
          if (req->arg == "drop") {
            std::fprintf(stderr, "[rig] FAULT drop: closing stage connection\n");
            vs::net::close(c);
            return;
          }
          {
            std::lock_guard lk(rig.stage_mu);
            rig.stage.inject_stall();
          }
          rep = {vs::ReplyKind::Ok, 0, {}};
          break;
      }
    }
    if (!vs::net::send_all(c, vs::format_reply(rep))) break;
  }
  vs::net::close(c);
}

}  // namespace

int main(int argc, char** argv) {
  std::string addr = "127.0.0.1";
  int port = 5555;
  double trigger_hz = 1000;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--addr") addr = next();
    else if (a == "--port") port = std::stoi(next());
    else if (a == "--trigger-hz") trigger_hz = std::stod(next());
    else {
      std::fprintf(stderr, "usage: vs_rig [--addr 127.0.0.1] [--port 5555] [--trigger-hz 1000]\n");
      return 2;
    }
  }
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });

  if (!layout_ok()) {
    std::fprintf(stderr, "[rig] ArvBuffer layout check failed: this Aravis build differs from 0.8.30\n");
    return 1;
  }
  Rig rig;
  ArvGvFakeCamera* cam = arv_gv_fake_camera_new(addr.c_str(), "VS-RIG-0001");
  if (!cam || !arv_gv_fake_camera_is_running(cam)) {
    std::fprintf(stderr, "[rig] fake GigE camera failed to start on %s\n", addr.c_str());
    return 1;
  }
  ArvFakeCamera* fake = arv_gv_fake_camera_get_fake_camera(cam);
  // In trigger mode the fake camera only checks for a pending software trigger
  // once per 1/trigger_hz, on slots aligned to the wall clock. This rate, not
  // the network, dominates its trigger-to-frame latency.
  arv_fake_camera_set_trigger_frequency(fake, trigger_hz);
  arv_fake_camera_set_fill_pattern(fake, fill, &rig);
  // Start at the 640x480 the scene is designed for.
  arv_fake_camera_write_register(fake, ARV_FAKE_CAMERA_REGISTER_WIDTH, 640);
  arv_fake_camera_write_register(fake, ARV_FAKE_CAMERA_REGISTER_HEIGHT, 480);

  vs::net::init();
  vs::net::socket_t l = vs::net::listen_tcp(static_cast<uint16_t>(port));
  if (l == vs::net::kInvalid) {
    std::fprintf(stderr, "[rig] cannot listen on 127.0.0.1:%d\n", port);
    return 1;
  }
  std::fprintf(stderr, "[rig] GigE Vision camera on %s (GVCP 3956), stage on 127.0.0.1:%d, trigger poll %.0f Hz\n",
               addr.c_str(), port, trigger_hz);

  std::vector<std::thread> clients;
  while (!g_stop) {
    vs::net::socket_t c = vs::net::accept_one(l, 200);
    if (c != vs::net::kInvalid) clients.emplace_back(serve_client, std::ref(rig), c);
  }
  for (auto& t : clients) t.join();
  vs::net::close(l);
  const uint64_t n = rig.seq.load();
  std::fprintf(stderr, "[rig] rendered %llu frames, mean render %.3f ms, mean interval %.3f ms\n",
               static_cast<unsigned long long>(n), n ? rig.render_ns.load() / 1e6 / n : 0.0,
               n > 1 ? rig.gap_ns.load() / 1e6 / (n - 1) : 0.0);
  g_object_unref(cam);
  return 0;
}
