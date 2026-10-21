#pragma once
// The station's real-time pipeline. Three of our threads plus Aravis's own:
//
//   [Aravis GVSP rx thread] --ArvStream buffers--> [acquisition / sequencer]
//        --BoundedQueue (drop-newest, counted)--> [processing]
//        --coalesced notify--> [GUI thread]      --shm ring--> [recorder process]
//
// The acquisition thread also runs the inspection sequence (move -> settle ->
// trigger -> acquire -> process) because those steps are sequential and all
// block on I/O; the processing thread stays free to measure frames.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "camera/aravis_camera.hpp"
#include "core/bounded_queue.hpp"
#include "core/edge_measure.hpp"
#include "core/latency.hpp"
#include "core/scene.hpp"
#include "core/station_fsm.hpp"
#include "pipeline/stage_client.hpp"

#ifdef __unix__
#include "core/shm_ring.hpp"
#endif

namespace vs {

struct RtConfig {
  bool enable = false;  // apply fifo and/or pin to the rx/acq/proc threads
  bool fifo = true, pin = true;
  int priority = 80;
  int cpu_rx = 1, cpu_acq = 2, cpu_proc = 3;
};

struct PipelineConfig {
  std::string camera_id = "127.0.0.1";
  std::string stage_host = "127.0.0.1";
  uint16_t stage_port = 5555;
  double exposure_us = 2000;  // short exposure: a moving stage blurs less
  size_t proc_queue = 8;
  StreamOptions stream;
  RtConfig rt;
  int proc_cost_us = 0;     // extra busy-work per frame, to force overload
  std::string shm_name;     // "" = no recorder ring
  uint32_t shm_slots = 64;
  size_t max_results = 200'000;  // live view runs for hours: keep the newest only
  PeakFit fit = PeakFit::Gaussian;
  EdgeRoi roi;
  SceneParams scene;        // optics + nominal part (for unit conversion and scoring)
};

enum class SettleMode { Controller, None, FixedDelay };

struct InspectionPlan {
  int cycles = 50;
  uint64_t seed = 7;
  double range_um = 1500;   // targets uniform in [-range, +range]
  SettleMode settle = SettleMode::Controller;
  int settle_delay_ms = 0;  // for FixedDelay
  int move_timeout_ms = 2000, settle_timeout_ms = 1000, frame_timeout_ms = 500;
  int frame_retries = 1;    // re-trigger once before declaring a timeout
  std::map<int, std::string> faults;  // cycle -> "stall" | "drop"
};

struct InspectionResult {
  uint64_t index = 0;        // processed-frame index
  int cycle = -1;            // -1 in free run
  bool has_truth = false, measured = false;
  TruthStamp truth{};
  EdgeResult edges{};
  double stage_target_um = 0;
  double left_err_px = 0, right_err_px = 0, width_err_um = 0;  // vs ground truth
  double pos_err_um = 0;     // part position via commanded stage pos, vs true
  uint64_t t_trigger_ns = 0, t_rx_ns = 0, t_done_ns = 0;
  int width = 0, height = 0;
  std::shared_ptr<const std::vector<uint8_t>> image;  // only on the latest result
};

struct Counters {
  std::atomic<uint64_t> rx_complete{0}, rx_incomplete{0}, seq_gap{0}, stale_discarded{0};
  std::atomic<uint64_t> queue_drops{0}, processed{0}, measure_fail{0}, ui_notify{0}, ui_coalesced{0};
  std::atomic<uint64_t> shm_published{0}, pool_alloc{0};
  std::atomic<uint64_t> cycles_ok{0}, cycles_failed{0}, recoveries{0}, retriggers{0};
};

struct Snapshot {
  State state = State::Idle;
  bool running = false;
  double fps = 0;
  Percentiles e2e_ms, transport_ms, proc_ms, trigger_ms;  // recent window
  uint64_t rx_complete = 0, rx_incomplete = 0, seq_gap = 0, queue_drops = 0, processed = 0;
  uint64_t cycles_ok = 0, cycles_failed = 0, recoveries = 0;
  StreamStats stream;
  double stage_target_um = 0;
  std::string last_error;
};

class Pipeline {
 public:
  explicit Pipeline(PipelineConfig cfg);
  ~Pipeline();

