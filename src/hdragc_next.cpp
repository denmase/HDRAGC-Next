// HDRAGCNext - Milestone 3: global tone curve on luma (LUT + monotonicity guard)
// Curve per design doc v2.1 SS10 (endpoint-preserving variant):
//   xn  = clamp((x - black)/(white - black), 0, 1)
//   y   = xn * (1 + s*(1-xn))              // shadow lift, s=strength in [0,1]
//   h   = p * smoothstep(0.7, 1.0, y)      // highlight weight, p=protect in [0,1]
//   y   = pow(y, 1 + h)                    // gentle highlight compress; y(1)=1 kept
// Provably monotonic on [0,1]; LUT assertion double-checks (SS10.6).
#include <avisynth.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <map>
#include <mutex>

#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
const AVS_Linkage* AVS_linkage = nullptr;

namespace {

enum DitherMode { DITHER_NONE = 0, DITHER_ORDERED = 1, DITHER_BLUE = 2 }; // BLUE: TODO

static const float kBayer8[8][8] = {
  { 0,32, 8,40, 2,34,10,42},{48,16,56,24,50,18,58,26},
  {12,44, 4,36,14,46, 6,38},{60,28,52,20,62,30,54,22},
  { 3,35,11,43, 1,33, 9,41},{51,19,59,27,49,17,57,25},
  {15,47, 7,39,13,45, 5,37},{63,31,55,23,61,29,53,21}
};

static inline float DitherLsb(int x, int y, DitherMode dm, const float* blue)
{
    if (dm == DITHER_BLUE && blue) return blue[(y & 63) * 64 + (x & 63)] - 0.5f;
    return (kBayer8[y & 7][x & 7] + 0.5f) / 64.0f - 0.5f;
}


static inline float smoothstep01(float t) {
    t = std::min(1.0f, std::max(0.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

// -- chroma / identity path (M2, unchanged) --
static void ProcessPlane(const uint8_t* sp, int spitch, uint8_t* dp, int dpitch,
                         int rowbytes, int h, int sbits, int dbits, DitherMode dm,
                         const float* blue)
{
    const int selem = (sbits == 8) ? 1 : 2;
    const int elems = rowbytes / selem;
    const float smax = (float)((1 << sbits) - 1);
    const float dmax = (float)((1 << dbits) - 1);
    const bool dither = (dm != DITHER_NONE) && (dbits < sbits);
    for (int y = 0; y < h; y++) {
        const uint8_t* srow = sp + (size_t)y * spitch;
        uint8_t* drow = dp + (size_t)y * dpitch;
        for (int x = 0; x < elems; x++) {
            float f = (sbits == 8) ? (float)srow[x] / smax
                                   : (float)((const uint16_t*)srow)[x] / smax;
            double v = (double)f * (double)dmax;
            if (dither) v += (double)DitherLsb(x, y, dm, blue);
            long iv = lrint(v);
            if (iv < 0) iv = 0;
            if (iv > (long)dmax) iv = (long)dmax;
            if (dbits == 8) drow[x] = (uint8_t)iv;
            else            ((uint16_t*)drow)[x] = (uint16_t)iv;
        }
    }
}

// ---- Laplacian pyramid (Milestone 4) ----
struct Buffer { int w, h; std::vector<float> d; void alloc(int W, int H) { w=W; h=H; d.resize((size_t)W*H); } };

// 5-tap Gaussian [1,4,6,4,1]/16, separable, edge clamp, then 2x decimate.
// Pass 1: vertical blur at every src row -> tmp (s.w x dst.h rows? No: tmp is
// dst.h rows where each row is the blurred value of src row 2*y).
// Pass 2: horizontal blur + decimate on tmp.
static void Downsample2x(const Buffer& s, Buffer& dst)
{
    const int dw = (s.w / 2) < 1 ? 1 : (s.w / 2);
    const int dh = (s.h / 2) < 1 ? 1 : (s.h / 2);
    dst.alloc(dw, dh);
    auto clampi = [](int v, int m) { return v < 0 ? 0 : (v >= m ? m - 1 : v); };
    static const float c[5] = {1,4,6,4,1};

    std::vector<float> tmp((size_t)dw * s.h);   // horizontally-decimated, all src rows
    for (int y = 0; y < s.h; y++) {
        const float* row = &s.d[(size_t)y * s.w];
        for (int x = 0; x < dw; x++) {
            float v = 0.f;
            for (int k = -2; k <= 2; k++)
                v += c[k+2] * row[clampi(2*x + k, s.w)];
            tmp[(size_t)y * dw + x] = v / 16.f;
        }
    }
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            float v = 0.f;
            for (int k = -2; k <= 2; k++)
                v += c[k+2] * tmp[(size_t)clampi(2*y + k, s.h) * dw + x];
            dst.d[(size_t)y * dw + x] = v / 16.f;
        }
    }
}

// bilinear 2x upsample, map (x+0.5)/2-0.5, edge clamp
static void Upsample2x(const Buffer& s, Buffer& dst, int dw, int dh)
{
    dst.alloc(dw, dh);
    for (int y = 0; y < dh; y++) {
        float gy = (y + 0.5f) * 0.5f - 0.5f;
        int y0 = (int)floorf(gy); float fy = gy - y0;
        int y1 = y0 + 1;
        y0 = y0 < 0 ? 0 : (y0 >= s.h ? s.h - 1 : y0);
        y1 = y1 < 0 ? 0 : (y1 >= s.h ? s.h - 1 : y1);
        for (int x = 0; x < dw; x++) {
            float gx = (x + 0.5f) * 0.5f - 0.5f;
            int x0 = (int)floorf(gx); float fx = gx - x0;
            int x1 = x0 + 1;
            x0 = x0 < 0 ? 0 : (x0 >= s.w ? s.w - 1 : x0);
            x1 = x1 < 0 ? 0 : (x1 >= s.w ? s.w - 1 : x1);
            float a = s.d[(size_t)y0*s.w + x0], b = s.d[(size_t)y0*s.w + x1];
            float c = s.d[(size_t)y1*s.w + x0], e = s.d[(size_t)y1*s.w + x1];
            dst.d[(size_t)y*dw + x] = (a + fx*(b-a)) * (1-fy) + (c + fx*(e-c)) * fy;
        }
    }
}

// -- luma statistics for auto points --
struct LumaStats { float black, white, p50; };  // normalized 0..1

static LumaStats ComputeLumaStats(const PVideoFrame& f, int sbits)
{
    const uint8_t* p = f->GetReadPtr(PLANAR_Y);
    const int pitch = f->GetPitch(PLANAR_Y);
    const int w = f->GetRowSize(PLANAR_Y) / ((sbits == 8) ? 1 : 2);
    const int h = f->GetHeight(PLANAR_Y);
    const int nbins = 1 << sbits;
    std::vector<uint32_t> hist(nbins, 0);
    uint64_t total = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = p + (size_t)y * pitch;
        if (sbits == 8) {
            for (int x = 0; x < w; x++) hist[row[x]]++;
        } else {
            const uint16_t* r16 = (const uint16_t*)row;
            for (int x = 0; x < w; x++) hist[std::min(r16[x], (uint16_t)(nbins - 1))]++;
        }
        total += w;
    }
    auto pct = [&](double p01) -> float {
        uint64_t target = (uint64_t)(total * p01);
        uint64_t cum = 0;
        for (int i = 0; i < nbins; i++) {
            cum += hist[i];
            if (cum >= target) return (float)i / (float)(nbins - 1);
        }
        return 1.0f;
    };
    LumaStats s;
    s.black = pct(0.01);
    s.white = pct(0.99);
    s.p50   = pct(0.50);
    return s;
}

// ---- Temporal analyzer (Milestone 5, design doc SS11-13) ----
struct RawParams {
    float black, white, tau;     // effective raw params this frame (auto or fixed)
    int dark;                    // konten gelap (p50<0.25) -> mask_gamma efektif 1.2
    float p[5];                  // percentiles p01,p25,p50,p75,p99 (normalized)
    float contrast;              // p95-p05
    float score;                 // scene cut score vs previous frame (v2)
};

struct FrameState { float black, white, tau; int dark; };

static float SceneScoreV2(const RawParams& a, const RawParams& b)
{
    // v2: percentile-delta metric (design doc SS12.3, robust variant).
    // d_shift: mean normalized |delta| over 5 percentiles (12.5% shift = 1.0).
    // Fades shift all percentiles slightly (score <0.2, no cut); real cuts
    // shift them a lot (score >=0.35, cut). Spike-mass histogram metrics are
    // scale-blind and fire on any redistribution -- rejected empirically.
    float d_shift = 0.f;
    for (int i = 0; i < 5; i++) {
        float d = fabsf(a.p[i] - b.p[i]) / 0.125f;
        d_shift += (d > 1.f) ? 1.f : d;
    }
    d_shift /= 5.f;
    float d_p50 = fabsf(a.p[2] - b.p[2]) / 0.25f;
    if (d_p50 > 1.f) d_p50 = 1.f;
    float denom = a.contrast > 0.05f ? a.contrast : 0.05f;
    float d_contrast = fabsf(a.contrast - b.contrast) / denom;
    if (d_contrast > 1.f) d_contrast = 1.f;
    return 0.6f * d_shift + 0.25f * d_contrast + 0.15f * d_p50;
}

class TemporalAnalyzer
{
public:
    TemporalAnalyzer(PClip child, int src_bits, bool auto_points,
                     float fixed_black, float fixed_white, float shadow_threshold,
                     float temporal, int radius, int mode,
                     float scene_cut, float scene_cut_low, int cooldown)
        : child_(child), src_bits_(src_bits), auto_points_(auto_points),
          fixed_black_(fixed_black), fixed_white_(fixed_white),
          shadow_thr_(shadow_threshold), alpha_(1.0f - temporal),
          radius_(radius), mode_(mode), scene_cut_(scene_cut),
          scene_cut_low_(scene_cut_low), cooldown_init_(cooldown),
          cooldown_(0), suspect_(0) {}

