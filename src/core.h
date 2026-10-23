// core.h - HDRAGC-Next shared processing core (API-agnostic).
// Dual-interface refactor (M1): all algorithms live here; the AviSynth+
// and VapourSynth adapters (src/avisynth.cpp, src/vs.cpp) are thin shells
// that convert host frame objects into PlaneView and call processFrame().
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <functional>
#include <mutex>
#include <map>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace hdragc {

enum DitherMode { DITHER_NONE = 0, DITHER_ORDERED = 1, DITHER_BLUE = 2 };
enum ChromaMode { CHROMA_SIMPLE = 0, CHROMA_LUMA_RATIO = 1 };
enum ShowMode   { SHOW_NONE = 0, SHOW_BASE = 1, SHOW_MASK = 2, SHOW_LIFT = 3 };

// Host-agnostic description of one plane of one frame.
struct PlaneView {
    const uint8_t* data;
    int stride, w, h, bits;
};
struct WritablePlaneView {
    uint8_t* data;
    int stride, w, h, bits;
};

// Neutral frame source for the temporal analyzer. The adapter binds this to
// its host's frame fetch (AVS: child->GetFrame; VS: getFrameFilter).
struct FrameSrc {
    // fetch fills the luma view of frame n and returns an opaque token that
    // keeps the backing frame alive. The caller MUST call release(token)
    // after its last read of view data. (AVS token = heap PVideoFrame;
    // VS token = const VSFrame*. This avoids a use-after-free: the analyzer
    // reads the histogram AFTER fetch returns.)
    std::function<void*(int n, PlaneView& y, void* ctx)> fetch;
    std::function<void(void* token)> release;
    int num_frames = 0;
};

struct Params {
    float strength = 0.8f, protect = 0.8f, black = 0.0f, white = 0.0f;
    bool  auto_points = true;
    float temporal = 0.85f;
    int   temporal_radius = 4, temporal_mode = 0;
    float scene_cut = 0.35f, scene_cut_low = 0.20f;
    int   scene_cooldown = 3;
    float detail_gain = 1.0f, saturation = 1.0f;
    bool  auto_saturation = true;
    int   chroma_mode = CHROMA_LUMA_RATIO;
    float luma_ratio_mix = 0.7f, chroma_softknee = 0.9f;
    int   levels = 4;
    float local_mix = 1.0f, shadow_threshold = 0.35f, mask_gamma = 2.0f;
    int   mask_level = -1;
    int   output_bits = 0;          // 0 = follow input
    int   dither = DITHER_BLUE;
    int   show = SHOW_NONE;
    float natural = 0.0f, veil = 0.0f;
};

// ---- shared building blocks (used by core and by unit-style tests) ----
extern const float kBayer8[8][8];

float DitherLsb(int x, int y, DitherMode dm, const float* blue);
void  ProcessPlane(const uint8_t* sp, int spitch, uint8_t* dp, int dpitch,
                   int rowbytes, int h, int sbits, int dbits, DitherMode dm,
                   const float* blue);
void  ApplyToneCurve(const uint8_t* sp, int spitch, uint8_t* dp, int dpitch,
                     int rowbytes, int h, int sbits, int dbits,
                     const float* lut, DitherMode dm, const float* blue);

struct RawFrameParams {
    float black, white, tau;
    float p[5];
    float contrast;
    int   dark;
    float score;
};

class TemporalAnalyzer {
public:
    TemporalAnalyzer(FrameSrc src, int src_bits, bool auto_points,
                     float fixed_black, float fixed_white, float shadow_threshold,
                     float temporal, int radius, int mode,
                     float scene_cut, float scene_cut_low, int cooldown);

    struct FrameState { float black, white, tau; int dark; };
    FrameState GetState(int n, void* ctx);

private:
    static FrameState Lerp(const FrameState& p, const RawFrameParams& r, float a);
    RawFrameParams GetRaw(int n, void* ctx);

    FrameSrc src_;
    int src_bits_;
    bool auto_points_;
    float fixed_black_, fixed_white_, shadow_thr_;
    float alpha_;
    int radius_, mode_;
    float scene_cut_, scene_cut_low_;
    int cooldown_init_, cooldown_, suspect_;
    std::map<int, RawFrameParams> raw_cache_;
    std::map<int, FrameState> state_cache_;
    std::mutex mu_;
};

class HDRAGCCore {
public:
    HDRAGCCore(const Params& p, FrameSrc src, int src_bits);
    ~HDRAGCCore();

    // Process one frame. yuv_in[3] = Y,U,V of source frame n;
    // out[3] = writable destination planes (format chosen by adapter).
    void processFrame(const PlaneView yuv_in[3], WritablePlaneView out[3], int n, void* ctx);

private:
    Params p_;
    FrameSrc src_;
    int src_bits_;
    DitherMode dither_;
    float blue64_[64][64];
    float lut_buf_[4096];
    TemporalAnalyzer analyzer_;
};

} // namespace hdragc
