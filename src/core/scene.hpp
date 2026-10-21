#pragma once
// The simulated part under the camera, and the ground truth that travels with
// each frame.
//
// A rectangular part (bright on dark) sits on the stage at world x = 0. The
// camera looks down; moving the stage by +dx moves the part by -dx/um_per_px
// in the image. Edges are rendered exactly: an ideal step blurred by a
// Gaussian PSF and integrated over each pixel, so the true sub-pixel edge
// positions are known to float precision.
//
// Ground truth is stamped into row 0 of every frame (TruthStamp), so it rides
// the real GVSP stream and needs no side channel. Measurement never reads row 0.
#include <cstdint>
#include <span>
#include <vector>

namespace vs {

struct SceneParams {
  int width = 640;
  int height = 480;
  double um_per_px = 10.0;      // optical scale
  double part_width_um = 2345.6;  // true part width (234.56 px)
  double part_center_um = 0.0;  // part position on the stage (world)
  int part_row0 = 100, part_row1 = 380;
  double background = 40.0, foreground = 200.0;
  double psf_sigma_px = 0.8;
  double noise_sigma = 2.0;
};

// Pixel coordinate convention used everywhere: pixel i covers [i, i+1).
struct EdgeTruth {
  double left_px = 0, right_px = 0;
};

// Image x of the part edges when the stage is at `stage_x_um`.
EdgeTruth edges_at(const SceneParams& p, double stage_x_um);

// Renders one frame. `stage_samples_um` are stage positions sampled across the
// exposure window (one sample = no motion blur). `seed` drives the noise.
void render_part(const SceneParams& p, std::span<const double> stage_samples_um, uint64_t seed,
                 uint8_t* out /* width*height Mono8 */);

#pragma pack(push, 1)
struct TruthStamp {
  uint32_t magic = 0x53545356;  // "VSTS"
  uint32_t version = 1;
  uint64_t seq = 0;            // rig frame sequence (gap = lost frame)
  uint64_t exposure_start_ns = 0;  // CLOCK_MONOTONIC when exposure began (trigger seen)
  uint64_t fill_ns = 0;            // end of exposure, i.e. start of readout
  uint64_t readout_done_ns = 0;    // render finished; packets go out right after
  int64_t stage_x_nm = 0;      // mean stage position during exposure
  int64_t left_px_e6 = 0, right_px_e6 = 0;  // true edges of the mean position
  uint32_t exposure_us = 0;
  uint32_t stage_in_position = 0;  // 1 if the stage was settled for the whole exposure
};
#pragma pack(pop)

void write_stamp(const TruthStamp& s, uint8_t* row0, size_t row_bytes);
bool read_stamp(const uint8_t* row0, size_t row_bytes, TruthStamp* out);

}  // namespace vs
