#include "pipeline/pipeline.hpp"

#include <cmath>
#include <cstdio>
#include <random>

#include "core/rt.hpp"

namespace vs {
namespace {
double ms(uint64_t a, uint64_t b) { return b > a ? static_cast<double>(b - a) / 1e6 : 0.0; }

void busy_us(int us) {
  const uint64_t end = now_ns() + static_cast<uint64_t>(us) * 1000;
  while (now_ns() < end) {
  }
}
}  // namespace

Pipeline::Pipeline(PipelineConfig cfg)
    : cfg_(std::move(cfg)),
      q_proc_(cfg_.proc_queue, DropPolicy::DropNewest),
      pool_(cfg_.proc_queue + 4, DropPolicy::DropNewest) {
  for (size_t i = 0; i < cfg_.proc_queue + 4; ++i) pool_.try_push(std::make_unique<RawFrame>());
}

Pipeline::~Pipeline() { stop(); }

bool Pipeline::connect() {
  if (!cam_.is_open()) {
    if (!cam_.open(cfg_.camera_id) || !cam_.set_exposure_us(cfg_.exposure_us)) {
      error_ = "camera: " + cam_.error();
      return false;
    }
  }
  if (!stage_.connected() && !stage_.connect(cfg_.stage_host, cfg_.stage_port)) {
    error_ = "stage: " + stage_.last_error();
    return false;
  }
  return true;
}

void Pipeline::apply_rt(const char* who, int cpu) {
  if (!cfg_.rt.enable) return;
  auto a = cfg_.rt.fifo ? rt::set_fifo(cfg_.rt.priority) : rt::Result{true, "no fifo"};
  auto b = cfg_.rt.pin ? rt::pin_to_cpu(cpu) : rt::Result{true, "no pin"};
  std::lock_guard lk(rt_mu_);
  rt_notes_.push_back(std::string(who) + ": " + a.detail + ", " + b.detail + " -> " + rt::describe_self());
}

std::string Pipeline::rt_report() const {
  std::lock_guard lk(rt_mu_);
  std::string s;
  for (const auto& n : rt_notes_) s += n + "\n";
  return s;
}

void Pipeline::set_state(Event e) {
  std::lock_guard lk(fsm_mu_);
  fsm_.fire(e);
  state_ = fsm_.state();
}

std::unique_ptr<RawFrame> Pipeline::get_frame() {
  if (auto f = pool_.try_pop()) return std::move(*f);
  c_.pool_alloc++;
  return std::make_unique<RawFrame>();
}

bool Pipeline::start_common() {
  if (running_) return false;
  if (!connect()) return false;
  stop_ = false;
  finished_ = false;
  last_seq_ = 0;
  t_start_ns_ = now_ns();
#ifdef __unix__
  if (!cfg_.shm_name.empty() && !shm_) {
    shm_ = std::make_unique<ShmRing>();
    if (!shm_->create(cfg_.shm_name, cfg_.shm_slots, 640 * 480)) {
      error_ = "shm: " + shm_->error();
      shm_.reset();
      return false;
    }
  }
#endif
  StreamOptions so = cfg_.stream;
  so.on_stream_thread_start = [this] { apply_rt("gvsp-rx", cfg_.rt.cpu_rx); };
  if (!cam_.start(so)) {
    error_ = "stream: " + cam_.error();
    return false;
  }
  running_ = true;
  proc_ = std::thread([this] { proc_loop(); });
  return true;
}

bool Pipeline::start_freerun(double fps) {
  if (!connect()) return false;
  if (!cam_.set_trigger_software(false) || !cam_.set_frame_rate(fps)) {
    error_ = cam_.error();
    return false;
  }
  if (!start_common()) return false;
  acq_ = std::thread([this] { acq_freerun_loop(); });
  return true;
}

bool Pipeline::start_inspection(const InspectionPlan& plan) {
  if (!connect()) return false;
  if (!cam_.set_trigger_software(true)) {
    error_ = cam_.error();
    return false;
  }
  if (!start_common()) return false;
  acq_ = std::thread([this, plan] { acq_inspection_loop(plan); });
  return true;
}

void Pipeline::stop() {
  stop_ = true;
  if (acq_.joinable()) acq_.join();
  q_proc_.close();
  if (proc_.joinable()) proc_.join();
  if (running_) cam_.stop();
  running_ = false;
  q_proc_.reopen();  // it was closed only to release the processing thread
}

void Pipeline::jog(double delta_um) {
  std::lock_guard lk(jog_mu_);
  jogs_.push_back(delta_um);
}

void Pipeline::reset_error() {
  if (!running_) set_state(Event::Reset);
}

void Pipeline::drain_jogs() {
  std::vector<double> j;
  {
    std::lock_guard lk(jog_mu_);
    j.swap(jogs_);
  }
  for (double d : j) {
    const double target = stage_target_.load() + d;
    if (stage_.move(target) == StageClient::Status::Ok) stage_target_ = target;
  }
}

void Pipeline::note_seq(const RawFrame& f) {
  TruthStamp st;
  if (!f.complete || !read_stamp(f.pixels.data(), static_cast<size_t>(f.width), &st)) return;
  if (last_seq_ != 0 && st.seq > last_seq_ + 1) c_.seq_gap += st.seq - last_seq_ - 1;
  if (st.seq > last_seq_) last_seq_ = st.seq;
}

bool Pipeline::hand_off(Job job) {
  std::optional<Job> rejected;
  if (q_proc_.try_push(std::move(job), &rejected)) return true;
  c_.queue_drops++;
  if (rejected && rejected->frame) pool_.try_push(std::move(rejected->frame));
  return false;
}

void Pipeline::acq_freerun_loop() {
  apply_rt("acquisition", cfg_.rt.cpu_acq);
  while (!stop_) {
    drain_jogs();
    auto f = get_frame();
    if (!cam_.pop(*f, 100'000)) {
      pool_.try_push(std::move(f));
      continue;
    }
    if (!f->complete) {
      c_.rx_incomplete++;
      pool_.try_push(std::move(f));
      continue;
    }
    c_.rx_complete++;
    note_seq(*f);
    hand_off(Job{std::move(f), -1, stage_target_.load(), 0});
  }
}

bool Pipeline::recover(std::string why) {
  {
    std::lock_guard lk(err_mu_);
    last_error_ = why;
  }
  std::fprintf(stderr, "[station] error: %s -> recovering\n", why.c_str());
  for (int attempt = 0; attempt < 20 && !stop_; ++attempt) {
    if (!stage_.connected() && !stage_.connect(cfg_.stage_host, cfg_.stage_port)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    double x = 0;
    if (stage_.reset() == StageClient::Status::Ok &&
        stage_.position(&x, nullptr) == StageClient::Status::Ok) {
      stage_target_ = x;  // the axis now holds wherever it stopped
      set_state(Event::Reset);
      c_.recoveries++;
      return true;
    }
    stage_.disconnect();
  }
  return false;
}

void Pipeline::acq_inspection_loop(InspectionPlan plan) {
  apply_rt("acquisition", cfg_.rt.cpu_acq);
  std::mt19937_64 rng(plan.seed);
  std::uniform_real_distribution<double> pos(-plan.range_um, plan.range_um);
  using St = StageClient::Status;

  for (int cycle = 0; cycle < plan.cycles && !stop_; ++cycle) {
    const double target = pos(rng);
    const uint64_t t_cycle0 = now_ns();
    auto fail = [&](Event ev, const std::string& why) {
      set_state(ev);
      c_.cycles_failed++;
      if (!recover("cycle " + std::to_string(cycle) + ": " + why)) {
        std::fprintf(stderr, "[station] recovery failed, aborting plan\n");
        stop_ = true;
      }
    };
    auto ev_of = [](St s) { return s == St::Timeout ? Event::Timeout : Event::Fault; };

    if (auto it = plan.faults.find(cycle); it != plan.faults.end()) stage_.inject(it->second);

    set_state(Event::Start);
    St s = stage_.move(target);
    if (s != St::Ok) { fail(ev_of(s), std::string("MOVE ") + to_string(s)); continue; }
    stage_target_ = target;
    s = stage_.wait_arrived(plan.move_timeout_ms);
    if (s != St::Ok) { fail(ev_of(s), std::string("ARRIVE ") + to_string(s)); continue; }
    set_state(Event::Arrived);

    if (plan.settle == SettleMode::Controller) {
      s = stage_.wait_settled(plan.settle_timeout_ms);
      if (s != St::Ok) { fail(ev_of(s), std::string("SETTLE ") + to_string(s)); continue; }
    } else if (plan.settle == SettleMode::FixedDelay) {
      std::this_thread::sleep_for(std::chrono::milliseconds(plan.settle_delay_ms));
    }
    set_state(Event::Settled);

    // Trigger, then take the first frame rendered after the trigger. Frames
    // stamped before it (in flight from an earlier trigger) are stale.
    bool got = false;
    std::unique_ptr<RawFrame> f;
    uint64_t t_trig = 0;
    for (int attempt = 0; attempt <= plan.frame_retries && !got && !stop_; ++attempt) {
      if (attempt > 0) c_.retriggers++;
      t_trig = now_ns();
      if (!cam_.software_trigger()) break;
      const uint64_t deadline = t_trig + static_cast<uint64_t>(plan.frame_timeout_ms) * 1'000'000;
      while (!got && now_ns() < deadline) {
        if (!f) f = get_frame();
        if (!cam_.pop(*f, (deadline - now_ns()) / 1000 + 1)) break;
        if (!f->complete) { c_.rx_incomplete++; continue; }
        c_.rx_complete++;
        note_seq(*f);
        TruthStamp st;
        if (read_stamp(f->pixels.data(), static_cast<size_t>(f->width), &st) && st.exposure_start_ns < t_trig) {
          c_.stale_discarded++;
          continue;
        }
        got = true;
      }
    }
    if (!got) {
      if (f) pool_.try_push(std::move(f));
      fail(Event::Timeout, "no frame after trigger");
      continue;
    }
    set_state(Event::FrameReceived);

    hand_off(Job{std::move(f), cycle, target, t_trig});
    bool done;
    {
      std::unique_lock lk(cyc_mu_);
      done = cyc_cv_.wait_for(lk, std::chrono::milliseconds(1000), [&] { return cycle_done_ >= cycle || stop_; });
    }
    if (!done) { fail(Event::Timeout, "processing timeout"); continue; }
    set_state(Event::ResultReady);
    c_.cycles_ok++;
    lat_cycle_.add(ms(t_cycle0, now_ns()));
  }
  finished_ = true;
}

void Pipeline::proc_loop() {
  apply_rt("processing", cfg_.rt.cpu_proc);
  uint64_t index = 0;
  while (true) {
    auto job = q_proc_.pop_for(std::chrono::milliseconds(100));
    if (!job) {
      if (stop_ && q_proc_.size() == 0) break;
      continue;
    }
    RawFrame& f = *job->frame;
    InspectionResult r;
    r.index = index++;
    r.cycle = job->cycle;
    r.stage_target_um = job->stage_target_um;
    r.t_trigger_ns = job->t_trigger_ns;
    r.t_rx_ns = f.rx_ns;
    r.width = f.width;
    r.height = f.height;
    r.has_truth = read_stamp(f.pixels.data(), static_cast<size_t>(f.width), &r.truth);

    r.edges = measure_edges(f.pixels.data(), f.width, f.height, cfg_.roi, cfg_.fit);
    if (cfg_.proc_cost_us > 0) busy_us(cfg_.proc_cost_us);
    r.measured = r.edges.ok;
    if (!r.measured) c_.measure_fail++;
    if (r.measured && r.has_truth) {
      const double um = cfg_.scene.um_per_px;
      r.left_err_px = r.edges.left_px - static_cast<double>(r.truth.left_px_e6) / 1e6;
      r.right_err_px = r.edges.right_px - static_cast<double>(r.truth.right_px_e6) / 1e6;
      r.width_err_um = r.edges.width_px() * um - cfg_.scene.part_width_um;
      // Where the part is on the stage, using where we *commanded* the stage.
      const double part_um = r.stage_target_um + (r.edges.center_px() - f.width / 2.0) * um;
      r.pos_err_um = part_um - cfg_.scene.part_center_um;
    }
    r.t_done_ns = now_ns();
    c_.processed++;

    if (r.has_truth) {
      lat_e2e_.add(ms(r.truth.fill_ns, r.t_done_ns));
      lat_transport_.add(ms(r.truth.fill_ns, r.t_rx_ns));
      if (r.truth.readout_done_ns && f.leader_ns) {
        lat_readout_.add(ms(r.truth.fill_ns, r.truth.readout_done_ns));
        lat_wire_.add(ms(r.truth.readout_done_ns, f.leader_ns));
        lat_assembly_.add(ms(f.leader_ns, f.rx_ns));
      }
      if (r.t_trigger_ns) lat_trigger_.add(ms(r.t_trigger_ns, r.truth.exposure_start_ns));
    }
    lat_proc_.add(ms(r.t_rx_ns, r.t_done_ns));

#ifdef __unix__
    if (shm_) {
      shm_->publish(f.pixels.data(), static_cast<uint32_t>(f.width), static_cast<uint32_t>(f.height),
                    r.truth.seq, now_ns());
      c_.shm_published++;
    }
#endif
    // The GUI gets the newest frame only. One copy per *notified* result; when
    // the GUI is behind, results are coalesced instead of queuing up signals.
    const bool notify = on_result_ && !ui_pending_.exchange(true, std::memory_order_acq_rel);
    {
      std::lock_guard lk(res_mu_);
      if (results_.size() >= cfg_.max_results)
        results_.erase(results_.begin(), results_.begin() + static_cast<std::ptrdiff_t>(cfg_.max_results / 2));
      results_.push_back(r);
      if (notify) {
        auto latest = std::make_shared<InspectionResult>(r);
        latest->image = std::make_shared<const std::vector<uint8_t>>(f.pixels);
        latest_ = std::move(latest);
      }
    }
    if (on_result_) {
      if (notify) { c_.ui_notify++; on_result_(); }
      else c_.ui_coalesced++;
    }
    const int cycle = job->cycle;
    pool_.try_push(std::move(job->frame));
    if (cycle >= 0) {
      {
        std::lock_guard lk(cyc_mu_);
        cycle_done_ = cycle;
      }
      cyc_cv_.notify_all();
    }
  }
}

Snapshot Pipeline::snapshot() const {
  Snapshot s;
  s.state = state_.load();
  s.running = running_;
  s.rx_complete = c_.rx_complete;
  s.rx_incomplete = c_.rx_incomplete;
  s.seq_gap = c_.seq_gap;
  s.queue_drops = c_.queue_drops;
  s.processed = c_.processed;
  s.cycles_ok = c_.cycles_ok;
  s.cycles_failed = c_.cycles_failed;
  s.recoveries = c_.recoveries;
  s.stream = cam_.stream_stats();
  s.stage_target_um = stage_target_.load();
  s.e2e_ms = lat_e2e_.compute_recent(500);
  s.transport_ms = lat_transport_.compute_recent(500);
  s.proc_ms = lat_proc_.compute_recent(500);
  s.trigger_ms = lat_trigger_.compute_recent(500);
  const double secs = static_cast<double>(now_ns() - t_start_ns_) / 1e9;
  s.fps = secs > 0 ? static_cast<double>(c_.processed) / secs : 0;
  std::lock_guard lk(err_mu_);
  s.last_error = last_error_;
  return s;
}

std::shared_ptr<const InspectionResult> Pipeline::latest() const {
  std::lock_guard lk(res_mu_);
  return latest_;
}

std::vector<InspectionResult> Pipeline::results() const {
  std::lock_guard lk(res_mu_);
  return results_;
}

}  // namespace vs
