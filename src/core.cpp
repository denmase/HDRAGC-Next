// core.cpp - HDRAGC-Next shared processing core (API-agnostic).
// Transcribed 1:1 from the pre-refactor avisynth-embedded implementation so
// that output is byte-identical (M1 gate: T1-T16 PASS + byte-identical dumps).
#include "core.h"

namespace hdragc {

const float kBayer8[8][8] = {
  { 0,32, 8,40, 2,34,10,42},{48,16,56,24,50,18,58,26},
  {12,44, 4,36,14,46, 6,38},{60,28,52,20,62,30,54,22},
  { 3,35,11,43, 1,33, 9,41},{51,19,59,27,49,17,57,25},
  {15,47, 7,39,13,45, 5,37},{63,31,55,23,61,29,53,21}
};

float DitherLsb(int x, int y, DitherMode dm, const float* blue)
{
    if (dm == DITHER_BLUE && blue) return blue[(y & 63) * 64 + (x & 63)] - 0.5f;
    return (kBayer8[y & 7][x & 7] + 0.5f) / 64.0f - 0.5f;
}

void ProcessPlane(const uint8_t* sp, int spitch, uint8_t* dp, int dpitch,
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

// ---- Laplacian pyramid helpers ----
struct Buffer { int w, h; std::vector<float> d; void alloc(int W, int H) { w=W; h=H; d.resize((size_t)W*H); } };

static void Downsample2x(const Buffer& s, Buffer& dst)
{
    const int dw = (s.w / 2) < 1 ? 1 : (s.w / 2);
    const int dh = (s.h / 2) < 1 ? 1 : (s.h / 2);
    dst.alloc(dw, dh);
    auto clampi = [](int v, int m) { return v < 0 ? 0 : (v >= m ? m - 1 : v); };
    static const float c[5] = {1,4,6,4,1};
    std::vector<float> tmp((size_t)dw * s.h);
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
            float c2 = s.d[(size_t)y1*s.w + x0], e = s.d[(size_t)y1*s.w + x1];
            dst.d[(size_t)y*dw + x] = (a + fx*(b-a)) * (1-fy) + (c2 + fx*(e-c2)) * fy;
        }
    }
}