    FrameState GetState(int n, IScriptEnvironment* env)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = state_cache_.find(n);
        if (it != state_cache_.end()) return it->second;

        if (mode_ == 1) { // window: deterministic average, no state
            FrameState acc { 0, 0, 0, 0 };
            int cnt = 0;
            for (int k = n - radius_; k <= n + radius_; k++) {
                if (k < 0 || k >= child_->GetVideoInfo().num_frames) continue;
                RawParams r = GetRaw(k, env);
                acc.black += r.black; acc.white += r.white; acc.tau += r.tau;
                cnt++;
            }
            if (cnt == 0) cnt = 1;
            FrameState s { acc.black / cnt, acc.white / cnt, acc.tau / cnt, 0 };
            state_cache_[n] = s;
            return s;
        }

        // IIR mode. Anchor: largest cached frame < n.
        int m = -1;
        if (!state_cache_.empty()) {
            auto it2 = state_cache_.lower_bound(n);
            if (it2 != state_cache_.begin()) { --it2; m = it2->first; }
        }
        const int max_gap = radius_ * 2;
        if (m < 0 || n - m > max_gap) {   // init / reset after seek gap (SS13.2)
            RawParams r = GetRaw(n, env);
            FrameState s { r.black, r.white, r.tau, r.dark };
            state_cache_[n] = s;
            cooldown_ = cooldown_init_; suspect_ = 0;
            return s;
        }
        for (int i = m + 1; i <= n; i++) {
            RawParams r  = GetRaw(i, env);
            RawParams rp = GetRaw(i - 1, env);
            r.score = SceneScoreV2(rp, r);
            if (getenv("TA_DBG"))
                fprintf(stderr, "[TA] i=%d score=%.3f raw(b=%.3f,w=%.3f,t=%.3f) prev(b=%.3f,w=%.3f)\n",
                        i, r.score, r.black, r.white, r.tau,
                        state_cache_[i-1].black, state_cache_[i-1].white);
            const FrameState& prev = state_cache_[i - 1];
            FrameState cur;
            if (cooldown_ > 0) cooldown_--;
            if (r.score >= scene_cut_) {                    // confirmed cut
                cur = FrameState{ r.black, r.white, r.tau, r.dark };
                cooldown_ = cooldown_init_; suspect_ = 0;
            } else if (r.score >= scene_cut_low_) {         // suspicious
                if (suspect_) {                             // 2 consecutive -> cut
                    cur = FrameState{ r.black, r.white, r.tau, r.dark };
                    cooldown_ = cooldown_init_; suspect_ = 0;
                } else {                                    // spike: faster adapt, NO reset
                    suspect_ = 1;
                    float a = alpha_ * 2.5f; if (a > 1.f) a = 1.f;
                    cur = Lerp(prev, r, a);
                }
            } else {
                suspect_ = 0;
                cur = Lerp(prev, r, alpha_);
            }
            state_cache_[i] = cur;
        }
        return state_cache_[n];
    }

private:
    static FrameState Lerp(const FrameState& p, const RawParams& r, float a)
    {
        return FrameState{ p.black + a * (r.black - p.black),
                           p.white + a * (r.white - p.white),
                           p.tau   + a * (r.tau   - p.tau) };
    }

