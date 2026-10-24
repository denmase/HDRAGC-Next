// vs.cpp - VapourSynth adapter (thin shell over hdragc core).
// Mirrors src/avisynth.cpp: parameter semantics, defaults and output
// behavior are identical to the AviSynth+ interface (VapourSynth R81 / API4).
#include <VapourSynth4.h>
#include "core.h"
#include <cstring>
#include <cstdlib>

using namespace hdragc;

namespace {

struct HLGEFData {
    VSNode* node;
    VSVideoInfo vi;
    int src_bits;
    int req_span;
    HDRAGCCore* core;
};

// ---- arg parsing helpers (defaults identical to the AVS interface) ----
static float mgetf(const VSMap* m, const char* k, float def, const VSAPI* vsapi) {
    int e; float v = vsapi->mapGetFloat(m, k, 0, &e);
    return e ? def : v;
}
static int mgeti(const VSMap* m, const char* k, int def, const VSAPI* vsapi) {
    int e; int v = vsapi->mapGetInt(m, k, 0, &e);
    return e ? def : (int)v;
}
static const char* mgets(const VSMap* m, const char* k, const char* def, const VSAPI* vsapi) {
    int e; const char* v = vsapi->mapGetData(m, k, 0, &e);
    return e ? def : v;
}

static const VSFrame* VS_CC hlgefGetFrame(int n, int activationReason, void* instanceData,
                                          void**, VSFrameContext* frameCtx,
                                          VSCore* core, const VSAPI* vsapi)
{
    HLGEFData* d = (HLGEFData*)instanceData;
    if (activationReason == arInitial) {
        // Request every frame the temporal analyzer may read (anchor span):
        // [n - 2*radius, n]. Processing resumes in arAllFramesReady.
        int lo = n - d->req_span; if (lo < 0) lo = 0;
        for (int k = lo; k <= n; k++)
            vsapi->requestFrameFilter(k, d->node, frameCtx);
        return nullptr;
    }
    // arAllFramesReady
    const VSFrame* srcf = vsapi->getFrameFilter(n, d->node, frameCtx);
    VSFrame* dstf = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, srcf, core);

    const int sbits = d->src_bits;
    const int obits = d->vi.format.bitsPerSample;
    PlaneView in[3];
    WritablePlaneView o[3];
    for (int p = 0; p < 3; p++) {
        in[p] = { vsapi->getReadPtr(srcf, p),  (int)vsapi->getStride(srcf, p),
                  vsapi->getFrameWidth(srcf, p), vsapi->getFrameHeight(srcf, p), sbits };
        o[p]  = { vsapi->getWritePtr(dstf, p), (int)vsapi->getStride(dstf, p),
                  vsapi->getFrameWidth(dstf, p), vsapi->getFrameHeight(dstf, p), obits };
    }
    d->core->processFrame(in, o, n, frameCtx);

    vsapi->freeFrame(srcf);
    return dstf;
}

static void VS_CC hlgefFree(void* instanceData, VSCore*, const VSAPI* vsapi) {
    HLGEFData* d = (HLGEFData*)instanceData;
    delete d->core;
    vsapi->freeNode(d->node);
    delete d;
}