// ---- temporal analyzer (percentile-delta scene metric + IIR) ----
static float SceneScoreV2(const RawFrameParams& a, const RawFrameParams& b)
{
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

TemporalAnalyzer::TemporalAnalyzer(FrameSrc src, int src_bits, bool auto_points,
                     float fixed_black, float fixed_white, float shadow_threshold,
                     float temporal, int radius, int mode,
                     float scene_cut, float scene_cut_low, int cooldown)
    : src_(std::move(src)), src_bits_(src_bits), auto_points_(auto_points),
      fixed_black_(fixed_black), fixed_white_(fixed_white), shadow_thr_(shadow_threshold),
      alpha_(1.0f - temporal), radius_(radius), mode_(mode),
      scene_cut_(scene_cut), scene_cut_low_(scene_cut_low),
      cooldown_init_(cooldown), cooldown_(0), suspect_(0) {}

TemporalAnalyzer::FrameState TemporalAnalyzer::Lerp(const FrameState& p, const RawFrameParams& r, float a)
{
    return FrameState{ p.black + a * (r.black - p.black),
                       p.white + a * (r.white - p.white),
                       p.tau   + a * (r.tau   - p.tau),
                       r.dark };
}

TemporalAnalyzer::FrameState TemporalAnalyzer::GetState(int n, void* ctx)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = state_cache_.find(n);
    if (it != state_cache_.end()) return it->second;

    if (mode_ == 1) {
        FrameState acc { 0, 0, 0, 0 };
        int cnt = 0;
        for (int k = n - radius_; k <= n + radius_; k++) {
            if (k < 0 || k >= src_.num_frames) continue;
            RawFrameParams r = GetRaw(k, ctx);
            acc.black += r.black; acc.white += r.white; acc.tau += r.tau;
            cnt++;
        }
        if (cnt == 0) cnt = 1;
        FrameState s { acc.black / cnt, acc.white / cnt, acc.tau / cnt, 0 };
        state_cache_[n] = s;
        return s;
    }

    int m = -1;
    if (!state_cache_.empty()) {
        auto it2 = state_cache_.lower_bound(n);
        if (it2 != state_cache_.begin()) { --it2; m = it2->first; }
    }
    const int max_gap = radius_ * 2;
    if (m < 0 || n - m > max_gap) {
        RawFrameParams r = GetRaw(n, ctx);
        FrameState s { r.black, r.white, r.tau, r.dark };
        state_cache_[n] = s;
        cooldown_ = cooldown_init_; suspect_ = 0;
        return s;
    }
    for (int i = m + 1; i <= n; i++) {
        RawFrameParams r  = GetRaw(i, ctx);
        RawFrameParams rp = GetRaw(i - 1, ctx);
        r.score = SceneScoreV2(rp, r);
        const FrameState& prev = state_cache_[i - 1];
        FrameState cur;
        if (cooldown_ > 0) cooldown_--;
        if (r.score >= scene_cut_) {
            cur = FrameState{ r.black, r.white, r.tau, r.dark };
            cooldown_ = cooldown_init_; suspect_ = 0;
        } else if (r.score >= scene_cut_low_) {
            if (suspect_) {
                cur = FrameState{ r.black, r.white, r.tau, r.dark };
                cooldown_ = cooldown_init_; suspect_ = 0;
            } else {
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

RawFrameParams TemporalAnalyzer::GetRaw(int n, void* ctx)
{
    auto it = raw_cache_.find(n);
    if (it != raw_cache_.end()) return it->second;
    PlaneView yv { nullptr, 0, 0, 0, 0 };
    void* token = src_.fetch(n, yv, ctx);
    const uint8_t* p = yv.data;
    const int pitch = yv.stride;
    const int w = yv.w;
    const int h = yv.h;
    const int sbits = yv.bits;
    const int nbins = 1 << sbits;
    std::vector<uint32_t> hist(nbins, 0);
    for (int yy = 0; yy < h; yy++) {
        const uint8_t* row = p + (size_t)yy * pitch;
        if (sbits == 8) for (int x = 0; x < w; x++) hist[row[x]]++;
        else { const uint16_t* r16 = (const uint16_t*)row;
               for (int x = 0; x < w; x++) hist[std::min(r16[x], (uint16_t)(nbins-1))]++; }
    }
    const uint64_t total = (uint64_t)w * h;
    auto pct = [&](double q) -> float {
        uint64_t target = (uint64_t)(total * q), cum = 0;
        for (int i = 0; i < nbins; i++) { cum += hist[i]; if (cum >= target) return (float)i / (nbins - 1); }
        return 1.0f;
    };
    RawFrameParams r;
    static const double qs[5] = { 0.01, 0.25, 0.50, 0.75, 0.99 };
    for (int i = 0; i < 5; i++) r.p[i] = pct(qs[i]);
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
    src_.release(token);
    return r;
}

// ---- chroma ----
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

static void ProcessChromaLR(const PlaneView& suv, const PlaneView& svv,
                            WritablePlaneView& du, WritablePlaneView& dv,
                            const Buffer& yin_base, const Buffer& yout_base,
                            const Buffer& yout_full, int sbits, int dbits,
                            ChromaMode mode, float g_sat, float mix, float softknee,
                            DitherMode dm, const float* blue)
{
    const int cw = suv.w, ch = suv.h;
    const int W = yout_full.w, H = yout_full.h;
    const float smax = (float)((1 << sbits) - 1);
    const float dmax = (float)((1 << dbits) - 1);
    const float s_neutral = smax * 0.5f, d_neutral = dmax * 0.5f;
    const float eps = 16.0f / 65535.0f;
    const bool dither = (dm != DITHER_NONE) && (dbits < sbits);

    // CAST-PRESERVING saturation (fix tint kekuningan, temuan #12): boost hanya
    // komponen AC chroma; cast DC scene (rata-rata offset) dipertahankan penuh.
    double cast_du = 0.0, cast_dv = 0.0;
    for (int y = 0; y < ch; y++) {
        const uint8_t* ur = suv.data + (size_t)y * suv.stride;
        const uint8_t* vr = svv.data + (size_t)y * svv.stride;
        for (int x = 0; x < cw; x++) {
            float uin = (sbits == 8) ? (float)ur[x] : (float)((const uint16_t*)ur)[x];
            float vin = (sbits == 8) ? (float)vr[x] : (float)((const uint16_t*)vr)[x];
            cast_du += (uin - s_neutral) / smax;
            cast_dv += (vin - s_neutral) / smax;
        }
    }
    cast_du /= (double)(cw * ch);
    cast_dv /= (double)(cw * ch);

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

    for (int y = 0; y < ch; y++) {
        const uint8_t* ur = suv.data + (size_t)y * suv.stride;
        const uint8_t* vr = svv.data + (size_t)y * svv.stride;
        uint8_t* uo = du.data + (size_t)y * du.stride;
        uint8_t* vo = dv.data + (size_t)y * dv.stride;
        int ly = (int)((double)y * H / ch); if (ly >= H) ly = H - 1;
        for (int x = 0; x < cw; x++) {
            float uin = (sbits == 8) ? (float)ur[x] : (float)((const uint16_t*)ur)[x];
            float vin = (sbits == 8) ? (float)vr[x] : (float)((const uint16_t*)vr)[x];
            float dun = (uin - s_neutral) / smax - (float)cast_du;   // AC only
            float dvn = (vin - s_neutral) / smax - (float)cast_dv;   // AC only
            float m = sqrtf(dun * dun + dvn * dvn);
            float g = g_sat;
            if (mode == CHROMA_LUMA_RATIO)
                g *= (1.0f - mix + mix * rup.d[(size_t)y * cw + x]);
            float mun = m * g;
            if (mode == CHROMA_LUMA_RATIO && mun > 1e-6f) {
                int lx = (int)((double)x * W / cw); if (lx >= W) lx = W - 1;
                float yy = yout_full.d[(size_t)ly * W + lx];
                float m_max = 1.2f * (yy < 1.0f - yy ? yy : 1.0f - yy);
                if (m_max < 0.05f) m_max = 0.05f;
                float knee = softknee * m_max;
                if (mun > knee) {
                    float excess = mun - knee;
                    float room = m_max - knee;
                    mun = knee + excess * room / (excess + room);
                }
                if (mun > m_max) mun = m_max;
            } else if (mode == CHROMA_SIMPLE) {
                if (mun > 0.5f) mun = 0.5f;
            }
            float scale = (m > 1e-6f) ? (mun / m) : 0.0f;
            double uo_v = (double)d_neutral + ((double)cast_du + (double)(dun * scale)) * dmax;
            double vo_v = (double)d_neutral + ((double)cast_dv + (double)(dvn * scale)) * dmax;
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

// ---- tone LUT ----
static const int kLutSize = 4096;

static bool BuildToneLut(float* lut, float black, float white,
                         float strength, float protect)
{
    const float range = std::max(1e-6f, white - black);
    const float s = std::min(1.0f, std::max(0.0f, strength));
    const float p = std::min(1.0f, std::max(0.0f, protect));
    for (int i = 0; i < kLutSize; i++) {
        float f = (float)i / (kLutSize - 1);
        float xn2 = (f - black) / range;
        xn2 = std::min(1.0f, std::max(0.0f, xn2));
        float y = xn2 * (1.0f + s * (1.0f - xn2));
        float t = (y - 0.7f) / 0.3f;
        t = std::min(1.0f, std::max(0.0f, t));
        float ss = t * t * (3.0f - 2.0f * t);
        y = powf(y, 1.0f + p * ss);
        lut[i] = std::min(1.0f, std::max(0.0f, y));
    }
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

void ApplyToneCurve(const uint8_t* sp, int spitch, uint8_t* dp, int dpitch,
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

// ---- HDRAGCCore ----
static void InitBlueNoiseImpl(float (*tile)[64])
{
    uint32_t rng = 0x12345678u;
    auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / float(1 << 24); };
    float r[64][64];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) r[y][x] = rnd();
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
        return r[y][x] - gx;
    };
    for (int it = 0; it < 160; it++) {
        float emax = -1e9f, emin = 1e9f; int ax = 0, ay = 0, ix = 0, iy = 0;
        for (int y = 0; y < 64; y += 2) for (int x = 0; x < 64; x += 2) {
            float e = hp(y, x);
            if (e > emax) { emax = e; ax = x; ay = y; }
            if (e < emin) { emin = e; ix = x; iy = y; }
        }
        float tmp = r[ay][ax]; r[ay][ax] = r[iy][ix]; r[iy][ix] = tmp;
    }
    float mn = 1e9f, mx = -1e9f;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        if (r[y][x] < mn) mn = r[y][x];
        if (r[y][x] > mx) mx = r[y][x];
    }
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
        tile[y][x] = (r[y][x] - mn) / (mx - mn);
}

HDRAGCCore::HDRAGCCore(const Params& p, FrameSrc src, int src_bits)
    : p_(p), src_(std::move(src)), src_bits_(src_bits),
      dither_(p.dither == 0 ? DITHER_NONE : p.dither == 1 ? DITHER_ORDERED : DITHER_BLUE),
      analyzer_(src_, src_bits_, p.auto_points, p.black, p.white, p.shadow_threshold,
                p.temporal, p.temporal_radius, p.temporal_mode,
                p.scene_cut, p.scene_cut_low, p.scene_cooldown)
{
    static float tile[64][64];
    static bool once = [](){ InitBlueNoiseImpl(tile); return true; }();
    (void)once;
    memcpy(blue64_, tile, sizeof(blue64_));
}

HDRAGCCore::~HDRAGCCore() = default;

void HDRAGCCore::processFrame(const PlaneView yuv_in[3], WritablePlaneView out[3], int n, void* ctx)
{
    const int out_bits = p_.output_bits == 0 ? src_bits_ : p_.output_bits;
    const PlaneView& sy = yuv_in[0];
    const PlaneView& su = yuv_in[1];
    const PlaneView& sv = yuv_in[2];
    WritablePlaneView& dy = out[0];
    WritablePlaneView& du = out[1];
    WritablePlaneView& dv = out[2];
    const float* lut = lut_buf_;
    const float* blue = (const float*)blue64_;

    TemporalAnalyzer::FrameState st = analyzer_.GetState(n, ctx);
    float black = st.black, white = st.white, tau_state = st.tau;
    if (white - black < 1e-4f) { black = 0.0f; white = 1.0f; }
    const bool lut_ok = BuildToneLut(lut_buf_, black, white, p_.strength, p_.protect);
    (void)lut_ok;

    if (p_.strength <= 0.0f) {
        ApplyToneCurve(sy.data, sy.stride, dy.data, dy.stride,
                       sy.w * ((src_bits_ == 8) ? 1 : 2), sy.h, src_bits_, out_bits,
                       lut, dither_, blue);
        ProcessPlane(su.data, su.stride, du.data, du.stride,
                     su.w * ((src_bits_ == 8) ? 1 : 2), su.h, src_bits_, out_bits,
                     dither_, blue);
        ProcessPlane(sv.data, sv.stride, dv.data, dv.stride,
                     sv.w * ((src_bits_ == 8) ? 1 : 2), sv.h, src_bits_, out_bits,
                     dither_, blue);
        return;
    }

    // ---- pyramid path ----
    const int W = sy.w, H = sy.h;
    const float smax = (float)((1 << src_bits_) - 1);

    Buffer lvl[8];
    int L = 0;
    lvl[0].alloc(W, H);
    for (int y = 0; y < H; y++) {
        const uint8_t* row = sy.data + (size_t)y * sy.stride;
        if (src_bits_ == 8)
            for (int x = 0; x < W; x++) lvl[0].d[(size_t)y*W + x] = row[x] / smax;
        else {
            const uint16_t* r16 = (const uint16_t*)row;
            for (int x = 0; x < W; x++) lvl[0].d[(size_t)y*W + x] = r16[x] / smax;
        }
    }
    const int min_dim = (W < H ? W : H);
    int want = std::min(6, std::max(2, p_.levels));
    int max_levels_dim = 1;
    while ((min_dim >> (max_levels_dim + 1)) >= 24 && max_levels_dim < 6)
        max_levels_dim++;
    if (want > max_levels_dim) want = max_levels_dim;
    while (L < want && lvl[L].w > 16 && lvl[L].h > 16) {
        Downsample2x(lvl[L], lvl[L+1]);
        L++;
    }

    std::vector<Buffer> det(L);
    for (int i = 0; i < L; i++)
        Upsample2x(lvl[i+1], det[i], lvl[i].w, lvl[i].h);
    for (int i = 0; i < L; i++)
        for (size_t j = 0; j < det[i].d.size(); j++)
            det[i].d[j] = lvl[i].d[j] - det[i].d[j];

    const float gmask = std::max(0.1f, (st.dark && p_.mask_gamma > 1.2f) ? 1.2f : p_.mask_gamma);
    Buffer& base = lvl[L];
    const size_t bn = base.d.size();
    std::vector<float> mask_eff(bn);
    const float tau = tau_state;
    for (size_t j = 0; j < bn; j++) {
        float t = (tau - base.d[j]) / tau;
        t = std::min(1.0f, std::max(0.0f, t));
        float m = powf(t, gmask);
        mask_eff[j] = 1.0f - p_.local_mix + p_.local_mix * m;
    }
    // salin base sbg Y_in untuk rasio luma chroma (SS16.3)
    Buffer yin_base;
    yin_base.alloc(base.w, base.h);
    yin_base.d = base.d;

    // tone map base
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

    // VEIL SUPPRESSION (anti-haze)
    if (p_.veil > 0.0f) {
        Buffer d, dl, dh;
        d.alloc(base.w, base.h);
        for (size_t j = 0; j < bn; j++) d.d[j] = base.d[j] - yin_base.d[j];
        Downsample2x(d, dl);
        Upsample2x(dl, dh, d.w, d.h);
        for (size_t j = 0; j < bn; j++) {
            float delta2 = d.d[j] - p_.veil * dh.d[j];
            base.d[j] = std::min(1.0f, std::max(0.0f, yin_base.d[j] + delta2));
        }
    }

    // debug show modes
    if (p_.show != SHOW_NONE) {
        Buffer dbg;
        dbg.alloc(base.w, base.h);
        if (p_.show == SHOW_MASK) {
            for (size_t j = 0; j < bn; j++) {
                float t = (tau - yin_base.d[j]) / tau;
                t = std::min(1.0f, std::max(0.0f, t));
                dbg.d[j] = powf(t, gmask);
            }
        } else if (p_.show == SHOW_BASE) {
            dbg.d = base.d;
        } else {
            for (size_t j = 0; j < bn; j++) {
                float lift = base.d[j] - yin_base.d[j];
                dbg.d[j] = std::min(1.0f, std::max(0.0f, 0.5f + lift * 2.5f));
            }
        }
        Buffer full;
        UpsampleTo(dbg, full, W, H);
        const float dmax = (float)((1 << out_bits) - 1);
        for (int y = 0; y < H; y++) {
            uint8_t* row = dy.data + (size_t)y * dy.stride;
            for (int x = 0; x < W; x++) {
                long iv = lrint((double)full.d[(size_t)y*W + x] * (double)dmax);
                if (iv < 0) iv = 0; if (iv > (long)dmax) iv = (long)dmax;
                if (out_bits == 8) row[x] = (uint8_t)iv;
                else ((uint16_t*)row)[x] = (uint16_t)iv;
            }
        }
        ProcessPlane(su.data, su.stride, du.data, du.stride, su.w * ((src_bits_==8)?1:2), su.h, src_bits_, out_bits, dither_, blue);
        ProcessPlane(sv.data, sv.stride, dv.data, dv.stride, sv.w * ((src_bits_==8)?1:2), sv.h, src_bits_, out_bits, dither_, blue);
        return;
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
            float w2 = 1.0f;
            float dd = det[i].d[j];
            if (fabsf(dd) > 0.10f)
                w2 = 1.0f - 0.7f * fabsf(2.0f * b - 1.0f);
            up.d[j] += dd * w2;
        }
        cur = up;
    }

    // write Y
    {
        const float dmax = (float)((1 << out_bits) - 1);
        const bool dither = (dither_ != DITHER_NONE) && (out_bits < src_bits_);
        const float lift_cap = 0.20f * (1.0f - 0.75f * p_.natural);
        for (int y = 0; y < H; y++) {
            uint8_t* row = dy.data + (size_t)y * dy.stride;
            for (int x = 0; x < W; x++) {
                float o = lvl[0].d[(size_t)y*W + x];
                float vv = cur.d[(size_t)y*W + x];
                float dd = vv - o;
                if (dd > lift_cap) vv = o + lift_cap;
                double v = (double)vv * (double)dmax;
                if (dither) v += (double)DitherLsb(x, y, dither_, blue);
                long iv = lrint(v);
                if (iv < 0) iv = 0;
                if (iv > (long)dmax) iv = (long)dmax;
                if (out_bits == 8) row[x] = (uint8_t)iv;
                else ((uint16_t*)row)[x] = (uint16_t)iv;
            }
        }
    }

    // chroma
    float avg_after = 0.f;
    for (size_t j = 0; j < cur.d.size(); j++) avg_after += cur.d[j];
    avg_after /= (float)cur.d.size();
    float g_sat = p_.saturation;
    if (p_.auto_saturation)
        g_sat *= std::min(1.5f, std::max(0.8f, 1.0f + (1.0f - avg_after) * 0.25f));
    g_sat = 1.0f + (g_sat - 1.0f) * (1.0f - p_.natural);

    PlaneView suv { su.data, su.stride, su.w, su.h, src_bits_ };
    PlaneView svv { sv.data, sv.stride, sv.w, sv.h, src_bits_ };
    ProcessChromaLR(suv, svv, du, dv, yin_base, base, cur, src_bits_, out_bits,
                    (ChromaMode)p_.chroma_mode, g_sat, p_.luma_ratio_mix,
                    p_.chroma_softknee, dither_, blue);
}

} // namespace hdragc