    RawParams GetRaw(int n, IScriptEnvironment* env)
    {
        auto it = raw_cache_.find(n);
        if (it != raw_cache_.end()) return it->second;
        PVideoFrame f = child_->GetFrame(n, env);
        const uint8_t* p = f->GetReadPtr(PLANAR_Y);
        const int pitch = f->GetPitch(PLANAR_Y);
        const int w = f->GetRowSize(PLANAR_Y) / ((src_bits_ == 8) ? 1 : 2);
        const int h = f->GetHeight(PLANAR_Y);
        const int nbins = 1 << src_bits_;
        std::vector<uint32_t> hist(nbins, 0);
        for (int y = 0; y < h; y++) {
            const uint8_t* row = p + (size_t)y * pitch;
            if (src_bits_ == 8) for (int x = 0; x < w; x++) hist[row[x]]++;
            else { const uint16_t* r16 = (const uint16_t*)row;
                   for (int x = 0; x < w; x++) hist[std::min(r16[x], (uint16_t)(nbins-1))]++; }
        }
        const uint64_t total = (uint64_t)w * h;
        auto pct = [&](double q) -> float {
            uint64_t target = (uint64_t)(total * q), cum = 0;
            for (int i = 0; i < nbins; i++) { cum += hist[i]; if (cum >= target) return (float)i / (nbins - 1); }
            return 1.0f;
        };
        RawParams r;
        static const double qs[5] = { 0.01, 0.25, 0.50, 0.75, 0.99 };
        for (int i = 0; i < 5; i++) r.p[i] = pct(qs[i]);
        // Guard band (temuan frame real M7): stretch black=p01 MENGHANCURKAN semua
        // pixel di bawah p01 -- padahal di foto gelap itu justru massa shadow yang
        // mau diangkat. Black diturunkan 0.10, white dinaikkan 0.05 supaya massa
        // percentile tidak ter-crush.
        // Guard band (temuan T16) + STRETCH BLENDING k=0.6 (temuan visual: stretch
        // percentil penuh pada gambar narrow-range = re-gamma total -> HAZE).
        // Titik auto hanya ditarik 60% dari identity: remap lebih lembut, kontras
        // tonal asli lebih terjaga.
        const float kStretch = 0.6f;
        float bb = (r.p[0] - 0.10f > 0.f ? r.p[0] - 0.10f : 0.f) * kStretch;
        float ww = 1.0f - (1.0f - (r.p[4] + 0.05f < 1.f ? r.p[4] + 0.05f : 1.f)) * kStretch;
        r.black = auto_points_ ? bb : fixed_black_;
        r.white = auto_points_ ? ww : fixed_white_;
        r.contrast = pct(0.95) - pct(0.05);
        r.dark = (r.p[2] < 0.25f) ? 1 : 0;
        r.tau = (shadow_thr_ > 0.f) ? shadow_thr_
                                    : (r.dark ? 0.45f
                                              : std::min(0.45f, std::max(0.20f, r.p[2])));
        r.score = 0.f;
        raw_cache_[n] = r;
        return r;
    }

    PClip child_;
    int src_bits_;
    bool auto_points_;
    float fixed_black_, fixed_white_, shadow_thr_;
    float alpha_;
    int radius_, mode_;
    float scene_cut_, scene_cut_low_;
    int cooldown_init_, cooldown_, suspect_;
    std::map<int, RawParams> raw_cache_;
    std::map<int, FrameState> state_cache_;
    std::mutex mu_;
};

// -- tone curve LUT --
static const int kLutSize = 4096;

static bool BuildToneLut(float* lut, float black, float white,
                         float strength, float protect)
{
    const float range = std::max(1e-6f, white - black);
    const float s = std::min(1.0f, std::max(0.0f, strength));
    const float p = std::min(1.0f, std::max(0.0f, protect));
    for (int i = 0; i < kLutSize; i++) {
        float f = (float)i / (kLutSize - 1);             // LUT indexed by FULL-RANGE input
        float xn2 = (f - black) / range;                 // remap once
        xn2 = std::min(1.0f, std::max(0.0f, xn2));
        float y = xn2 * (1.0f + s * (1.0f - xn2));
        float hwt = p * smoothstep01((y - 0.7f) / 0.3f);
        y = powf(y, 1.0f + hwt);
        lut[i] = std::min(1.0f, std::max(0.0f, y));
    }
    // SS10.6: monotonicity assertion — on violation, fallback identity + log
    for (int i = 1; i < kLutSize; i++) {
        if (lut[i] < lut[i - 1] - 1e-6f) {
            fprintf(stderr, "HDRAGCNext: NON-MONOTONIC LUT at %d (%f < %f) -> identity fallback\n",
                    i, lut[i], lut[i - 1]);
            for (int j = 0; j < kLutSize; j++) lut[j] = (float)j / (kLutSize - 1);
            return false;
        }
    }
    return true;
}

