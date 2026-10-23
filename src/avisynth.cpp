// avisynth.cpp - AviSynth+ adapter (thin shell over hdragc core).
#include <avisynth.h>
#include "core.h"
#include <cstring>
#include <mutex>

#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
const AVS_Linkage* AVS_linkage = nullptr;

namespace {

class HDRAGCNext : public GenericVideoFilter
{
    hdragc::Params params_;
    hdragc::HDRAGCCore* core_;
    int src_bits_;
    int out_bits_;
    int temporal_radius_;

public:
    HDRAGCNext(PClip child, const hdragc::Params& p, IScriptEnvironment* env)
        : GenericVideoFilter(child), params_(p), core_(nullptr),
          temporal_radius_(p.temporal_radius)
    {
        if (!vi.IsPlanar() || !vi.IsYUV())
            env->ThrowError("HDRAGCNext: input must be planar YUV.");

        src_bits_ = vi.BitsPerComponent();   // BEFORE vi overwrite

        VideoInfo out_vi = vi;
        int bits = (p.output_bits == 0) ? src_bits_ : p.output_bits;
        int flag;
        switch (bits) {
            case 8:  flag = VideoInfo::CS_Sample_Bits_8;  break;
            case 10: flag = VideoInfo::CS_Sample_Bits_10; break;
            case 12: flag = VideoInfo::CS_Sample_Bits_12; break;
            case 14: flag = VideoInfo::CS_Sample_Bits_14; break;
            case 16: flag = VideoInfo::CS_Sample_Bits_16; break;
            default: env->ThrowError("HDRAGCNext: output_bits must be 0, 8, 10, 12, 14, or 16.");
        }
        out_vi.pixel_type = (vi.pixel_type & ~VideoInfo::CS_Sample_Bits_Mask) | flag;
        out_bits_ = bits;
        vi = out_vi;   // GenericVideoFilter exposes base `vi` via GetVideoInfo()

        IScriptEnvironment* env_c = env;
        PClip child_c = child;
        int sbits = src_bits_;
        hdragc::FrameSrc src;
        src.num_frames = child->GetVideoInfo().num_frames;
        src.fetch = [child_c, env_c, sbits](int n, hdragc::PlaneView& y, void*) -> void* {
            PVideoFrame* f = new PVideoFrame(child_c->GetFrame(n, env_c));
            y.data   = (*f)->GetReadPtr(PLANAR_Y);
            y.stride = (*f)->GetPitch(PLANAR_Y);
            y.w      = (*f)->GetRowSize(PLANAR_Y) / ((sbits == 8) ? 1 : 2);
            y.h      = (*f)->GetHeight(PLANAR_Y);
            y.bits   = sbits;
            return f;
        };
        src.release = [](void* token) { delete (PVideoFrame*)token; };
        core_ = new hdragc::HDRAGCCore(params_, std::move(src), src_bits_);
    }

    ~HDRAGCNext() override { delete core_; }

    int __stdcall SetCacheHints(int cachehints, int frame_range) override
    {
        if (cachehints == CACHE_GENERIC)
            frame_range = temporal_radius_ * 2 + 2;
        return 0;
    }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override
    {
        PVideoFrame src = child->GetFrame(n, env);
        PVideoFrame dst = env->NewVideoFrame(vi);

        hdragc::PlaneView in[3] = {
            { src->GetReadPtr(PLANAR_Y),  src->GetPitch(PLANAR_Y),
              src->GetRowSize(PLANAR_Y) / ((src_bits_ == 8) ? 1 : 2), src->GetHeight(PLANAR_Y), src_bits_ },
            { src->GetReadPtr(PLANAR_U),  src->GetPitch(PLANAR_U),
              src->GetRowSize(PLANAR_U) / ((src_bits_ == 8) ? 1 : 2), src->GetHeight(PLANAR_U), src_bits_ },
            { src->GetReadPtr(PLANAR_V),  src->GetPitch(PLANAR_V),
              src->GetRowSize(PLANAR_V) / ((src_bits_ == 8) ? 1 : 2), src->GetHeight(PLANAR_V), src_bits_ },
        };
        hdragc::WritablePlaneView o[3] = {
            { dst->GetWritePtr(PLANAR_Y), dst->GetPitch(PLANAR_Y),
              dst->GetRowSize(PLANAR_Y) / ((out_bits_ == 8) ? 1 : 2), dst->GetHeight(PLANAR_Y), out_bits_ },
            { dst->GetWritePtr(PLANAR_U), dst->GetPitch(PLANAR_U),
              dst->GetRowSize(PLANAR_U) / ((out_bits_ == 8) ? 1 : 2), dst->GetHeight(PLANAR_U), out_bits_ },
            { dst->GetWritePtr(PLANAR_V), dst->GetPitch(PLANAR_V),
              dst->GetRowSize(PLANAR_V) / ((out_bits_ == 8) ? 1 : 2), dst->GetHeight(PLANAR_V), out_bits_ },
        };
        core_->processFrame(in, o, n, nullptr);
        return dst;
    }
};

AVSValue __cdecl Create_HDRAGCNext(AVSValue args, void*, IScriptEnvironment* env)
{
    hdragc::Params p;
    p.strength          = (float)args[1].AsFloat(0.8);
    p.protect           = (float)args[2].AsFloat(0.8);
    p.black             = (float)args[3].AsFloat(0.0);
    p.white             = (float)args[4].AsFloat(0.0);
    p.auto_points       = args[5].AsBool(true);
    p.temporal          = (float)args[6].AsFloat(0.85);
    p.temporal_radius   = args[7].AsInt(4);
    p.temporal_mode     = strcmp(args[8].AsString("iir"), "window") ? 0 : 1;
    p.scene_cut         = (float)args[9].AsFloat(0.35);
    p.scene_cut_low     = (float)args[10].AsFloat(0.20);
    p.scene_cooldown    = args[11].AsInt(3);
    p.detail_gain       = (float)args[12].AsFloat(1.0);
    p.saturation        = (float)args[13].AsFloat(1.0);
    p.auto_saturation   = args[14].AsBool(true);
    p.chroma_mode       = strcmp(args[15].AsString("luma_ratio"), "simple") ? 1 : 0;
    p.luma_ratio_mix    = (float)args[16].AsFloat(0.7);
    p.chroma_softknee   = (float)args[17].AsFloat(0.9);
    p.levels            = args[18].AsInt(4);
    p.local_mix         = (float)args[19].AsFloat(1.0);
    p.shadow_threshold  = (float)args[20].AsFloat(0.35);
    p.mask_gamma        = (float)args[21].AsFloat(2.0);
    p.mask_level        = args[22].AsInt(-1);
    p.output_bits       = args[23].AsInt(0);
    const char* dn      = args[24].AsString("blue");
    p.dither            = !strcmp(dn, "none") ? 0 : !strcmp(dn, "ordered") ? 1 : 2;
    const char* sh      = args[25].AsString("none");
    p.show              = !strcmp(sh, "base") ? 1 : !strcmp(sh, "mask") ? 2 : !strcmp(sh, "lift") ? 3 : 0;
    p.natural           = (float)args[26].AsFloat(0.0);
    p.veil              = (float)args[27].AsFloat(0.0);
    return new HDRAGCNext(args[0].AsClip(), p, env);
}

const char* hdragc_init(IScriptEnvironment* env, const AVS_Linkage* linkage)
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
        "[output_bits]i[dither]s[show]s[natural]f[veil]f",
        Create_HDRAGCNext, nullptr);
    return "HDRAGCNext: adaptive shadow brightening / local tone mapping (dual-core M1)";
}

} // namespace

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
