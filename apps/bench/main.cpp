// vs_bench: headless station runs that produce the numbers in results/.
//
//   vs_bench features
//   vs_bench freerun --fps 200 --seconds 10 [--no-socket-auto] [--queue 8]
//                    [--proc-cost-us N] [--rt] [--stress N] [--shm /vs_frames]
//   vs_bench inspect --cycles 50 [--settle controller|none|fixed:MS]
//                    [--fault CYCLE:stall|drop] [--fit gaussian|parabolic|integer] [--rt]
//   common: --camera 127.0.0.1 --stage-port 5555 --json out.json
//
// Exit code 1 if frame accounting does not balance or an invariant fails.
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/rt.hpp"
#include "pipeline/pipeline.hpp"

using namespace vs;

namespace {

struct Args {
  std::string mode;
  std::string camera = "127.0.0.1";
  int stage_port = 5555;
  double fps = 100, seconds = 5;
  bool socket_auto = true, rt = false, fifo = true, pin = true;
  size_t queue = 8;
  int proc_cost_us = 0, stress = 0, cycles = 50, buffers = 16;
  std::string shm, json, settle = "controller", fit = "gaussian";
  std::map<int, std::string> faults;
};

std::string pjson(const Percentiles& p) {
  char b[256];
  std::snprintf(b, sizeof b, "{\"n\":%zu,\"p50\":%.4f,\"p90\":%.4f,\"p99\":%.4f,\"p999\":%.4f,\"max\":%.4f,\"mean\":%.4f}",
                p.n, p.p50, p.p90, p.p99, p.p999, p.max, p.mean);
  return b;
}

void print_p(const char* name, const Percentiles& p) {
  std::printf("  %-22s n=%-6zu p50=%8.3f  p99=%8.3f  p99.9=%8.3f  max=%8.3f ms\n", name, p.n, p.p50, p.p99,
              p.p999, p.max);
}

struct ErrStats {
  size_t n = 0;
  double mean = 0, sd = 0, max_abs = 0, rms = 0;
};
ErrStats err_stats(const std::vector<double>& v) {
  ErrStats s;
  s.n = v.size();
  if (v.empty()) return s;
  double sum = 0, sq = 0;
  for (double x : v) { sum += x; sq += x * x; s.max_abs = std::max(s.max_abs, std::abs(x)); }
  s.mean = sum / static_cast<double>(v.size());
  s.rms = std::sqrt(sq / static_cast<double>(v.size()));
  s.sd = std::sqrt(std::max(0.0, sq / static_cast<double>(v.size()) - s.mean * s.mean));
  return s;
}
std::string ejson(const ErrStats& e) {
  char b[200];
  std::snprintf(b, sizeof b, "{\"n\":%zu,\"mean\":%.5f,\"sd\":%.5f,\"rms\":%.5f,\"max_abs\":%.5f}", e.n, e.mean, e.sd,
                e.rms, e.max_abs);
  return b;
}

std::vector<std::thread> start_stress(int n, std::atomic<bool>& stop) {
  std::vector<std::thread> t;
  for (int i = 0; i < n; ++i)
    t.emplace_back([&stop] {
      volatile uint64_t x = 0;
      while (!stop.load(std::memory_order_relaxed)) x = x + 1;
    });
  return t;
}

PipelineConfig make_cfg(const Args& a) {
  PipelineConfig c;
  c.camera_id = a.camera;
  c.stage_port = static_cast<uint16_t>(a.stage_port);
  c.proc_queue = a.queue;
  c.stream.socket_buffer_auto = a.socket_auto;
  c.stream.n_buffers = a.buffers;
  c.rt.enable = a.rt;
  c.rt.fifo = a.fifo;
  c.rt.pin = a.pin;
  c.proc_cost_us = a.proc_cost_us;
  c.shm_name = a.shm;
  c.fit = a.fit == "parabolic" ? PeakFit::Parabolic : a.fit == "integer" ? PeakFit::Integer : PeakFit::Gaussian;
  return c;
}

long read_sysctl(const char* path) {
  long v = -1;
  std::ifstream(path) >> v;
  return v;
}

void write_json(const std::string& path, const std::string& body) {
  if (path.empty()) return;
  std::ofstream(path) << body << "\n";
  std::printf("wrote %s\n", path.c_str());
}

int cmd_features(const Args& a) {
  AravisCamera cam;
  if (!cam.open(a.camera)) { std::fprintf(stderr, "%s\n", cam.error().c_str()); return 1; }
  std::printf("Camera model: %s\n\n| wanted (SFNC) | node on this device | type | value |\n|---|---|---|---|\n",
              cam.model().c_str());
  for (const auto& f : cam.feature_report())
    std::printf("| %s | %s | %s | %s |\n", f.wanted.c_str(), f.resolved.empty() ? "**missing**" : f.resolved.c_str(),
                f.type.c_str(), f.value.c_str());
  return 0;
}

int cmd_freerun(const Args& a) {
  Pipeline p(make_cfg(a));
  std::atomic<bool> stress_stop{false};
  auto stress = start_stress(a.stress, stress_stop);
  if (!p.start_freerun(a.fps)) { std::fprintf(stderr, "start: %s\n", p.error().c_str()); return 1; }
  std::this_thread::sleep_for(std::chrono::duration<double>(a.seconds));
  p.stop();
  stress_stop = true;
  for (auto& t : stress) t.join();

  const Counters& c = p.counters();
  const StreamStats ss = p.camera().stream_stats();
  const auto e2e = p.lat_e2e().compute(), tr = p.lat_transport().compute(), pr = p.lat_proc().compute();
  const double fps = static_cast<double>(c.processed) / a.seconds;
  const uint64_t in = c.rx_complete, out = c.processed + c.queue_drops;
  const bool balanced = in == out;

  std::printf("freerun %.0f fps x %.1fs  socket_auto=%d queue=%zu proc_cost=%dus rt=%d stress=%d\n", a.fps, a.seconds,
              a.socket_auto, a.queue, a.proc_cost_us, a.rt, a.stress);
  std::printf("  processed %.1f fps | rx complete %llu, incomplete %llu, seq gaps %llu | queue drops %llu | "
              "missing pkts %llu, underruns %llu\n",
              fps, (unsigned long long)c.rx_complete.load(), (unsigned long long)c.rx_incomplete.load(),
              (unsigned long long)c.seq_gap.load(), (unsigned long long)c.queue_drops.load(),
              (unsigned long long)ss.missing_packets, (unsigned long long)ss.underruns);
  print_p("exposure end->processed", e2e);
  print_p("exposure end->host rx", tr);
  print_p("rx->processed", pr);
  const auto ro = p.lat_readout().compute(), wi = p.lat_wire().compute(), as = p.lat_assembly().compute();
  print_p("  rig readout/render", ro);
  print_p("  send->GVSP leader", wi);
  print_p("  leader->pop", as);
  std::printf("  accounting: rx_complete %llu == processed + queue_drops %llu : %s\n", (unsigned long long)in,
              (unsigned long long)out, balanced ? "OK" : "MISMATCH");
  if (a.rt) std::printf("%s", p.rt_report().c_str());

  std::ostringstream j;
  j << "{\"mode\":\"freerun\",\"fps_target\":" << a.fps << ",\"seconds\":" << a.seconds
    << ",\"socket_auto\":" << a.socket_auto << ",\"queue\":" << a.queue << ",\"proc_cost_us\":" << a.proc_cost_us
    << ",\"rt\":" << a.rt << ",\"stress\":" << a.stress << ",\"fps\":" << fps
    << ",\"rx_complete\":" << c.rx_complete << ",\"rx_incomplete\":" << c.rx_incomplete << ",\"seq_gap\":" << c.seq_gap
    << ",\"queue_drops\":" << c.queue_drops << ",\"processed\":" << c.processed
    << ",\"missing_packets\":" << ss.missing_packets << ",\"underruns\":" << ss.underruns
    << ",\"shm_published\":" << c.shm_published << ",\"pool_alloc\":" << c.pool_alloc
    << ",\"balanced\":" << balanced << ",\"lat_e2e_ms\":" << pjson(e2e) << ",\"lat_transport_ms\":" << pjson(tr)
    << ",\"lat_proc_ms\":" << pjson(pr) << ",\"lat_readout_ms\":" << pjson(ro) << ",\"lat_wire_ms\":" << pjson(wi)
    << ",\"lat_assembly_ms\":" << pjson(as) << ",\"fifo\":" << (a.rt && a.fifo) << ",\"pin\":" << (a.rt && a.pin)
    << ",\"rmem_default\":" << read_sysctl("/proc/sys/net/core/rmem_default") << "}";
  write_json(a.json, j.str());
  return balanced ? 0 : 1;
}

int cmd_inspect(const Args& a) {
  Pipeline p(make_cfg(a));
  InspectionPlan plan;
  plan.cycles = a.cycles;
  plan.faults = a.faults;
  if (a.settle == "none") plan.settle = SettleMode::None;
  else if (a.settle.rfind("fixed:", 0) == 0) {
    plan.settle = SettleMode::FixedDelay;
    plan.settle_delay_ms = std::stoi(a.settle.substr(6));
  }
  std::atomic<bool> stress_stop{false};
  auto stress = start_stress(a.stress, stress_stop);
  if (!p.start_inspection(plan)) { std::fprintf(stderr, "start: %s\n", p.error().c_str()); return 1; }
  while (!p.finished()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  p.stop();
  stress_stop = true;
  for (auto& t : stress) t.join();

  const Counters& c = p.counters();
  std::vector<double> edge_err, width_err, pos_err;
  size_t not_inpos = 0;
  for (const auto& r : p.results()) {
    if (!r.measured || !r.has_truth || r.cycle < 0) continue;
    edge_err.push_back(r.left_err_px);
    edge_err.push_back(r.right_err_px);
    width_err.push_back(r.width_err_um);
    pos_err.push_back(r.pos_err_um);
    not_inpos += r.truth.stage_in_position ? 0 : 1;
  }
  const ErrStats ee = err_stats(edge_err), we = err_stats(width_err), pe = err_stats(pos_err);
  const auto cyc = p.lat_cycle().compute(), trig = p.lat_trigger().compute(), e2e = p.lat_e2e().compute();
  const uint64_t attempted = c.cycles_ok + c.cycles_failed;
  const bool ok = attempted == static_cast<uint64_t>(a.cycles) && c.recoveries == c.cycles_failed &&
                  c.measure_fail == 0;

  std::printf("inspect %d cycles  settle=%s fit=%s faults=%zu rt=%d\n", a.cycles, a.settle.c_str(), a.fit.c_str(),
              a.faults.size(), a.rt);
  std::printf("  cycles ok %llu, failed %llu, recoveries %llu, retriggers %llu, stale frames %llu\n",
              (unsigned long long)c.cycles_ok.load(), (unsigned long long)c.cycles_failed.load(),
              (unsigned long long)c.recoveries.load(), (unsigned long long)c.retriggers.load(),
              (unsigned long long)c.stale_discarded.load());
  std::printf("  edge error vs truth   : mean %+.4f px  sd %.4f  max|e| %.4f px  (n=%zu)\n", ee.mean, ee.sd, ee.max_abs, ee.n);
  std::printf("  width error vs truth  : mean %+.3f um  sd %.3f  max|e| %.3f um\n", we.mean, we.sd, we.max_abs);
  std::printf("  part position error   : mean %+.3f um  sd %.3f  max|e| %.3f um  (frames exposed while not in position: %zu)\n",
              pe.mean, pe.sd, pe.max_abs, not_inpos);
  print_p("cycle time", cyc);
  print_p("trigger->exposure start", trig);
  print_p("exposure end->processed", e2e);
  if (a.rt) std::printf("%s", p.rt_report().c_str());

  std::ostringstream j;
  j << "{\"mode\":\"inspect\",\"cycles\":" << a.cycles << ",\"settle\":\"" << a.settle << "\",\"fit\":\"" << a.fit
    << "\",\"faults\":" << a.faults.size() << ",\"rt\":" << a.rt << ",\"cycles_ok\":" << c.cycles_ok
    << ",\"cycles_failed\":" << c.cycles_failed << ",\"recoveries\":" << c.recoveries
    << ",\"retriggers\":" << c.retriggers << ",\"stale_discarded\":" << c.stale_discarded
    << ",\"not_in_position\":" << not_inpos << ",\"edge_err_px\":" << ejson(ee) << ",\"width_err_um\":" << ejson(we)
    << ",\"pos_err_um\":" << ejson(pe) << ",\"lat_cycle_ms\":" << pjson(cyc) << ",\"lat_trigger_ms\":" << pjson(trig)
    << ",\"lat_e2e_ms\":" << pjson(e2e) << ",\"ok\":" << ok << "}";
  write_json(a.json, j.str());
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (argc < 2) {
    std::fprintf(stderr, "usage: vs_bench features|freerun|inspect [options]  (see source header)\n");
    return 2;
  }
  a.mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string k = argv[i];
    auto v = [&]() -> std::string {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); std::exit(2); }
      return argv[++i];
    };
    if (k == "--camera") a.camera = v();
    else if (k == "--stage-port") a.stage_port = std::stoi(v());
    else if (k == "--fps") a.fps = std::stod(v());
    else if (k == "--seconds") a.seconds = std::stod(v());
    else if (k == "--no-socket-auto") a.socket_auto = false;
    else if (k == "--queue") a.queue = std::stoul(v());
    else if (k == "--buffers") a.buffers = std::stoi(v());
    else if (k == "--proc-cost-us") a.proc_cost_us = std::stoi(v());
    else if (k == "--rt") a.rt = true;
    else if (k == "--fifo-only") { a.rt = true; a.pin = false; }
    else if (k == "--pin-only") { a.rt = true; a.fifo = false; }
    else if (k == "--stress") a.stress = std::stoi(v());
    else if (k == "--shm") a.shm = v();
    else if (k == "--json") a.json = v();
    else if (k == "--cycles") a.cycles = std::stoi(v());
    else if (k == "--settle") a.settle = v();
    else if (k == "--fit") a.fit = v();
    else if (k == "--fault") {
      std::string f = v();
      auto c = f.find(':');
      if (c == std::string::npos) { std::fprintf(stderr, "--fault CYCLE:stall|drop\n"); return 2; }
      a.faults[std::stoi(f.substr(0, c))] = f.substr(c + 1);
    } else {
      std::fprintf(stderr, "unknown option %s\n", k.c_str());
      return 2;
    }
  }
  if (a.mode == "features") return cmd_features(a);
  if (a.mode == "freerun") return cmd_freerun(a);
  if (a.mode == "inspect") return cmd_inspect(a);
  std::fprintf(stderr, "unknown mode %s\n", a.mode.c_str());
  return 2;
}