// -- luma path: stats -> LUT -> apply (with depth conversion) --
static void ApplyToneCurve(const uint8_t* sp, int spitch, uint8_t* dp, int dpitch,
                           int rowbytes, int h, int sbits, int dbits,
                           const float* lut, DitherMode dm, const float* blue)
{
    const int selem = (sbits == 8) ? 1 : 2;
    const int elems = rowbytes / selem;
    const float smax = (float)((1 << sbits) - 1);
    const float dmax = (float)((1 << dbits) - 1);
    const bool dither = (dm != DITHER_NONE) && (dbits < sbits);
    for (int y = 0; y < h; y++) {
        const uint8_t* srow = sp + (size_t)y * spitch;
        uint8_t* drow = dp + (size_t)y * dpitch;
        for (int x = 0; x < elems; x++) {
            float f = (sbits == 8) ? (float)srow[x] / smax
                                   : (float)((const uint16_t*)srow)[x] / smax;
            // linear interp: grid quantization +-0.5 step (up to +-8 lsb @16bit)
            // becomes exact for linear ramps (identity round-trip stays lossless)
            float pos = f * (float)(kLutSize - 1);
            int i0 = (int)pos;
            if (i0 > kLutSize - 2) i0 = kLutSize - 2;
            if (i0 < 0) i0 = 0;
            float frac = pos - (float)i0;
            float yl = lut[i0] + frac * (lut[i0 + 1] - lut[i0]);
            double v = (double)yl * (double)dmax;
            if (dither) v += (double)DitherLsb(x, y, dm, blue);
            long iv = lrint(v);
            if (iv < 0) iv = 0;
            if (iv > (long)dmax) iv = (long)dmax;
            if (dbits == 8) drow[x] = (uint8_t)iv;
            else            ((uint16_t*)drow)[x] = (uint16_t)iv;
        }
    }
}

// ---- M6 chroma: luma-ratio saturation + hue-preserving soft clamp (SS16) ----
enum ChromaMode { CHROMA_SIMPLE = 0, CHROMA_LUMA_RATIO = 1 };

// r field: (Y_out_base+eps)/(Y_in_base+eps) clamped; upsample to target res.
static void UpsampleTo(const Buffer& s, Buffer& dst, int dw, int dh)
{
    Buffer cur = s;
    while (cur.w < dw || cur.h < dh) {
        Buffer nx;
        int tw = cur.w * 2 < dw ? cur.w * 2 : dw;
        int th = cur.h * 2 < dh ? cur.h * 2 : dh;
        Upsample2x(cur, nx, tw, th);
        cur = nx;
    }
    dst = cur;
}

static void ProcessChromaLR(const PVideoFrame& srcf, const PVideoFrame& dstf,
                            const Buffer& yin_base, const Buffer& yout_base,
                            const Buffer& yout_full, int sbits, int dbits,
                            ChromaMode mode, float g_sat, float mix, float softknee,
                            DitherMode dm, const float* blue)
{
    const int cw = srcf->GetRowSize(PLANAR_U) / ((sbits == 8) ? 1 : 2);
    const int ch = srcf->GetHeight(PLANAR_U);
    const int W = yout_full.w, H = yout_full.h;
    const float smax = (float)((1 << sbits) - 1);
    const float dmax = (float)((1 << dbits) - 1);
    const float s_neutral = smax * 0.5f, d_neutral = dmax * 0.5f;
    const float eps = 16.0f / 65535.0f;
    const bool dither = (dm != DITHER_NONE) && (dbits < sbits);

    // rasio luma lokal -> resolusi chroma
    Buffer rup;
    if (mode == CHROMA_LUMA_RATIO) {
        Buffer rbase;
        rbase.alloc(yin_base.w, yin_base.h);
        for (size_t j = 0; j < rbase.d.size(); j++) {
            float r = (yout_base.d[j] + eps) / (yin_base.d[j] + eps);
            rbase.d[j] = std::min(1.5f, std::max(0.8f, r));
        }
        UpsampleTo(rbase, rup, cw, ch);
    }

    const uint8_t* su = srcf->GetReadPtr(PLANAR_U);
    const uint8_t* sv = srcf->GetReadPtr(PLANAR_V);
    const int spu = srcf->GetPitch(PLANAR_U), spv = srcf->GetPitch(PLANAR_V);
    uint8_t* du = dstf->GetWritePtr(PLANAR_U);
    uint8_t* dv = dstf->GetWritePtr(PLANAR_V);
    const int dpu = dstf->GetPitch(PLANAR_U), dpv = dstf->GetPitch(PLANAR_V);

    for (int y = 0; y < ch; y++) {
        const uint8_t* ur = su + (size_t)y * spu;
        const uint8_t* vr = sv + (size_t)y * spv;
        uint8_t* uo = du + (size_t)y * dpu;
        uint8_t* vo = dv + (size_t)y * dpv;
        int ly = (int)((double)y * H / ch); if (ly >= H) ly = H - 1;
        for (int x = 0; x < cw; x++) {
            float uin = (sbits == 8) ? (float)ur[x] : (float)((const uint16_t*)ur)[x];
            float vin = (sbits == 8) ? (float)vr[x] : (float)((const uint16_t*)vr)[x];
            // normalized offset in FULL-SCALE fractions: full saturation per
            // component = 0.5 (doc SS16 units; m_max formula assumes this)
            float dun = (uin - s_neutral) / smax;
            float dvn = (vin - s_neutral) / smax;
            float m = sqrtf(dun * dun + dvn * dvn);
            float g = g_sat;
            if (mode == CHROMA_LUMA_RATIO)
                g *= (1.0f - mix + mix * rup.d[(size_t)y * cw + x]);
            float mun = m * g;
            if (mode == CHROMA_LUMA_RATIO && mun > 1e-6f) {
                // hue-preserving soft clamp (SS16.4)
                int lx = (int)((double)x * W / cw); if (lx >= W) lx = W - 1;
                float yy = yout_full.d[(size_t)ly * W + lx];
                float m_max = 1.2f * (yy < 1.0f - yy ? yy : 1.0f - yy);  // normalized headroom
                if (m_max < 0.05f) m_max = 0.05f;
                float knee = softknee * m_max;
                if (mun > knee) {
                    float excess = mun - knee;
                    float room = m_max - knee;
                    mun = knee + excess * room / (excess + room);
                }
                if (mun > m_max) mun = m_max;
            } else if (mode == CHROMA_SIMPLE) {
                if (mun > 0.5f) mun = 0.5f;   // legacy hard clamp at full swing
            }
            float scale = (m > 1e-6f) ? (mun / m) : 0.0f;
            double uo_v = (double)d_neutral + (double)(dun * scale) * dmax;
            double vo_v = (double)d_neutral + (double)(dvn * scale) * dmax;
            if (dither) {
                float b = DitherLsb(x, y, dm, blue);
                uo_v += b; vo_v += b;
            }
            long iu = lrint(uo_v), iv2 = lrint(vo_v);
            if (iu < 0) iu = 0; if (iu > (long)dmax) iu = (long)dmax;
            if (iv2 < 0) iv2 = 0; if (iv2 > (long)dmax) iv2 = (long)dmax;
            if (dbits == 8) { uo[x] = (uint8_t)iu; vo[x] = (uint8_t)iv2; }
            else { ((uint16_t*)uo)[x] = (uint16_t)iu; ((uint16_t*)vo)[x] = (uint16_t)iv2; }
        }
    }
}

