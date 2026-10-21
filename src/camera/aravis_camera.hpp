#pragma once
// Thin C++ wrapper over Aravis (GigE Vision / USB3 Vision via GenICam).
//
// Features are resolved by *checking the device's GenICam node map*, not by
// assuming SFNC names: the Aravis fake camera exposes ExposureTimeAbs and
// GainRaw, not ExposureTime and Gain. resolve() tries the SFNC name first and
// falls back, and feature_report() records which node was actually used.
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

typedef struct _ArvCamera ArvCamera;
typedef struct _ArvStream ArvStream;
typedef struct _ArvBuffer ArvBuffer;

namespace vs {

struct FeatureInfo {
  std::string wanted;    // SFNC name we asked for
  std::string resolved;  // node actually present ("" = missing)
  std::string type;      // GenICam node type
  std::string value;     // current value as string
};

struct StreamOptions {
  int n_buffers = 16;
  bool socket_buffer_auto = true;  // size SO_RCVBUF to the payload (see README: GVSP loss)
  int packet_size = 0;             // 0 = leave device default
  // Called once from Aravis's own GVSP receive thread when it starts, so the
  // caller can apply SCHED_FIFO / pinning to the thread that reads the socket.
  std::function<void()> on_stream_thread_start;
};

struct StreamStats {
  uint64_t completed = 0, failures = 0, underruns = 0;
  uint64_t missing_packets = 0, resent_packets = 0, received_packets = 0;
};

// A frame copied out of an Aravis buffer (the buffer goes straight back to the stream).
struct RawFrame {
  uint64_t camera_frame_id = 0;
  uint64_t camera_ts_ns = 0;
  uint64_t rx_ns = 0;  // host monotonic time of pop
  uint64_t leader_ns = 0;  // host monotonic time the GVSP leader arrived (Aravis rx thread)
  int width = 0, height = 0;
  bool complete = false;  // ARV_BUFFER_STATUS_SUCCESS
  int status = 0;
  std::vector<uint8_t> pixels;
};

class AravisCamera {
 public:
  AravisCamera();
  ~AravisCamera();
  AravisCamera(const AravisCamera&) = delete;
  AravisCamera& operator=(const AravisCamera&) = delete;

  // id: device id, or an IP address ("127.0.0.1" for the rig).
  bool open(const std::string& id);
  bool is_open() const { return cam_ != nullptr; }
  const std::string& error() const { return error_; }
  std::string model() const;

  // GenICam feature access. All control calls are serialised by a mutex so the
  // GUI thread and the acquisition thread can both issue GVCP commands.
  std::optional<std::string> resolve(const std::vector<std::string>& candidates) const;
  std::vector<FeatureInfo> feature_report() const;
  bool set_exposure_us(double us);
  double exposure_us() const;
  bool set_gain(double g);
  double gain() const;
  bool set_pixel_format(const std::string& fmt);
  std::string pixel_format() const;
  std::vector<std::string> pixel_formats() const;
  bool set_frame_rate(double fps);
  bool set_trigger_software(bool on);  // TriggerMode On + TriggerSource Software
  bool software_trigger();              // TriggerSoftware command
  bool set_region(int w, int h);

  bool start(const StreamOptions& opt);
  void stop();
  // Pops one buffer (copying into `out`), returns false on timeout.
  bool pop(RawFrame& out, uint64_t timeout_us);
  StreamStats stream_stats() const;

 private:
  bool set_feature_float(const std::vector<std::string>& names, double v);
  double get_feature_float(const std::vector<std::string>& names) const;
  bool check(const char* what) const;  // consumes GError

  ArvCamera* cam_ = nullptr;
  ArvStream* stream_ = nullptr;
  mutable std::mutex ctl_;
  mutable std::string error_;
  mutable void* gerror_ = nullptr;  // GError*
  std::function<void()> on_thread_start_;
  size_t payload_ = 0;
};

}  // namespace vs