  bool connect();  // camera + stage
  const std::string& error() const { return error_; }
  AravisCamera& camera() { return cam_; }

  bool start_freerun(double fps);
  bool start_inspection(const InspectionPlan& plan);
  void stop();
  bool running() const { return running_; }
  bool finished() const { return finished_; }  // inspection plan completed

  // Queued to the acquisition thread, which owns the stage connection; only
  // honoured in free run (live view), never in the middle of an inspection.
  void jog(double delta_um);
  void reset_error();         // operator Reset from the GUI

  Snapshot snapshot() const;
  std::shared_ptr<const InspectionResult> latest() const;
  std::vector<InspectionResult> results() const;  // without images
  const Counters& counters() const { return c_; }
  // Full-run latency samples in ms.
  const LatencyRecorder& lat_e2e() const { return lat_e2e_; }
  const LatencyRecorder& lat_transport() const { return lat_transport_; }
  const LatencyRecorder& lat_proc() const { return lat_proc_; }
  const LatencyRecorder& lat_trigger() const { return lat_trigger_; }
  const LatencyRecorder& lat_cycle() const { return lat_cycle_; }
  // Transport split: readout (rig render) / wire (rig send -> GVSP leader on
  // the Aravis thread) / assembly (leader -> our pop, incl. all payload packets).
  const LatencyRecorder& lat_readout() const { return lat_readout_; }
  const LatencyRecorder& lat_wire() const { return lat_wire_; }
  const LatencyRecorder& lat_assembly() const { return lat_assembly_; }
  std::string rt_report() const;

  // Invoked on the processing thread at most once per GUI consumption:
  // the GUI calls ack_result() when it has taken latest().
  void set_on_result(std::function<void()> fn) { on_result_ = std::move(fn); }
  void ack_result() { ui_pending_.store(false, std::memory_order_release); }

 private:
  struct Job {
    std::unique_ptr<RawFrame> frame;
    int cycle = -1;
    double stage_target_um = 0;
    uint64_t t_trigger_ns = 0;
  };

  bool start_common();
  void acq_freerun_loop();
  void acq_inspection_loop(InspectionPlan plan);
  void proc_loop();
  bool hand_off(Job job);  // to the processing queue; false = dropped
  void note_seq(const RawFrame& f);
  bool recover(std::string why);
  void set_state(Event e);
  void drain_jogs();
  std::unique_ptr<RawFrame> get_frame();
  void apply_rt(const char* who, int cpu);

  PipelineConfig cfg_;
  AravisCamera cam_;
  StageClient stage_;
  StationFsm fsm_;
  mutable std::mutex fsm_mu_;
  std::atomic<State> state_{State::Idle};

  BoundedQueue<Job> q_proc_;
  BoundedQueue<std::unique_ptr<RawFrame>> pool_;
  std::thread acq_, proc_;
  std::atomic<bool> running_{false}, stop_{false}, finished_{false};

  mutable std::mutex res_mu_;
  std::vector<InspectionResult> results_;
  std::shared_ptr<const InspectionResult> latest_;
  std::function<void()> on_result_;
  std::atomic<bool> ui_pending_{false};

  // Cycle hand-shake: the sequencer waits for its frame's result.
  std::mutex cyc_mu_;
  std::condition_variable cyc_cv_;
  int cycle_done_ = -1;

  std::mutex jog_mu_;
  std::vector<double> jogs_;
  std::atomic<double> stage_target_{0};

  uint64_t last_seq_ = 0;
  Counters c_;
  LatencyRecorder lat_e2e_, lat_transport_, lat_proc_, lat_trigger_, lat_cycle_;
  LatencyRecorder lat_readout_, lat_wire_, lat_assembly_;
  mutable std::mutex rt_mu_;
  std::vector<std::string> rt_notes_;
  std::string error_;
  mutable std::mutex err_mu_;
  std::string last_error_;
  uint64_t t_start_ns_ = 0;
#ifdef __unix__
  std::unique_ptr<ShmRing> shm_;
#endif
};

}  // namespace vs