enum ShowMode { SHOW_NONE = 0, SHOW_BASE = 1, SHOW_MASK = 2, SHOW_LIFT = 3 };

// dither offset +-0.5 lsb target: blue tile (64x64) or Bayer 8x8

class HDRAGCNext : public GenericVideoFilter
{
    VideoInfo out_vi_;
    int out_bits_, src_bits_;
    DitherMode dither_;
    float strength_, protect_, black_, white_;
    bool auto_points_;
    int levels_;
    float local_mix_, shadow_threshold_, mask_gamma_;
    float saturation_;
    bool auto_saturation_;
    ChromaMode chroma_mode_;
    float luma_ratio_mix_, chroma_softknee_;
    TemporalAnalyzer* analyzer_;
    int temporal_mode_;
    int temporal_radius_;
    int show_;
    float natural_;      // anti-fauxHDR: 0 = off, 1 = natural penuh
    float blue64_[64][64];      // tiled blue noise (void-and-cluster, M7)
    float lut_buf_[kLutSize];   // reused per frame (M7 buffer reuse)

public:
    HDRAGCNext(PClip child, float strength, float protect, float black, float white,
               bool auto_points, int levels, float local_mix, float shadow_threshold,
               float mask_gamma, int output_bits, const char* dither,
               float temporal, int temporal_radius, int temporal_mode,
               float scene_cut, float scene_cut_low, int scene_cooldown,
               float saturation, bool auto_saturation, const char* chroma_mode,
               float luma_ratio_mix, float chroma_softknee, const char* show,
               float natural,
               IScriptEnvironment* env)
        : GenericVideoFilter(child), out_bits_(0), src_bits_(0), dither_(DITHER_NONE),
          strength_(strength), protect_(protect), black_(black), white_(white),
          auto_points_(auto_points), levels_(levels), local_mix_(local_mix),
          shadow_threshold_(shadow_threshold), mask_gamma_(mask_gamma),
          saturation_(saturation), auto_saturation_(auto_saturation),
          chroma_mode_(!strcmp(chroma_mode ? chroma_mode : "luma_ratio", "simple")
                        ? CHROMA_SIMPLE : CHROMA_LUMA_RATIO),
          luma_ratio_mix_(luma_ratio_mix), chroma_softknee_(chroma_softknee),
          show_(SHOW_NONE), natural_(std::min(1.0f, std::max(0.0f, natural))),
          analyzer_(nullptr), temporal_mode_(temporal_mode),
          temporal_radius_(temporal_radius)
    {
        if (!vi.IsPlanar() || !vi.IsYUV())
            env->ThrowError("HDRAGCNext: input must be planar YUV.");

        const char* dname = dither ? dither : "blue";
        if      (!strcmp(dname, "none"))    dither_ = DITHER_NONE;
        else if (!strcmp(dname, "ordered")) dither_ = DITHER_ORDERED;
        else if (!strcmp(dname, "blue"))    dither_ = DITHER_BLUE; // TODO: real blue noise
        else env->ThrowError("HDRAGCNext: dither must be \"none\"|\"ordered\"|\"blue\".");

        src_bits_ = vi.BitsPerComponent();  // BEFORE vi overwrite
        out_vi_ = vi;
        int bits = (output_bits == 0) ? src_bits_ : output_bits;
        int flag;
        switch (bits) {
            case 8:  flag = VideoInfo::CS_Sample_Bits_8;  break;
            case 10: flag = VideoInfo::CS_Sample_Bits_10; break;
            case 12: flag = VideoInfo::CS_Sample_Bits_12; break;
            case 14: flag = VideoInfo::CS_Sample_Bits_14; break;
            case 16: flag = VideoInfo::CS_Sample_Bits_16; break;
            default: env->ThrowError("HDRAGCNext: output_bits must be 0, 8, 10, 12, 14, or 16.");
        }
        out_vi_.pixel_type = (vi.pixel_type & ~VideoInfo::CS_Sample_Bits_Mask) | flag;
        out_bits_ = bits;
        vi = out_vi_;  // GenericVideoFilter exposes base `vi` via GetVideoInfo()

        const char* sh = show ? show : "none";
        if      (!strcmp(sh, "base")) show_ = SHOW_BASE;
        else if (!strcmp(sh, "mask")) show_ = SHOW_MASK;
        else if (!strcmp(sh, "lift")) show_ = SHOW_LIFT;
        else show_ = SHOW_NONE;
        InitBlueNoise();

        analyzer_ = new TemporalAnalyzer(child, src_bits_, auto_points_,
                                         black_, white_, shadow_threshold_,
                                         temporal, temporal_radius, temporal_mode,
                                         scene_cut, scene_cut_low, scene_cooldown);
    }

