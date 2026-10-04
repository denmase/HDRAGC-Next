// RawSourceYV12 - minimal raw YV12 file reader plugin (proof-of-concept input)
#include <avisynth.h>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
  #define RAWSRC_HIDDEN
  #define RAWSRC_EXPORT __declspec(dllexport)
#else
  #define RAWSRC_HIDDEN __attribute__((visibility("hidden")))
  #define RAWSRC_EXPORT __attribute__((visibility("default")))
#endif

RAWSRC_HIDDEN
const AVS_Linkage* AVS_linkage = nullptr;

class RawSourceYV12 : public IClip
{
    VideoInfo vi_;
    FILE* fp_;
    int frame_size_;
public:
    RawSourceYV12(const char* path, int w, int h, int length, IScriptEnvironment* env)
        : fp_(nullptr)
    {
        memset(&vi_, 0, sizeof(vi_));
        vi_.width = w; vi_.height = h;
        vi_.fps_numerator = 24; vi_.fps_denominator = 1;
        vi_.num_frames = length;
        vi_.pixel_type = VideoInfo::CS_YV12;
        frame_size_ = w * h * 3 / 2;
        fp_ = fopen(path, "rb");
        if (!fp_) env->ThrowError("RawSourceYV12: cannot open %s", path);
        fseek(fp_, 0, SEEK_END);
        long sz = ftell(fp_);
        if (sz < (long)frame_size_ * length)
            env->ThrowError("RawSourceYV12: file too small (%ld < %ld)", sz, (long)frame_size_ * length);
    }
    ~RawSourceYV12() override { if (fp_) fclose(fp_); }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override
    {
        PVideoFrame f = env->NewVideoFrame(vi_);
        fseek(fp_, (long)n * frame_size_, SEEK_SET);
        // PER-BARIS dengan pitch masing-masing plane (pitch di-align AviSynth+,
        // TIDAK sama dengan width!) dan U->PLANAR_U, V->PLANAR_V (nama logis;
        // jebakan "YV12 V dulu" hanya berlaku layout memori packed).
        const int w = vi_.width, h = vi_.height;
        unsigned char* y = f->GetWritePtr(PLANAR_Y);
        for (int row = 0; row < h; row++)
            if (fread(y + (size_t)row * f->GetPitch(PLANAR_Y), 1, w, fp_) != (size_t)w)
                env->ThrowError("RawSourceYV12: short read");
        unsigned char* u = f->GetWritePtr(PLANAR_U);
        unsigned char* v = f->GetWritePtr(PLANAR_V);
        for (int row = 0; row < h / 2; row++)
            if (fread(u + (size_t)row * f->GetPitch(PLANAR_U), 1, w / 2, fp_) != (size_t)(w / 2))
                env->ThrowError("RawSourceYV12: short read");
        for (int row = 0; row < h / 2; row++)
            if (fread(v + (size_t)row * f->GetPitch(PLANAR_V), 1, w / 2, fp_) != (size_t)(w / 2))
                env->ThrowError("RawSourceYV12: short read");
        return f;
    }
    void __stdcall GetAudio(void* buf, int64_t start, int64_t count, IScriptEnvironment* env) override
    { (void)buf; (void)start; (void)count; (void)env; }
    bool __stdcall GetParity(int n) override { return false; }
    int __stdcall SetCacheHints(int cachehints, int frame_range) override { return 0; }
    const VideoInfo& __stdcall GetVideoInfo() override { return vi_; }
};

static AVSValue __cdecl Create_RawSourceYV12(AVSValue args, void*, IScriptEnvironment* env)
{
    return new RawSourceYV12(args[0].AsString(), args[1].AsInt(), args[2].AsInt(),
                             args[3].AsInt(1), env);
}

static const char* rawsrc_init(IScriptEnvironment* env, const AVS_Linkage* linkage)
{
    AVS_linkage = linkage;
    env->AddFunction("RawSourceYV12", "s[width]i[height]i[length]i",
                     Create_RawSourceYV12, nullptr);
    return "RawSourceYV12: raw YV12 reader (proof input)";
}

extern "C" RAWSRC_EXPORT
const char* AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* linkage)
{ return rawsrc_init(env, linkage); }

extern "C" RAWSRC_EXPORT
const char* AvisynthPluginInit2(IScriptEnvironment* env)
{ return rawsrc_init(env, nullptr); }
