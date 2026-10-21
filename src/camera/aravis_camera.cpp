#include "camera/aravis_camera.hpp"

#include <arv.h>

#include <cstring>

#include "core/latency.hpp"

namespace vs {
namespace {

const std::vector<std::string> kExposure = {"ExposureTime", "ExposureTimeAbs"};
const std::vector<std::string> kGain = {"Gain", "GainRaw"};

GError** gerr(void*& p) { return reinterpret_cast<GError**>(&p); }

void stream_cb(void* user, ArvStreamCallbackType type, ArvBuffer*) {
  if (type != ARV_STREAM_CALLBACK_TYPE_INIT) return;
  auto* fn = static_cast<std::function<void()>*>(user);
  if (fn && *fn) (*fn)();
}

}  // namespace

AravisCamera::AravisCamera() = default;

AravisCamera::~AravisCamera() {
  stop();
  if (cam_) g_object_unref(cam_);
}

bool AravisCamera::check(const char* what) const {
  GError*& e = *gerr(gerror_);
  if (!e) return true;
  error_ = std::string(what) + ": " + e->message;
  g_clear_error(&e);
  return false;
}

bool AravisCamera::open(const std::string& id) {
  std::lock_guard lk(ctl_);
  cam_ = arv_camera_new(id.empty() ? nullptr : id.c_str(), gerr(gerror_));
  return check("open");
}

std::string AravisCamera::model() const {
  std::lock_guard lk(ctl_);
  const char* m = arv_camera_get_model_name(cam_, gerr(gerror_));
  check("model");
  return m ? m : "";
}

std::optional<std::string> AravisCamera::resolve(const std::vector<std::string>& candidates) const {
  ArvDevice* dev = arv_camera_get_device(cam_);
  for (const auto& n : candidates) {
    ArvGcNode* node = arv_device_get_feature(dev, n.c_str());
    if (node && ARV_IS_GC_FEATURE_NODE(node) &&
        arv_gc_feature_node_is_available(ARV_GC_FEATURE_NODE(node), nullptr))
      return n;
  }
  return std::nullopt;
}

std::vector<FeatureInfo> AravisCamera::feature_report() const {
  std::lock_guard lk(ctl_);
  const std::vector<std::vector<std::string>> wanted = {
      kExposure, kGain, {"PixelFormat"}, {"TriggerMode"}, {"TriggerSource"}, {"TriggerSoftware"},
      {"TriggerSelector"}, {"AcquisitionFrameRate", "AcquisitionFrameRateAbs"}, {"Width"}, {"Height"},
      {"GevSCPSPacketSize"}, {"GevTimestampTickFrequency"}, {"DeviceTemperature"}};
  std::vector<FeatureInfo> out;
  ArvDevice* dev = arv_camera_get_device(cam_);
  for (const auto& c : wanted) {
    FeatureInfo fi;
    fi.wanted = c.front();
    if (auto r = resolve(c)) {
      fi.resolved = *r;
      ArvGcNode* node = arv_device_get_feature(dev, r->c_str());
      fi.type = G_OBJECT_TYPE_NAME(node);
      if (!ARV_IS_GC_COMMAND(node)) {
        GError* e = nullptr;
        const char* v = arv_gc_feature_node_get_value_as_string(ARV_GC_FEATURE_NODE(node), &e);
        fi.value = v ? v : "";
        if (e) g_error_free(e);
      }
    }
    out.push_back(fi);
  }
  return out;
}

bool AravisCamera::set_feature_float(const std::vector<std::string>& names, double v) {
  auto n = resolve(names);
  if (!n) { error_ = "no node for " + names.front(); return false; }
  ArvDevice* dev = arv_camera_get_device(cam_);
  ArvGcNode* node = arv_device_get_feature(dev, n->c_str());
  if (ARV_IS_GC_FLOAT(node))
    arv_device_set_float_feature_value(dev, n->c_str(), v, gerr(gerror_));
  else
    arv_device_set_integer_feature_value(dev, n->c_str(), static_cast<gint64>(v), gerr(gerror_));
  return check(n->c_str());
}

double AravisCamera::get_feature_float(const std::vector<std::string>& names) const {
  auto n = resolve(names);
  if (!n) return 0;
  ArvDevice* dev = arv_camera_get_device(cam_);
  ArvGcNode* node = arv_device_get_feature(dev, n->c_str());
  double v = ARV_IS_GC_FLOAT(node) ? arv_device_get_float_feature_value(dev, n->c_str(), gerr(gerror_))
                                   : static_cast<double>(arv_device_get_integer_feature_value(dev, n->c_str(), gerr(gerror_)));
  check(n->c_str());
  return v;
}

bool AravisCamera::set_exposure_us(double us) { std::lock_guard lk(ctl_); return set_feature_float(kExposure, us); }
double AravisCamera::exposure_us() const { std::lock_guard lk(ctl_); return get_feature_float(kExposure); }
bool AravisCamera::set_gain(double g) { std::lock_guard lk(ctl_); return set_feature_float(kGain, g); }
double AravisCamera::gain() const { std::lock_guard lk(ctl_); return get_feature_float(kGain); }

bool AravisCamera::set_pixel_format(const std::string& fmt) {
  std::lock_guard lk(ctl_);
  arv_camera_set_pixel_format_from_string(cam_, fmt.c_str(), gerr(gerror_));
  return check("PixelFormat");
}

std::string AravisCamera::pixel_format() const {
  std::lock_guard lk(ctl_);
  const char* s = arv_camera_get_pixel_format_as_string(cam_, gerr(gerror_));
  check("PixelFormat");
  return s ? s : "";
}

std::vector<std::string> AravisCamera::pixel_formats() const {
  std::lock_guard lk(ctl_);
  guint n = 0;
  const char** f = arv_camera_dup_available_pixel_formats_as_strings(cam_, &n, gerr(gerror_));
  check("PixelFormat list");
  std::vector<std::string> out;
  for (guint i = 0; f && i < n; ++i) out.emplace_back(f[i]);
  g_free(f);
  return out;
}

bool AravisCamera::set_frame_rate(double fps) {
  std::lock_guard lk(ctl_);
  arv_camera_set_frame_rate(cam_, fps, gerr(gerror_));
  return check("AcquisitionFrameRate");
}

bool AravisCamera::set_trigger_software(bool on) {
  std::lock_guard lk(ctl_);
  if (on) {
    arv_camera_set_trigger(cam_, "Software", gerr(gerror_));  // TriggerMode=On, Source=Software
    return check("trigger");
  }
  arv_camera_clear_triggers(cam_, gerr(gerror_));
  return check("clear triggers");
}

bool AravisCamera::software_trigger() {
  std::lock_guard lk(ctl_);
  arv_camera_software_trigger(cam_, gerr(gerror_));
  return check("TriggerSoftware");
}

bool AravisCamera::set_region(int w, int h) {
  std::lock_guard lk(ctl_);
  arv_camera_set_region(cam_, 0, 0, w, h, gerr(gerror_));
  return check("region");
}

bool AravisCamera::start(const StreamOptions& opt) {
  std::lock_guard lk(ctl_);
  if (opt.packet_size > 0) {
    arv_camera_gv_set_packet_size(cam_, opt.packet_size, gerr(gerror_));
    if (!check("packet size")) return false;
  }
  if (ARV_IS_GV_DEVICE(arv_camera_get_device(cam_)))
    arv_camera_gv_set_stream_options(cam_, opt.packet_socket ? ARV_GV_STREAM_OPTION_NONE
                                                             : ARV_GV_STREAM_OPTION_PACKET_SOCKET_DISABLED);
  on_thread_start_ = opt.on_stream_thread_start;
  stream_ = arv_camera_create_stream(cam_, stream_cb, &on_thread_start_, gerr(gerror_));
  if (!check("create stream")) return false;
  if (ARV_IS_GV_STREAM(stream_) && opt.socket_buffer_auto)
    g_object_set(stream_, "socket-buffer", ARV_GV_STREAM_SOCKET_BUFFER_AUTO, nullptr);
  payload_ = arv_camera_get_payload(cam_, gerr(gerror_));
  if (!check("payload")) return false;
  for (int i = 0; i < opt.n_buffers; ++i) arv_stream_push_buffer(stream_, arv_buffer_new(payload_, nullptr));
  arv_camera_start_acquisition(cam_, gerr(gerror_));
  return check("start acquisition");
}

void AravisCamera::stop() {
  std::lock_guard lk(ctl_);
  if (!stream_) return;
  arv_camera_stop_acquisition(cam_, gerr(gerror_));
  check("stop");
  g_object_unref(stream_);
  stream_ = nullptr;
}

bool AravisCamera::pop(RawFrame& out, uint64_t timeout_us) {
  ArvStream* s = stream_;
  if (!s) return false;
  ArvBuffer* b = arv_stream_timeout_pop_buffer(s, timeout_us);
  if (!b) return false;
  out.rx_ns = now_ns();
  out.status = arv_buffer_get_status(b);
  out.complete = out.status == ARV_BUFFER_STATUS_SUCCESS;
  out.camera_frame_id = arv_buffer_get_frame_id(b);
  out.camera_ts_ns = arv_buffer_get_timestamp(b);
  // Aravis stamps the GVSP leader's arrival with the realtime clock; map it
  // onto CLOCK_MONOTONIC with the offset between the two clocks right now.
  const int64_t offset = static_cast<int64_t>(g_get_real_time()) * 1000 - static_cast<int64_t>(now_ns());
  const uint64_t sys = arv_buffer_get_system_timestamp(b);
  out.leader_ns = sys ? static_cast<uint64_t>(static_cast<int64_t>(sys) - offset) : 0;
  if (out.complete && arv_buffer_get_payload_type(b) == ARV_BUFFER_PAYLOAD_TYPE_IMAGE) {
    out.width = arv_buffer_get_image_width(b);
    out.height = arv_buffer_get_image_height(b);
    size_t n = 0;
    const void* d = arv_buffer_get_image_data(b, &n);
    out.pixels.resize(n);  // capacity is reused across frames: no steady-state allocation
    std::memcpy(out.pixels.data(), d, n);
  } else {
    out.width = out.height = 0;
    out.pixels.clear();
  }
  arv_stream_push_buffer(s, b);
  return true;
}

StreamStats AravisCamera::stream_stats() const {
  StreamStats st;
  if (!stream_) return st;
  guint64 c = 0, f = 0, u = 0;
  arv_stream_get_statistics(stream_, &c, &f, &u);
  st.completed = c;
  st.failures = f;
  st.underruns = u;
  for (guint i = 0; i < arv_stream_get_n_infos(stream_); ++i) {
    if (arv_stream_get_info_type(stream_, i) != G_TYPE_UINT64) continue;
    const std::string n = arv_stream_get_info_name(stream_, i);
    const uint64_t v = arv_stream_get_info_uint64(stream_, i);
    if (n == "n_missing_packets") st.missing_packets = v;
    else if (n == "n_resent_packets") st.resent_packets = v;
    else if (n == "n_received_packets") st.received_packets = v;
  }
  return st;
}

}  // namespace vs