    ~HDRAGCNext() override { delete analyzer_; }

    // CACHE_GENERIC = LRU up to X frames (3.7.5); returns int per IClip.
    int __stdcall SetCacheHints(int cachehints, int frame_range) override
    {
        if (cachehints == CACHE_GENERIC)
            frame_range = temporal_radius_ * 2 + 2;
        return 0;
    }


    void InitBlueNoise()
    {
        static const float (*tile)[64] = []() {
            static float t[64][64];
            uint32_t rng = 0x12345678u;
            auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / float(1 << 24); };
            float r[64][64];
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) r[y][x] = rnd();
            // 5x5 gaussian sigma~1.5 (separable [1,4,7,4,1]/21-ish), highpass energy
            static const float k5[5] = { 0.061f, 0.242f, 0.383f, 0.242f, 0.061f };
            auto clampi = [](int v) { return v < 0 ? 0 : (v > 63 ? 63 : v); };
            auto hp = [&](int y, int x) {
                float gx = 0.f;
                for (int j = -2; j <= 2; j++) {
                    float gy = 0.f;
                    for (int i = -2; i <= 2; i++)
                        gy += k5[i + 2] * r[clampi(y + j)][clampi(x + i)];
                    gx += k5[j + 2] * gy;
                }
                return r[y][x] - gx;   // high-pass
            };
            // void-and-cluster lite: swap max/min energy pixels, 160 iters
            for (int it = 0; it < 160; it++) {
                float emax = -1e9f, emin = 1e9f; int ax = 0, ay = 0, ix = 0, iy = 0;
                for (int y = 0; y < 64; y += 2) for (int x = 0; x < 64; x += 2) {
                    float e = hp(y, x);
                    if (e > emax) { emax = e; ax = x; ay = y; }
                    if (e < emin) { emin = e; ix = x; iy = y; }
                }
                float tmp = r[ay][ax]; r[ay][ax] = r[iy][ix]; r[iy][ix] = tmp;
            }
            // normalize ranks ke [0,1]
            float mn = 1e9f, mx = -1e9f;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                if (r[y][x] < mn) mn = r[y][x];
                if (r[y][x] > mx) mx = r[y][x];
            }
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
                t[y][x] = (r[y][x] - mn) / (mx - mn);
            return t;
        }();
        memcpy(blue64_, tile, sizeof(blue64_));
    }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override
    {
        PVideoFrame src = child->GetFrame(n, env);
        PVideoFrame dst = env->NewVideoFrame(out_vi_);

        // tone params: temporal-smoothed state (M5); falls back to raw on init
        FrameState st = analyzer_->GetState(n, env);
        float black = st.black, white = st.white, tau_state = st.tau;
        // degenerate/fixed full-range: white<=black means "no remapping"
        if (white - black < 1e-4f) { black = 0.0f; white = 1.0f; }
        const float* lut = lut_buf_;   // member, reused per frame (M7)
        const bool lut_ok = BuildToneLut(lut_buf_, black, white, strength_, protect_);
        (void)lut_ok;

        if (strength_ <= 0.0f) {
            // exact path (M3): identity LUT + linear interp = lossless round-trip
            ApplyToneCurve(src->GetReadPtr(PLANAR_Y),  src->GetPitch(PLANAR_Y),
                           dst->GetWritePtr(PLANAR_Y), dst->GetPitch(PLANAR_Y),
                           src->GetRowSize(PLANAR_Y),  src->GetHeight(PLANAR_Y),
                           src_bits_, out_bits_, lut, dither_, (const float*)blue64_);
        } else {
            // ---- M4: Laplacian pyramid + spatially-adaptive shadow gain ----
            const int W = src->GetRowSize(PLANAR_Y) / ((src_bits_ == 8) ? 1 : 2);
            const int H = src->GetHeight(PLANAR_Y);
            const float smax = (float)((1 << src_bits_) - 1);
            const uint8_t* sp = src->GetReadPtr(PLANAR_Y);
            const int spitch = src->GetPitch(PLANAR_Y);

            Buffer lvl[8];  // up to 7 levels
            int L = 0;
            lvl[0].alloc(W, H);
            for (int y = 0; y < H; y++) {
                const uint8_t* row = sp + (size_t)y * spitch;
                if (src_bits_ == 8)
                    for (int x = 0; x < W; x++) lvl[0].d[(size_t)y*W + x] = row[x] / smax;
                else {
                    const uint16_t* r16 = (const uint16_t*)row;
                    for (int x = 0; x < W; x++) lvl[0].d[(size_t)y*W + x] = r16[x] / smax;
                }
            }
            // LEVELS ADAPTIF (fix haze): base minimal ~1/8 dimensi terkecil.
            // Frame kecil dgn levels=4 -> base 22x14 -> upsample 4x = kabut.
            int min_dim = (W < H ? W : H);
            int want = std::min(6, std::max(2, levels_));
            int max_levels_dim = 1;
            while ((min_dim >> (max_levels_dim + 1)) >= 24 && max_levels_dim < 6)
                max_levels_dim++;
            if (want > max_levels_dim) want = max_levels_dim;
            while (L < want && lvl[L].w > 16 && lvl[L].h > 16) {
                Downsample2x(lvl[L], lvl[L+1]);
                L++;
            }

            // Laplacian details
            std::vector<Buffer> det(L);
            for (int i = 0; i < L; i++)
                Upsample2x(lvl[i+1], det[i], lvl[i].w, lvl[i].h);
            for (int i = 0; i < L; i++)
                for (size_t j = 0; j < det[i].d.size(); j++)
                    det[i].d[j] = lvl[i].d[j] - det[i].d[j];

            // shadow mask dari base (level L) — SS10.5
            float tau = tau_state;
            // konten gelap (auto) -> mask_gamma efektif 1.2 (anti haze + recovery)
            float g = std::max(0.1f, (st.dark && mask_gamma_ > 1.2f) ? 1.2f : mask_gamma_);
            if (natural_ > 0.0f)
                g = g + (2.5f - g) * natural_ * 0.6f;   // konsentrasi ke shadow terdalam
            Buffer& base = lvl[L];
            const size_t bn = base.d.size();
            std::vector<float> mask_eff(bn);
            for (size_t j = 0; j < bn; j++) {
                float t = (tau - base.d[j]) / tau;
                t = std::min(1.0f, std::max(0.0f, t));
                float m = powf(t, g);
                mask_eff[j] = 1.0f - local_mix_ + local_mix_ * m;
            }

            // salin base sbg Y_in untuk rasio luma chroma (SS16.3)
            Buffer yin_base;
            yin_base.alloc(base.w, base.h);
            yin_base.d = base.d;

                double mm = 0; for (size_t j = 0; j < bn; j++) mm += mask_eff[j];
                double bm = 0; for (size_t j = 0; j < bn; j++) bm += yin_base.d[j];
            // tone map base: lift positif termodulasi mask, negatif penuh (SS10.5.3)
            for (size_t j = 0; j < bn; j++) {
                float f = base.d[j];
                int i0 = (int)(f * (kLutSize - 1));
                if (i0 > kLutSize - 2) i0 = kLutSize - 2;
                if (i0 < 0) i0 = 0;
                float frac = f * (kLutSize - 1) - (float)i0;
                float yg = lut[i0] + frac * (lut[i0+1] - lut[i0]);
                float lift = yg - f;
                if (lift > 0.0f) lift *= mask_eff[j];
                base.d[j] = std::min(1.0f, std::max(0.0f, f + lift));
            }

            // ---- debug show modes (SS18): render ke Y, chroma identity ----
            if (show_ != SHOW_NONE) {
                Buffer dbg;
                dbg.alloc(base.w, base.h);
                if (show_ == SHOW_MASK) {
                    for (size_t j = 0; j < bn; j++) {
                        float t = (tau - yin_base.d[j]) / tau;
                        t = std::min(1.0f, std::max(0.0f, t));
                        dbg.d[j] = powf(t, g);
                    }
                } else if (show_ == SHOW_BASE) {
                    dbg.d = base.d;
                } else { // SHOW_LIFT: lift termodulasi, divisualkan 0.5+-0.2 -> full
                    for (size_t j = 0; j < bn; j++) {
                        float lift = base.d[j] - yin_base.d[j];
                        dbg.d[j] = std::min(1.0f, std::max(0.0f, 0.5f + lift * 2.5f));
                    }
                }
                Buffer full;
                UpsampleTo(dbg, full, W, H);
                uint8_t* dp = dst->GetWritePtr(PLANAR_Y);
                const int dpitch = dst->GetPitch(PLANAR_Y);
                const float dmax = (float)((1 << out_bits_) - 1);
                for (int y = 0; y < H; y++) {
                    uint8_t* row = dp + (size_t)y * dpitch;
                    for (int x = 0; x < W; x++) {
                        long iv = lrint((double)full.d[(size_t)y*W + x] * (double)dmax);
                        if (iv < 0) iv = 0; if (iv > (long)dmax) iv = (long)dmax;
                        if (out_bits_ == 8) row[x] = (uint8_t)iv;
                        else ((uint16_t*)row)[x] = (uint16_t)iv;
                    }
                }
                for (int p = 1; p < 3; p++) {
                    const int plane = (p == 1) ? PLANAR_U : PLANAR_V;
                    ProcessPlane(src->GetReadPtr(plane),  src->GetPitch(plane),
                                 dst->GetWritePtr(plane), dst->GetPitch(plane),
                                 src->GetRowSize(plane),  src->GetHeight(plane),
                                 src_bits_, out_bits_, dither_, (const float*)blue64_);
                }
                return dst;
            }

            // reconstruct
            Buffer cur;
            cur.alloc(base.w, base.h);
            cur.d = base.d;
            for (int i = L - 1; i >= 0; i--) {
                Buffer up;
                Upsample2x(cur, up, lvl[i].w, lvl[i].h);
                for (size_t j = 0; j < up.d.size(); j++) {
                    float b = up.d[j];
                    // anti-ring GATED (fix haze): rolloff hanya utk detail BESAR
                    // (edge -> potensi ringing); detail kecil (tekstur) utuh.
                    float d = det[i].d[j];
                    float w = 1.0f;
                    if (fabsf(d) > 0.10f)
                        w = 1.0f - 0.7f * fabsf(2.0f * b - 1.0f);
                    up.d[j] += d * w;
                }
                cur = up;
            }

            // tulis Y dari cur (+dither)
            uint8_t* dp = dst->GetWritePtr(PLANAR_Y);
            const int dpitch = dst->GetPitch(PLANAR_Y);
            const float dmax = (float)((1 << out_bits_) - 1);
            const bool dither = (dither_ != DITHER_NONE) && (out_bits_ < src_bits_);
            // ANTI-FAUXHDR: batasi delta per-pixel terhadap ORIGINAL.
            // natural=1 -> lift max ~0.05 (hitam tetap dalam, tidak abu-abu total).
            const float lift_cap = 0.20f * (1.0f - 0.75f * natural_);
            for (int y = 0; y < H; y++) {
                uint8_t* row = dp + (size_t)y * dpitch;
                for (int x = 0; x < W; x++) {
                    float o = lvl[0].d[(size_t)y*W + x];
                    float vv = cur.d[(size_t)y*W + x];
                    float d = vv - o;
                    if (d > lift_cap) vv = o + lift_cap;
                    double v = (double)vv * (double)dmax;
                    if (dither) v += (double)DitherLsb(x, y, dither_, (const float*)blue64_);
                    long iv = lrint(v);
                    if (iv < 0) iv = 0;
                    if (iv > (long)dmax) iv = (long)dmax;
                    if (out_bits_ == 8) row[x] = (uint8_t)iv;
                    else ((uint16_t*)row)[x] = (uint16_t)iv;
                }
            }

            // chroma: luma-ratio + hue-preserving soft clamp (SS16)
            float avg_after = 0.f;
            for (size_t j = 0; j < cur.d.size(); j++) avg_after += cur.d[j];
            avg_after /= (float)cur.d.size();
            float g_sat = saturation_;
            if (auto_saturation_)
                g_sat *= std::min(1.5f, std::max(0.8f, 1.0f + (1.0f - avg_after) * 0.25f));
            // anti-fauxHDR: kompensasi saturasi dinetralkan (fauxHDR khas:
            // shadow terangkat + warna diboost bersamaan)
            g_sat = 1.0f + (g_sat - 1.0f) * (1.0f - natural_);
            ProcessChromaLR(src, dst, yin_base, base, cur, src_bits_, out_bits_,
                            chroma_mode_, g_sat, luma_ratio_mix_, chroma_softknee_,
                            dither_, (const float*)blue64_);
            return dst;
        }

        for (int p = 1; p < 3; p++) {
            const int plane = (p == 1) ? PLANAR_U : PLANAR_V;
            ProcessPlane(src->GetReadPtr(plane),  src->GetPitch(plane),
                         dst->GetWritePtr(plane), dst->GetPitch(plane),
                         src->GetRowSize(plane),  src->GetHeight(plane),
                         src_bits_, out_bits_, dither_, (const float*)blue64_);
        }
        return dst;
    }
};

} // namespace