static void VS_CC hlgefCreate(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* vsapi)
{
    int err;
    VSNode* node = vsapi->mapGetNode(in, "clip", 0, &err);
    if (err) { vsapi->mapSetError(out, "HDRAGCNext: no input clip"); return; }
    const VSVideoInfo* invi = vsapi->getVideoInfo(node);

    // API4: VSVideoInfo carries VSVideoFormat by value; "format is set" is
    // indicated by colorFamily != cfUndefined.
    const VSVideoFormat* f = &invi->format;
    if (f->colorFamily == cfUndefined) { vsapi->mapSetError(out, "HDRAGCNext: input format must be constant"); vsapi->freeNode(node); return; }
    if (f->colorFamily != cfYUV || f->sampleType != stInteger ||
        f->bitsPerSample < 8 || f->bitsPerSample > 16 || (f->bitsPerSample & 1 && f->bitsPerSample != 8) ||
        f->numPlanes != 3) {
        vsapi->mapSetError(out, "HDRAGCNext: input must be integer planar YUV (8/10/12/14/16-bit)");
        vsapi->freeNode(node); return;
    }

    Params p;
    p.strength         = mgetf(in, "gain",               0.8f, vsapi);
    p.protect          = mgetf(in, "protect_highlights", 0.8f, vsapi);
    p.black            = mgetf(in, "black_point",        0.0f, vsapi);
    p.white            = mgetf(in, "white_point",        0.0f, vsapi);
    p.auto_points      = mgeti(in, "auto_points",        1, vsapi) != 0;
    p.temporal         = mgetf(in, "temporal",           0.85f, vsapi);
    p.temporal_radius  = mgeti(in, "temporal_radius",    4, vsapi);
    p.temporal_mode    = strcmp(mgets(in, "temporal_mode", "iir", vsapi), "window") ? 0 : 1;
    p.scene_cut        = mgetf(in, "scene_cut",          0.35f, vsapi);
    p.scene_cut_low    = mgetf(in, "scene_cut_low",      0.20f, vsapi);
    p.scene_cooldown   = mgeti(in, "scene_cooldown",     3, vsapi);
    p.detail_gain      = mgetf(in, "detail_gain",        1.0f, vsapi);
    p.saturation       = mgetf(in, "saturation",         1.0f, vsapi);
    p.auto_saturation  = mgeti(in, "auto_saturation",    1, vsapi) != 0;
    p.chroma_mode      = strcmp(mgets(in, "chroma_mode", "luma_ratio", vsapi), "simple") ? 1 : 0;
    p.luma_ratio_mix   = mgetf(in, "luma_ratio_mix",     0.7f, vsapi);
    p.chroma_softknee  = mgetf(in, "chroma_softknee",    0.9f, vsapi);
    p.levels           = mgeti(in, "levels",             4, vsapi);
    p.local_mix        = mgetf(in, "local_mix",          1.0f, vsapi);
    p.shadow_threshold = mgetf(in, "shadow_threshold",   0.35f, vsapi);
    p.mask_gamma       = mgetf(in, "mask_gamma",         2.0f, vsapi);
    p.mask_level       = mgeti(in, "mask_level",         -1, vsapi);
    int out_bits       = mgeti(in, "output_bits",        0, vsapi);
    const char* dn     = mgets(in, "dither",             "blue", vsapi);
    p.dither           = !strcmp(dn, "none") ? 0 : !strcmp(dn, "ordered") ? 1 : 2;
    const char* sh     = mgets(in, "show",               "none", vsapi);
    p.show             = !strcmp(sh, "base") ? 1 : !strcmp(sh, "mask") ? 2 : !strcmp(sh, "lift") ? 3 : 0;
    p.natural          = mgetf(in, "natural",            0.0f, vsapi);
    p.veil             = mgetf(in, "veil",               0.0f, vsapi);

    HLGEFData* d = new HLGEFData;
    d->node = node;
    d->vi = *invi;
    d->src_bits = f->bitsPerSample;
    if (out_bits != 0 && out_bits != d->src_bits) {
        if (out_bits < 8 || out_bits > 16 || out_bits == 9 || out_bits == 11 ||
            out_bits == 13 || out_bits == 15) {
            vsapi->mapSetError(out, "HDRAGCNext: output_bits must be 0, 8, 10, 12, 14, or 16");
            delete d; vsapi->freeNode(node); return;
        }
        VSVideoFormat fmt;
        if (!vsapi->queryVideoFormat(&fmt, cfYUV, stInteger, out_bits,
                                     f->subSamplingW, f->subSamplingH, core)) {
            vsapi->mapSetError(out, "HDRAGCNext: queryVideoFormat failed for requested output_bits");
            delete d; vsapi->freeNode(node); return;
        }
        d->vi.format = fmt;
    }

    VSNode* node_c = node;
    int sbits = d->src_bits;
    FrameSrc src;
    src.num_frames = invi->numFrames;
    src.fetch = [vsapi, node_c, sbits](int n, PlaneView& y, void* ctx) -> void* {
        const VSFrame* fr = vsapi->getFrameFilter(n, node_c, (VSFrameContext*)ctx);
        y.data   = vsapi->getReadPtr(fr, 0);
        y.stride = vsapi->getStride(fr, 0);
        y.w      = vsapi->getFrameWidth(fr, 0);
        y.h      = vsapi->getFrameHeight(fr, 0);
        y.bits   = sbits;
        return (void*)fr;   // token: frame stays alive until release
    };
    src.release = [vsapi](void* token) { vsapi->freeFrame((const VSFrame*)token); };

    d->req_span = p.temporal_radius * 2;
    d->core = new HDRAGCCore(p, std::move(src), d->src_bits);
    vsapi->createVideoFilter(out, "HDRAGCNext", &d->vi, hlgefGetFrame, hlgefFree,
                             fmParallel, nullptr, 0, d, core);
}

} // namespace

#define HLGEF_VS_EXPORT extern "C" __declspec(dllexport)
HLGEF_VS_EXPORT void VS_CC VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi)
{
    vspapi->configPlugin("com.hdragc.next", "hlgef", "HDRAGC-Next",
                         VS_MAKE_VERSION(1, 0), VS_MAKE_VERSION(4, 0), 0, plugin);
    vspapi->registerFunction("HDRAGCNext",
        "clip:vnode;"
        "gain:float:opt;protect_highlights:float:opt;"
        "black_point:float:opt;white_point:float:opt;auto_points:int:opt;"
        "temporal:float:opt;temporal_radius:int:opt;temporal_mode:data:opt;"
        "scene_cut:float:opt;scene_cut_low:float:opt;scene_cooldown:int:opt;"
        "detail_gain:float:opt;saturation:float:opt;auto_saturation:int:opt;"
        "chroma_mode:data:opt;luma_ratio_mix:float:opt;chroma_softknee:float:opt;"
        "levels:int:opt;local_mix:float:opt;shadow_threshold:float:opt;"
        "mask_gamma:float:opt;mask_level:int:opt;"
        "output_bits:int:opt;dither:data:opt;show:data:opt;"
        "natural:float:opt;veil:float:opt",
        "clip:vnode;",
        hlgefCreate, nullptr, plugin);
}