static AVSValue __cdecl Create_HDRAGCNext(AVSValue args, void*, IScriptEnvironment* env)
{
    return new HDRAGCNext(args[0].AsClip(),
                          (float)args[1].AsFloat(0.8),   // strength
                          (float)args[2].AsFloat(0.8),   // protect_highlights
                          (float)args[3].AsFloat(0.0),   // black_point
                          (float)args[4].AsFloat(0.0),   // white_point
                          args[5].AsBool(true),          // auto_points
                          args[18].AsInt(4),             // levels
                          (float)args[19].AsFloat(1.0),  // local_mix
                          (float)args[20].AsFloat(0.35), // shadow_threshold
                          (float)args[21].AsFloat(2.0),  // mask_gamma
                          args[23].AsInt(0),             // output_bits
                          args[24].AsString("blue"),     // dither
                          (float)args[6].AsFloat(0.85),  // temporal
                          args[7].AsInt(4),              // temporal_radius
                          strcmp(args[8].AsString("iir"), "window") ? 0 : 1, // mode
                          (float)args[9].AsFloat(0.35),  // scene_cut
                          (float)args[10].AsFloat(0.20), // scene_cut_low
                          args[11].AsInt(3),             // scene_cooldown
                          (float)args[13].AsFloat(1.0),  // saturation
                          args[14].AsBool(true),         // auto_saturation
                          args[15].AsString("luma_ratio"), // chroma_mode
                          (float)args[16].AsFloat(0.7),  // luma_ratio_mix
                          (float)args[17].AsFloat(0.9),  // chroma_softknee
                          args[25].AsString("none"),     // show
                          (float)args[27].AsFloat(0.0),    // natural
                          env);
}

static const char* hdragc_init(IScriptEnvironment* env, const AVS_Linkage* linkage)
{
    AVS_linkage = linkage;
    env->AddFunction(
        "HDRAGCNext",
        "c"
        "[strength]f[protect_highlights]f[black_point]f[white_point]f[auto_points]b"
        "[temporal]f[temporal_radius]i[temporal_mode]s"
        "[scene_cut]f[scene_cut_low]f[scene_cooldown]i"
        "[detail_gain]f[saturation]f[auto_saturation]b"
        "[chroma_mode]s[luma_ratio_mix]f[chroma_softknee]f"
        "[levels]i[local_mix]f[shadow_threshold]f[mask_gamma]f[mask_level]i"
        "[output_bits]i[dither]s[show]s[debug]b[natural]f",
        Create_HDRAGCNext, nullptr);
    return "HDRAGCNext: adaptive shadow brightening / local tone mapping (M3)";
}

#if defined(_MSC_VER)
  #define HDRAGC_EXPORT __declspec(dllexport)
#else
  #define HDRAGC_EXPORT __attribute__((visibility("default")))
#endif

extern "C" HDRAGC_EXPORT
const char* AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* linkage)
{
    return hdragc_init(env, linkage);
}

extern "C" HDRAGC_EXPORT
const char* AvisynthPluginInit2(IScriptEnvironment* env)
{
    return hdragc_init(env, nullptr);
}
